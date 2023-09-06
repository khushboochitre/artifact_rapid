#include "llvm/Transforms/Scalar/Allocator2k.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "allocator2k"

static bool isInstrumentFuncDef = false;
static Function *isNoAliasRTCheckFunc;
static bool isReturnSizeFuncDefAdded = false;
static Function *returnSizeFunc;

namespace {
class Allocator2kLegacyPass : public FunctionPass {
  const TargetMachine *TM = nullptr;

public:
  static char ID; // Pass identification, replacement for typeid..

  Allocator2kLegacyPass() : FunctionPass(ID) {
    initializeAllocator2kLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
  }

  bool runOnFunction(Function &F) override;
};

}


char Allocator2kLegacyPass::ID = 0;
FunctionPass *llvm::createAllocator2kPass() { return new Allocator2kLegacyPass(); }

INITIALIZE_PASS_BEGIN(Allocator2kLegacyPass, DEBUG_TYPE,
                      "Allocator2k instrumentation pass", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(Allocator2kLegacyPass, DEBUG_TYPE,
                    "Allocator2k instrumentation pass", false, false)



static bool isSafeAlloca(const AllocaInst *AllocaPtr, const TargetLibraryInfo &TLI)
{
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 8> WorkList;
  WorkList.push_back(AllocaPtr);

  while (!WorkList.empty()) {
    const Value *V = WorkList.pop_back_val();
    for (const Use &UI : V->uses()) {
      auto I = cast<const Instruction>(UI.getUser());
      assert(V == UI.get());

      switch (I->getOpcode()) {
			case Instruction::Load:
				break;
			case Instruction::VAArg:
				break;

      case Instruction::Store:
        if (V == I->getOperand(0)) {
					return false;
        }
        break;

      case Instruction::Ret:
        return false;

      case Instruction::Call:
      case Instruction::Invoke: {
        ImmutableCallSite CS(I);

        if (I->isLifetimeStartOrEnd())
          continue;
        if (dyn_cast<MemIntrinsic>(I)) {
        	continue;
        }

				LibFunc Func;
    		if (TLI.getLibFunc(ImmutableCallSite(CS), Func)) {
					continue;
				}

        ImmutableCallSite::arg_iterator B = CS.arg_begin(), E = CS.arg_end();
        for (ImmutableCallSite::arg_iterator A = B; A != E; ++A)
          if (A->get() == V)
            if (!(CS.doesNotCapture(A - B) && (CS.doesNotAccessMemory(A - B) ||
                                               CS.doesNotAccessMemory()))) {
              return false;
            }
        continue;
      }

      default:
        if (Visited.insert(I).second)
          WorkList.push_back(cast<const Instruction>(I));
      }
    }
  }

  return true;
}

static uint64_t getAllocaSizeInBytes(const AllocaInst &AI)
{
	uint64_t ArraySize = 1;
  if (AI.isArrayAllocation()) {
    const ConstantInt *CI = dyn_cast<ConstantInt>(AI.getArraySize());
    assert(CI && "non-constant array size");
    ArraySize = CI->getZExtValue();
  }
  Type *Ty = AI.getAllocatedType();
  uint64_t SizeInBytes =
      AI.getModule()->getDataLayout().getTypeAllocSize(Ty);
  return SizeInBytes * ArraySize;
}

static Value *getAllocaSize(AllocaInst *AI)
{
	assert(!AI->isStaticAlloca());
	Type *Ty = AI->getAllocatedType();
  uint64_t SizeInBytes = AI->getModule()->getDataLayout().getTypeAllocSize(Ty);
	IRBuilder<> IRB(AI->getParent());
	IRB.SetInsertPoint(AI);
	Value *ArrSz = AI->getArraySize();
	if (ArrSz->getType()->isPointerTy()) {
		ArrSz = IRB.CreatePtrToInt(ArrSz, IRB.getInt64Ty());
	}
	else if (ArrSz->getType() != IRB.getInt64Ty()) {
		ArrSz = IRB.CreateZExt(ArrSz, IRB.getInt64Ty());
	}
	Value *Sz = IRB.CreateMul(ArrSz, ConstantInt::get(IRB.getInt64Ty(), SizeInBytes));
	return Sz;
}

static void allocaToMalloc(Function *F, AllocaInst *AI)
{
	auto M = F->getParent();
	auto Int32Ty = Type::getInt32Ty(M->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());
	size_t alignment = 0;
	Value *Size;

	alignment = AI->getAlignment();

	if (AI->isStaticAlloca()) {
		size_t Sz = getAllocaSizeInBytes(*AI);
		if (Sz <= 64) {
			AI->setAlignment(MaybeAlign(64));
			return;
		}
		Size = ConstantInt::get(Int32Ty, Sz);
		errs() << "AI to Malloc:: " << *AI << " Size: " << Sz <<  "\n";
	}
	else {
		AI->setAlignment(MaybeAlign(64));
  	//Size = getAllocaSize(AI);
		// FIXME::
		return;
	}


  Instruction *Entry = dyn_cast<Instruction>(F->begin()->getFirstInsertionPt());
	IRBuilder<> IRB(Entry);
	auto Fn = M->getOrInsertFunction("aligned_alloc", AI->getType(), Size->getType(), Int32Ty);
	auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Int32Ty, alignment), Size});
  AI->replaceAllUsesWith(Call);
	AI->eraseFromParent();

	Fn = M->getOrInsertFunction("free", RetTy, Call->getType());

	for (auto &BB : *F) {
		auto Term = BB.getTerminator();
		if (isa<ReturnInst>(Term)) {
			IRB.SetInsertPoint(Term);
			IRB.CreateCall(Fn, {Call});
		}
	}
}

