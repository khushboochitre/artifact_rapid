//===--------- PPCPreEmitPeephole.cpp - Late peephole optimizations -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A pre-emit peephole for catching opportunities introduced by late passes such
// as MachineBlockPlacement.
//
//===----------------------------------------------------------------------===//

#include "PPC.h"
#include "PPCInstrInfo.h"
#include "PPCSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "ppc-pre-emit-peephole"

STATISTIC(NumRRConvertedInPreEmit,
          "Number of r+r instructions converted to r+i in pre-emit peephole");
STATISTIC(NumRemovedInPreEmit,
          "Number of instructions deleted in pre-emit peephole");
STATISTIC(NumberOfSelfCopies,
          "Number of self copy instructions eliminated");

static cl::opt<bool>
RunPreEmitPeephole("ppc-late-peephole", cl::Hidden, cl::init(true),
                   cl::desc("Run pre-emit peephole optimizations."));

namespace {
  class PPCPreEmitPeephole : public MachineFunctionPass {
  public:
    static char ID;
    PPCPreEmitPeephole() : MachineFunctionPass(ID) {
      initializePPCPreEmitPeepholePass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      MachineFunctionPass::getAnalysisUsage(AU);
    }

    MachineFunctionProperties getRequiredProperties() const override {
      return MachineFunctionProperties().set(
          MachineFunctionProperties::Property::NoVRegs);
    }

    // This function removes any redundant load immediates. It has two level
    // loops - The outer loop finds the load immediates BBI that could be used
    // to replace following redundancy. The inner loop scans instructions that
    // after BBI to find redundancy and update kill/dead flags accordingly. If
    // AfterBBI is the same as BBI, it is redundant, otherwise any instructions
    // that modify the def register of BBI would break the scanning.
    // DeadOrKillToUnset is a pointer to the previous operand that had the
    // kill/dead flag set. It keeps track of the def register of BBI, the use
    // registers of AfterBBIs and the def registers of AfterBBIs.
    bool removeRedundantLIs(MachineBasicBlock &MBB,
                            const TargetRegisterInfo *TRI) {
      LLVM_DEBUG(dbgs() << "Remove redundant load immediates from MBB:\n";
                 MBB.dump(); dbgs() << "\n");

      DenseSet<MachineInstr *> InstrsToErase;
      for (auto BBI = MBB.instr_begin(); BBI != MBB.instr_end(); ++BBI) {
        // Skip load immediate that is marked to be erased later because it
        // cannot be used to replace any other instructions.
        if (InstrsToErase.find(&*BBI) != InstrsToErase.end())
          continue;
        // Skip non-load immediate.
        unsigned Opc = BBI->getOpcode();
        if (Opc != PPC::LI && Opc != PPC::LI8 && Opc != PPC::LIS &&
            Opc != PPC::LIS8)
          continue;
        // Skip load immediate, where the operand is a relocation (e.g., $r3 =
        // LI target-flags(ppc-lo) %const.0).
        if (!BBI->getOperand(1).isImm())
          continue;
        assert(BBI->getOperand(0).isReg() &&
               "Expected a register for the first operand");

        LLVM_DEBUG(dbgs() << "Scanning after load immediate: "; BBI->dump(););

        Register Reg = BBI->getOperand(0).getReg();
        int64_t Imm = BBI->getOperand(1).getImm();
        MachineOperand *DeadOrKillToUnset = nullptr;
        if (BBI->getOperand(0).isDead()) {
          DeadOrKillToUnset = &BBI->getOperand(0);
          LLVM_DEBUG(dbgs() << " Kill flag of " << *DeadOrKillToUnset
                            << " from load immediate " << *BBI
                            << " is a unsetting candidate\n");
        }
        // This loop scans instructions after BBI to see if there is any
        // redundant load immediate.
        for (auto AfterBBI = std::next(BBI); AfterBBI != MBB.instr_end();
             ++AfterBBI) {
          // Track the operand that kill Reg. We would unset the kill flag of
          // the operand if there is a following redundant load immediate.
          int KillIdx = AfterBBI->findRegisterUseOperandIdx(Reg, true, TRI);
          if (KillIdx != -1) {
            assert(!DeadOrKillToUnset && "Shouldn't kill same register twice");
            DeadOrKillToUnset = &AfterBBI->getOperand(KillIdx);
            LLVM_DEBUG(dbgs()
                       << " Kill flag of " << *DeadOrKillToUnset << " from "
                       << *AfterBBI << " is a unsetting candidate\n");
          }

          if (!AfterBBI->modifiesRegister(Reg, TRI))
            continue;
          // Finish scanning because Reg is overwritten by a non-load
          // instruction.
          if (AfterBBI->getOpcode() != Opc)
            break;
          assert(AfterBBI->getOperand(0).isReg() &&
                 "Expected a register for the first operand");
          // Finish scanning because Reg is overwritten by a relocation or a
          // different value.
          if (!AfterBBI->getOperand(1).isImm() ||
              AfterBBI->getOperand(1).getImm() != Imm)
            break;

          // It loads same immediate value to the same Reg, which is redundant.
          // We would unset kill flag in previous Reg usage to extend live range
          // of Reg first, then remove the redundancy.
          if (DeadOrKillToUnset) {
            LLVM_DEBUG(dbgs()
                       << " Unset dead/kill flag of " << *DeadOrKillToUnset
                       << " from " << *DeadOrKillToUnset->getParent());
            if (DeadOrKillToUnset->isDef())
              DeadOrKillToUnset->setIsDead(false);
            else
              DeadOrKillToUnset->setIsKill(false);
          }
          DeadOrKillToUnset =
              AfterBBI->findRegisterDefOperand(Reg, true, true, TRI);
          if (DeadOrKillToUnset)
            LLVM_DEBUG(dbgs()
                       << " Dead flag of " << *DeadOrKillToUnset << " from "
                       << *AfterBBI << " is a unsetting candidate\n");
          InstrsToErase.insert(&*AfterBBI);
          LLVM_DEBUG(dbgs() << " Remove redundant load immediate: ";
                     AfterBBI->dump());
        }
      }

      for (MachineInstr *MI : InstrsToErase) {
        MI->eraseFromParent();
      }
      NumRemovedInPreEmit += InstrsToErase.size();
      return !InstrsToErase.empty();
    }