static bool doesExternalCall(Function &F) {
	for (auto &BB : F) {
		for (auto &I : BB) {
			CallBase *CB = dyn_cast<CallBase>(&I);
			if (CB) {
        ImmutableCallSite CS(CB);
				if (!CS.doesNotAccessMemory()) {
					return true;
				}
			}
		}
	}
	return false;
}

static Value* getGlobalUL(Module *M)
{
  const char *VarName = "je___max_global_addr";
  auto MaxGlobalAddrVar =
      dyn_cast_or_null<GlobalVariable>(M->getNamedValue(VarName));

  Type *VarTy = Type::getInt64Ty(M->getContext());

  if (!MaxGlobalAddrVar) {
    MaxGlobalAddrVar = new GlobalVariable(
        *M, VarTy, false, GlobalValue::ExternalLinkage, nullptr, VarName);
  }
  return MaxGlobalAddrVar;
}

static Value* getGlobalLimit(Function *F, BasicBlock *BB)
{
	IRBuilder<> IRB(BB->getFirstNonPHI());
	auto Limit = getGlobalUL(F->getParent());
	return IRB.CreateLoad(IRB.getInt64Ty(), Limit);
}

static void setGlobalLimit(Function &F)
{
  Instruction *Entry = dyn_cast<Instruction>(F.begin()->getFirstInsertionPt());
	IRBuilder<> IRB(Entry);
	auto GlobalLimit = getGlobalUL(F.getParent());
	IRB.CreateStore(ConstantInt::get(IRB.getInt64Ty(), 0xFFFFFFFF), GlobalLimit);
}

static void switchStack(Function &F) {
	if (F.getName() == "main") {
		/*F.addFnAttr("switch_stack");
		auto Int32Ty = Type::getInt32Ty(F.getParent()->getContext());
		auto VoidTy = Type::getVoidTy(F.getParent()->getContext());
		auto Fn = F.getParent()->getOrInsertFunction("exit", VoidTy, Int32Ty);
		for (auto &BB : F) {
			auto Term = BB.getTerminator();
			auto Ret = dyn_cast<ReturnInst>(Term);
			if (Ret) {
				IRBuilder<> IRB(Term);
				IRB.CreateCall(Fn, {Ret->getReturnValue()});
			}
		}*/
		setGlobalLimit(F);
	}
	
}

static bool run(Function &F, LoopInfo &LI, const TargetLibraryInfo &TLI) {
	switchStack(F);
	/*if (LI.empty() && !doesExternalCall(F)) {
		return false;
	}
	//return false;

	DenseSet<AllocaInst*> AISet;
	for (auto &BB : F) {
		for (auto &I : BB) {
			AllocaInst *AI = dyn_cast<AllocaInst>(&I);
			if (AI && !isSafeAlloca(AI, TLI)) {
				AISet.insert(AI);
			}
		}
	}

	for (auto AI : AISet) {
		allocaToMalloc(&F, AI);
	}*/

  return true;
}

static void lowerAlignment(Function &F, CallInst *CI) {
	IRBuilder<> IRB(CI);
	auto Arg0 = CI->getArgOperand(0);
	auto Base = Arg0->stripPointerCasts();
	if (isa<Constant>(Base)) {
		auto AlignVal = ConstantInt::get(IRB.getInt64Ty(), 0xFFFFFFFF);
		CI->replaceAllUsesWith(AlignVal);
		CI->eraseFromParent();
		return;
	}

	Value *CmpVal = getGlobalLimit(&F, CI->getParent());
	auto BaseInt = IRB.CreatePtrToInt(Arg0, IRB.getInt64Ty());
	assert(BaseInt->getType() == CmpVal->getType());

	auto Greater = IRB.CreateICmp(ICmpInst::ICMP_UGE, BaseInt, CmpVal);
	        
	Instruction *SplitBefore = CI;
	assert(SplitBefore);

	Instruction *ThenTerm = SplitBlockAndInsertIfThen(Greater, SplitBefore, false);
	BasicBlock *LastBlock = dyn_cast<BranchInst>(ThenTerm->getParent()->getTerminator())->getSuccessor(0);

	IRB.SetInsertPoint(ThenTerm);
	Value *ResAND = IRB.CreateAnd(BaseInt, ConstantInt::get(IRB.getInt64Ty(), 0xFFFF00000000ULL));
	Value *AddrPtr = IRB.CreateIntToPtr(ResAND, PointerType::getUnqual(IRB.getInt64Ty()));
	Value *AlignVal2 = IRB.CreateLoad(IRB.getInt64Ty(), AddrPtr);

	IRB.SetInsertPoint(SplitBefore);
	PHINode *AlignVal = IRB.CreatePHI(IRB.getInt64Ty(), 2);
	assert(AlignVal);
	AlignVal->addIncoming(AlignVal2, ThenTerm->getParent());
	AlignVal->addIncoming(CmpVal, ThenTerm->getParent()->getUniquePredecessor());
	CI->replaceAllUsesWith(AlignVal);
	CI->eraseFromParent();
}

static void lowerRuntimeCheck(Function &F, CallInst *CI) {
	IRBuilder<> IRB(CI);
	auto Base1 = CI->getArgOperand(0);
	auto Base2 = CI->getArgOperand(1);
	auto Size = CI->getArgOperand(2);

	Value *O1 = IRB.CreatePtrToInt(Base1, IRB.getInt64Ty());
	Value *O2 = IRB.CreatePtrToInt(Base2, IRB.getInt64Ty());

	assert(O1 && O2);
	Value *ResXOR = IRB.CreateXor(O1, O2);
	Value *RTCall = IRB.CreateICmp(ICmpInst::ICMP_UGE, ResXOR, Size);
	CI->replaceAllUsesWith(RTCall);
	CI->eraseFromParent();
}