    bool runOnMachineFunction(MachineFunction &MF) override {
      if (skipFunction(MF.getFunction()) || !RunPreEmitPeephole)
        return false;
      bool Changed = false;
      const PPCInstrInfo *TII = MF.getSubtarget<PPCSubtarget>().getInstrInfo();
      const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
      SmallVector<MachineInstr *, 4> InstrsToErase;
      for (MachineBasicBlock &MBB : MF) {
        Changed |= removeRedundantLIs(MBB, TRI);
        for (MachineInstr &MI : MBB) {
          unsigned Opc = MI.getOpcode();
          // Detect self copies - these can result from running AADB.
          if (PPCInstrInfo::isSameClassPhysRegCopy(Opc)) {
            const MCInstrDesc &MCID = TII->get(Opc);
            if (MCID.getNumOperands() == 3 &&
                MI.getOperand(0).getReg() == MI.getOperand(1).getReg() &&
                MI.getOperand(0).getReg() == MI.getOperand(2).getReg()) {
              NumberOfSelfCopies++;
              LLVM_DEBUG(dbgs() << "Deleting self-copy instruction: ");
              LLVM_DEBUG(MI.dump());
              InstrsToErase.push_back(&MI);
              continue;
            }
            else if (MCID.getNumOperands() == 2 &&
                     MI.getOperand(0).getReg() == MI.getOperand(1).getReg()) {
              NumberOfSelfCopies++;
              LLVM_DEBUG(dbgs() << "Deleting self-copy instruction: ");
              LLVM_DEBUG(MI.dump());
              InstrsToErase.push_back(&MI);
              continue;
            }
          }
          MachineInstr *DefMIToErase = nullptr;
          if (TII->convertToImmediateForm(MI, &DefMIToErase)) {
            Changed = true;
            NumRRConvertedInPreEmit++;
            LLVM_DEBUG(dbgs() << "Converted instruction to imm form: ");
            LLVM_DEBUG(MI.dump());
            if (DefMIToErase) {
              InstrsToErase.push_back(DefMIToErase);
            }
          }
        }

        // Eliminate conditional branch based on a constant CR bit by
        // CRSET or CRUNSET. We eliminate the conditional branch or
        // convert it into an unconditional branch. Also, if the CR bit
        // is not used by other instructions, we eliminate CRSET as well.
        auto I = MBB.getFirstInstrTerminator();
        if (I == MBB.instr_end())
          continue;
        MachineInstr *Br = &*I;
        if (Br->getOpcode() != PPC::BC && Br->getOpcode() != PPC::BCn)
          continue;
        MachineInstr *CRSetMI = nullptr;
        Register CRBit = Br->getOperand(0).getReg();
        unsigned CRReg = getCRFromCRBit(CRBit);
        bool SeenUse = false;
        MachineBasicBlock::reverse_iterator It = Br, Er = MBB.rend();
        for (It++; It != Er; It++) {
          if (It->modifiesRegister(CRBit, TRI)) {
            if ((It->getOpcode() == PPC::CRUNSET ||
                 It->getOpcode() == PPC::CRSET) &&
                It->getOperand(0).getReg() == CRBit)
              CRSetMI = &*It;
            break;
          }
          if (It->readsRegister(CRBit, TRI))
            SeenUse = true;
        }
        if (!CRSetMI) continue;

        unsigned CRSetOp = CRSetMI->getOpcode();
        if ((Br->getOpcode() == PPC::BCn && CRSetOp == PPC::CRSET) ||
            (Br->getOpcode() == PPC::BC  && CRSetOp == PPC::CRUNSET)) {
          // Remove this branch since it cannot be taken.
          InstrsToErase.push_back(Br);
          MBB.removeSuccessor(Br->getOperand(1).getMBB());
        }
        else {
          // This conditional branch is always taken. So, remove all branches
          // and insert an unconditional branch to the destination of this.
          MachineBasicBlock::iterator It = Br, Er = MBB.end();
          for (; It != Er; It++) {
            if (It->isDebugInstr()) continue;
            assert(It->isTerminator() && "Non-terminator after a terminator");
            InstrsToErase.push_back(&*It);
          }
          if (!MBB.isLayoutSuccessor(Br->getOperand(1).getMBB())) {
            ArrayRef<MachineOperand> NoCond;
            TII->insertBranch(MBB, Br->getOperand(1).getMBB(), nullptr,
                              NoCond, Br->getDebugLoc());
          }
          for (auto &Succ : MBB.successors())
            if (Succ != Br->getOperand(1).getMBB()) {
              MBB.removeSuccessor(Succ);
              break;
            }
        }

        // If the CRBit is not used by another instruction, we can eliminate
        // CRSET/CRUNSET instruction.
        if (!SeenUse) {
          // We need to check use of the CRBit in successors.
          for (auto &SuccMBB : MBB.successors())
            if (SuccMBB->isLiveIn(CRBit) || SuccMBB->isLiveIn(CRReg)) {
              SeenUse = true;
              break;
            }
          if (!SeenUse)
            InstrsToErase.push_back(CRSetMI);
        }
      }
      for (MachineInstr *MI : InstrsToErase) {
        LLVM_DEBUG(dbgs() << "PPC pre-emit peephole: erasing instruction: ");
        LLVM_DEBUG(MI->dump());
        MI->eraseFromParent();
        NumRemovedInPreEmit++;
      }
      return Changed;
    }
  };
}

INITIALIZE_PASS(PPCPreEmitPeephole, DEBUG_TYPE, "PowerPC Pre-Emit Peephole",
                false, false)
char PPCPreEmitPeephole::ID = 0;

FunctionPass *llvm::createPPCPreEmitPeepholePass() {
  return new PPCPreEmitPeephole();
}