static void lowerIntrinsics(Function *F) {
	Module *M = F->getParent();

  DenseSet<CallInst*> AlignIntr;
  DenseSet<CallInst*> CheckIntr;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
    	Instruction *BI = &*I;
			IntrinsicInst *Intrin = dyn_cast<IntrinsicInst>(BI);
			if (Intrin) {
				auto ID = Intrin->getIntrinsicID();
				if (ID == Intrinsic::alignment2k) {
					AlignIntr.insert(Intrin);
				}
				else if (ID == Intrinsic::noalias_check) {
					CheckIntr.insert(Intrin);
				}
			}
		}
	}
	for (auto Align : AlignIntr) {
		lowerAlignment(*F, Align);
	}
	for (auto Check : CheckIntr) {
		lowerRuntimeCheck(*F, Check);
	}
}

static void convertDynamicCheckIntrToCall(Function *F) {
	Module *M = F->getParent();
  	IntegerType *Int1Type = Type::getInt1Ty(M->getContext());
  	IntegerType *Int64Type = Type::getInt64Ty(M->getContext());
  	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);

  	if(!isInstrumentFuncDef) {
  		vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
		ArgTypes.push_back(CharStarType);
		ArgTypes.push_back(Int64Type);
		FunctionType *FTy = FunctionType::get(Int1Type, ArgTypes, false);
		isNoAliasRTCheckFunc = Function::Create(FTy,
				GlobalValue::ExternalLinkage, "isNoAliasRTCheck", M);
		isNoAliasRTCheckFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadOnly);
		isInstrumentFuncDef = true;
  	}

  	if(!isReturnSizeFuncDefAdded) {
  		vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
		FunctionType *FTy = FunctionType::get(Int64Type, ArgTypes, false);
		returnSizeFunc = Function::Create(FTy,
			GlobalValue::ExternalLinkage, "return_size", M);
		returnSizeFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadOnly);
		isReturnSizeFuncDefAdded = true;
  	}

		lowerIntrinsics(F);
  	//Collects block that consists of noalias Intrinsic.
  	DenseSet<BasicBlock *> BlocksWithIntrSet;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
            if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
							assert(0);
            	BasicBlock *B = &*BB;
            	BlocksWithIntrSet.insert(B);
            	break;
            }
      	}
    }

    for(auto BB: BlocksWithIntrSet) {
			auto GlobalLimit = getGlobalLimit(F, BB);
    	//Collects unique first operands.
    	DenseMap<Value *, Instruction *> ReturnSizeLoc;
    	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
            if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
            	if(!ReturnSizeLoc.count(CI->getOperand(0))) {
            		ReturnSizeLoc[CI->getOperand(0)] = BI;
            	}
            }
      	}
      	
      	DenseMap<Value *, Value *> PtrToIntMap1;
      	DenseMap<Value *, Value *> ReturnSizeMap;
		DenseSet<Instruction *> IntrinsicSet;
		//Load alignment for the first operand.
      	for(auto Pair: ReturnSizeLoc) {
      		assert(Pair.second);
      		IRBuilder<> IRB1(Pair.second);
      		Value *O1 = NULL;

	        if(PtrToIntMap1.count(Pair.first)) {
	            O1 = PtrToIntMap1[Pair.first];
	        }
	        else {
	        	if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Pair.first)) {
	        		assert(CE->isCast());
	        		O1 = CE->getOperand(0);
	        	}
	        	else {
	        		O1 = IRB1.CreatePtrToInt(Pair.first, IRB1.getInt64Ty());
	        	}
	            PtrToIntMap1[Pair.first] = O1;
	        }
	        assert(O1);
	        //Value *CmpVal = ConstantInt::get(IRB1.getInt64Ty(), 0xFFFFFFFF);
					Value *CmpVal = GlobalLimit;
	        assert(O1->getType() == CmpVal->getType());
	        //Check if the address is less than 32 bits.
	        auto Greater = IRB1.CreateICmp(ICmpInst::ICMP_UGE, O1, CmpVal);
	        
	        Instruction *SplitBefore = NULL;
	        if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Pair.first)) {
	        	SplitBefore = dyn_cast<Instruction>(Pair.second);
	        }
	        else {
	        	SplitBefore = dyn_cast<Instruction>(Greater)->getNextNode();
	        }
	        assert(SplitBefore);
	        
	       	Instruction *ThenTerm = SplitBlockAndInsertIfThen(Greater, SplitBefore, false);
	        BasicBlock *LastBlock = dyn_cast<BranchInst>(ThenTerm->getParent()->getTerminator())->getSuccessor(0);
	        
	        IRB1.SetInsertPoint(ThenTerm);
	        Value *ResAND = IRB1.CreateAnd(O1, ConstantInt::get(IRB1.getInt64Ty(), 0xFFFF00000000ULL));
	        Value *AddrPtr = IRB1.CreateIntToPtr(ResAND, PointerType::getUnqual(IRB1.getInt64Ty()));
  			Value *AlignVal2 = IRB1.CreateLoad(IRB1.getInt64Ty(), AddrPtr);

		    IRB1.SetInsertPoint(SplitBefore);
		    Value *AlignVal = IRB1.CreatePHI(IRB1.getInt64Ty(), 2);

		    assert(AlignVal);
		    (dyn_cast<PHINode>(AlignVal))->addIncoming(AlignVal2, ThenTerm->getParent());
		    (dyn_cast<PHINode>(AlignVal))->addIncoming(CmpVal, ThenTerm->getParent()->getUniquePredecessor());
		    ReturnSizeMap[Pair.first] = AlignVal;

		    assert(LastBlock);
		    for (BasicBlock::iterator I = LastBlock->begin(); I != LastBlock->end(); ++I) {
		        Instruction *BI = &*I;
		        CallInst *CI = dyn_cast<CallInst>(BI);
		        if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
		           	IntrinsicSet.insert(BI);
		        }
		    }
      	}

      	//Perform XOR and compare with the alignment value. 
      	DenseMap<Value *, Value *> ReplacementMap;
	    for(auto BI: IntrinsicSet) {
	    	assert(BI);
	      	IRBuilder<> IRB1(BI);
	        assert(BI->getOperand(0)->getType() == BI->getOperand(1)->getType());
	        Value *O1 = IRB1.CreatePtrToInt(BI->getOperand(0), IRB1.getInt64Ty());
	        Value *O2 = IRB1.CreatePtrToInt(BI->getOperand(1), IRB1.getInt64Ty());

	        assert(O1 && O2);
	        Value *ResXOR = IRB1.CreateXor(O1, O2);
	        assert(ReturnSizeMap.count(BI->getOperand(0)));
	        Value *RTCall = IRB1.CreateICmp(ICmpInst::ICMP_UGE, ResXOR, ReturnSizeMap[BI->getOperand(0)]);
		   	assert(RTCall);
		   	ReplacementMap[BI] = RTCall;
	    }

	    for(auto Pair: ReplacementMap) {
		  	Pair.first->replaceAllUsesWith(Pair.second);
		  	(dyn_cast<Instruction>(Pair.first))->eraseFromParent();
		}
    }

    //Remove Intrinsic.
    DenseSet<Instruction *> InstToRemove;
    for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
            if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
            	//InstToRemove.insert(BI);
            	assert(0 && "IntrinsicInst found.");
            }
      	}
    }

    /*for(auto IR: InstToRemove) {
    	IR->eraseFromParent();
    }*/

  	if (verifyFunction(*F, &errs())) {
		errs() << "Not able to verify!\n";
		errs() << *F << "\n";
		assert(0);
	}
}

bool Allocator2kLegacyPass::runOnFunction(Function &F) {

  if (F.isDeclaration()) {
    return false;
  }

  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);

  DominatorTree DT(F);
  LoopInfo LI(DT);

  convertDynamicCheckIntrToCall(&F);
  return run(F, LI, TLI);
}
