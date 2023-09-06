#include "llvm/Transforms/Vectorize/CustomVectorization.h"

#define REGIONDIST 4294967296
#define REGIONLIMIT 255

void fixIRForVectorization(Function *F, PHINode *PHI, LoopInfo *LI, DominatorTree *DT, AAResults *AA) {
	Loop *LP;
	bool presentInLoop = false;
	for (Loop *L : *LI) {
		if(L->contains(PHI->getParent())) {
			LP = L;
			presentInLoop = true;
			std::vector<Loop *> Worklist;
			Worklist.push_back(L);
			while(!Worklist.empty()) {
				Loop *CurLoop = Worklist.back();
				Worklist.pop_back();
				for (Loop *InnerL : *CurLoop) {
					if(InnerL->contains(PHI->getParent())) {
						LP = InnerL;
						presentInLoop = true;
						Worklist.push_back(InnerL);
					}
				}
			}
		}
	}

	if(!presentInLoop) {
		return;
	}

	if (PHI->getParent() != LP->getHeader() ||
		PHI->getNumIncomingValues() != 2) {
		return;
	}

	auto *Preheader = LP->getLoopPreheader();
	auto *Latch = LP->getLoopLatch();
	if (!Preheader || !Latch) {
		return;
	}

	if (PHI->getBasicBlockIndex(Preheader) < 0 ||
		PHI->getBasicBlockIndex(Latch) < 0) {
		return;
	}

	auto *Previous = dyn_cast<Instruction>(PHI->getIncomingValueForBlock(Latch));
	if (!Previous || !LP->contains(Previous)) {
		return;
	}

	assert(PHI->getParent());

	if(PHINode *PrevPHI = dyn_cast<PHINode>(Previous)) {
		std::vector<Instruction *> Visited;
		bool isPHI = true;
		//In case of PHI inside PHI, Previous of Previous 
		//PHI must dominate all uses of current PHI.
		while(isPHI) {
			if (PrevPHI->getParent() != PHI->getParent() ||
				PrevPHI->getNumIncomingValues() != 2 ||
				PrevPHI->getBasicBlockIndex(Preheader) < 0 ||
				PrevPHI->getBasicBlockIndex(Latch) < 0)
				return;

			if(isa<Instruction>(PrevPHI->getIncomingValueForBlock(Latch))) {
				if(isa<PHINode>(PrevPHI->getIncomingValueForBlock(Latch))) {
					std::vector<Instruction *>::iterator it = std::find (Visited.begin(), 
						Visited.end(), dyn_cast<Instruction>(PrevPHI->getIncomingValueForBlock(Latch)));
					if(it == Visited.end()) {
						PrevPHI = dyn_cast<PHINode>(PrevPHI->getIncomingValueForBlock(Latch));
						Visited.push_back(PrevPHI);
					}
					else {
		            	//Cycle exists.
						return;
					}
				}
				else {
					Previous = dyn_cast<Instruction>(PrevPHI->getIncomingValueForBlock(Latch));
					isPHI = false;
				}
			}
			else {
				//Not a recurrence.
				return;
			}
		}
	}

	if(!LP->contains(Previous)) {
		return;
	}

	Instruction *firstUser;
	bool firstUserFound = false;
	if (PHI->hasOneUse()) {
		auto *I = PHI->user_back();
		if (I->isCast() && (I->getParent() == PHI->getParent()) && I->hasOneUse() &&
			DT->dominates(Previous, I->user_back())) {
			return;
		}

		if(Instruction *II = dyn_cast<Instruction>(I)) {
			firstUser = II;
			firstUserFound = true;
		}
	}
	else {
		for (User *U : PHI->users()) {
			if (auto *I = dyn_cast<Instruction>(U)) {
				if(!isa<PHINode>(I) && Previous->getParent() == I->getParent()) {
					if(firstUserFound) {
						firstUser = DT->dominates(I, firstUser) ? I : firstUser;
					}
					else {
						firstUser = I;
						firstUserFound = true;
					}
				}
			}
		}
	}

	if(firstUserFound) {
		std::vector<Instruction *> moveInstructions;
		if (Previous != firstUser && !DT->dominates(Previous, firstUser)) {
			BasicBlock::iterator PreviousII(Previous);
			bool move = true;
			std::vector<BasicBlock::iterator> Worklist;
			Worklist.push_back(PreviousII);
			moveInstructions.clear();
			while(!Worklist.empty()) {
				BasicBlock::iterator II(firstUser);
		        BasicBlock::iterator Prev = Worklist.back();
		        Instruction *PrevI = &*Prev;
		        if(PrevI == firstUser) {
		        	Worklist.clear();
		        	moveInstructions.clear();
		        	break; 
		        }
		        /*if(DT.dominates(PrevI, firstUser)) {
		          Worklist.clear();
		          break; 
		        }*/
		        Worklist.pop_back();
		        if(PrevI->getParent() != firstUser->getParent()) {
		        	moveInstructions.clear();
		        	break;
		        }
		        while(II != firstUser->getParent()->end()) {
		        	if(II == Prev) {
		        		break;
		        	}
		        	Instruction *Instr = &*II;
		        	uint64_t TP1 = 1;
		        	uint64_t TP2 = 1;
		        	if(PointerType *PT = dyn_cast<PointerType>(Instr->getType())) {
			            Type *ElTy = PT->getElementType();
			            if(ElTy->isSized()) {
			            	DataLayout* DL = new DataLayout(F->getParent());
			            	TP1 = DL->getTypeAllocSize(ElTy);
			            }
			        }
			        if(PointerType *PT = dyn_cast<PointerType>(PrevI->getType())) {
			            Type *ElTy = PT->getElementType();
			            if(ElTy->isSized()) {
			            	DataLayout* DL = new DataLayout(F->getParent());
			            	TP2 = DL->getTypeAllocSize(ElTy);
			            }
			        }

			        const MemoryLocation &M1 = MemoryLocation(Instr, LocationSize::precise(TP1));
			        const MemoryLocation &M2 = MemoryLocation(PrevI, LocationSize::precise(TP2));
			        AliasResult AR = AA->alias(M1, M2);
			        if(AR != NoAlias) {
			        	move = false;
			        	moveInstructions.clear();
			        	Worklist.clear();
			        	break;
			        }
			        II++;
		        }
		        
		        if(move) {
		        	moveInstructions.push_back(PrevI);
		        	for(unsigned i = 0; i < PrevI->getNumOperands(); i++) {
		        		if(Instruction *OperI = dyn_cast<Instruction>(PrevI->getOperand(i))) {
		        			BasicBlock::iterator PrevII(OperI);
		        			std::vector<Instruction *>::iterator it = std::find (moveInstructions.begin(), moveInstructions.end(), OperI);
		        			if(!DT->dominates(OperI, firstUser) && it == moveInstructions.end()) {
		        				Worklist.push_back(PrevII);
		        			}
		        		}
		        	}
		        }

		        if(moveInstructions.size() > 3) {
		        	moveInstructions.clear();
		        	Worklist.clear();
		        }
		    }
		}

		if(moveInstructions.size() > 0) {
			Instruction *InsertHere = isa<PHINode>(firstUser) 
				? firstUser->getParent()->getFirstNonPHI() : firstUser;
			for(auto MI : moveInstructions) {
				BasicBlock *CurBB = MI->getParent();
				CurBB->getInstList().remove(MI);
				MI->insertBefore(InsertHere);
				InsertHere = MI;
			}
		}
	}

	if (verifyFunction(*F, &errs())) {
		errs() << "Not able to verify!\n";
		errs() << *F << "\n";
		assert(0);
	}
}

static bool isSafeAlloca(const AllocaInst *AllocaPtr, const TargetLibraryInfo &TLI) {
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

static uint64_t getAllocaSizeInBytes(const AllocaInst &AI) {
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

static bool isInnermostLoop(Loop *CLoop) {
	if(CLoop->empty()) {
		return true;
	}
	return false;
}

static bool trackInstruction(Instruction *I) {
  	assert(I);
  	if(isa<Constant>(I)) {
  		return false;
  	}
  	return (isa<LoadInst>(I) || isa<StoreInst>(I));
}

static void getBases(Loop *CLoop, Value *V, 
	SmallVectorImpl<const Value *> &Objects, 
	LoopInfo *LI, DominatorTree *DT) {
	const DataLayout &DL = CLoop->getHeader()->getParent()
				->getParent()->getDataLayout();
	auto B = GetUnderlyingObject(V, DL);
	assert(B);
	Instruction *I = dyn_cast<Instruction>(B);
	if(I && (isa<PHINode>(I) || isa<SelectInst>(I))) {
		if(/*(I->getParent() == CLoop->getLoopPreheader()) 
			||*/ (!DT->dominates(I->getParent(), CLoop->getLoopPreheader()))) {
			GetUnderlyingObjects(V, Objects, DL, LI, 0);
		}
	}

	if(Objects.size() < 1) {
		Objects.push_back(B);
	}
	assert(Objects.size() > 0);

	for (auto Obj : Objects) {
		if (Obj && !isa<Constant>(Obj)) {
			Instruction *BII = dyn_cast<Instruction>(const_cast<Value*>(Obj));
			if(BII && (/*(BII->getParent() == CLoop->getLoopPreheader()) 
				||*/ (!DT->dominates(BII->getParent(), CLoop->getLoopPreheader())))) {
				Objects.clear();
				break;
			}
		}
	}
}

static MemoryLocation getMemLoc(Instruction *I) {
	assert(isa<LoadInst>(I) || isa<StoreInst>(I));
	MemoryLocation Loc = MemoryLocation::get(I);
    assert(Loc.Ptr && Loc.Ptr->getType()->isPointerTy() 
               	   && Loc.Size.hasValue());
    if(isa<Constant>(Loc.Ptr)) {
        Loc.getWithNewPtr(nullptr);
    }
    return Loc;
}

static bool areMemDependenciesFound(Loop *CLoop, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	DenseSet<pair<Instruction *, Instruction *>> &Insts, 
	DenseSet<Instruction *> &ConflictingLoads, 
	/*DenseSet<Instruction *> &LoadInsts,*/
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, 
	DenseSet<Instruction *> &LoadSet, 
	DenseSet<Instruction *> &StoreSet) {
	//DenseSet<Instruction *> StoreInsts;

	for (BasicBlock *BB : CLoop->blocks()) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		    Instruction *BI = &*I;
		    if(trackInstruction(BI)) {
		      	assert(isa<LoadInst>(BI) || isa<StoreInst>(BI));
		      	if(isa<LoadInst>(BI)) {
	               	LoadSet.insert(BI);
				}
				else if(isa<StoreInst>(BI)) {
				  	StoreSet.insert(BI);
				}
			}
		}
	}

	for(auto I1 : LoadSet) {
	    MemoryLocation Loc1 = getMemLoc(I1);
	    if(!Loc1.Ptr) {
	    	continue;
	    }
		for(auto I2 : StoreSet) {
			MemoryLocation Loc2 = getMemLoc(I2);
			if(!Loc2.Ptr) {
		    	continue;
		    }
			AliasResult AR = AA->alias(Loc1, Loc2);
			if(AR == MustAlias) {
				ConflictingLoads.insert(I1);
			}
			if(AR == MayAlias) {
				auto Pair = make_pair(I1, I2);
				Insts.insert(Pair);
			}
		}
	}

	if(Insts.size() < 1) {
		return false;
	}

	for(auto Pair: Insts) {
		auto I1 = Pair.first;
		auto I2 = Pair.second;

		MemoryLocation Loc1 = getMemLoc(I1);
		MemoryLocation Loc2 = getMemLoc(I2);
		assert(Loc1.Ptr && Loc2.Ptr);

		SmallVector<const Value *, 4> Objects1, Objects2;
		getBases(CLoop, const_cast<Value*>(Loc1.Ptr), Objects1, 
			LI, DT);
		if(Objects1.size() == 0) {
			return false;
		}

		getBases(CLoop, const_cast<Value*>(Loc2.Ptr), Objects2, 
			LI, DT);
		if(Objects2.size() == 0) {
			return false;
		}
		
		assert(Objects1.size() > 0 && Objects2.size() > 0);

		for (auto Obj1 : Objects1) {
			Value *V1 = const_cast<Value*>(Obj1);

			for (auto Obj2 : Objects2) {
				if (Obj2 && !isa<Constant>(Obj2)) {
					Value *V2 = const_cast<Value*>(Obj2);

					if(V1 == V2) {
						return false;
					}

					auto P1 = make_pair(V1, V2);
					auto P2 = make_pair(V2, V1);
					if(!Bases.count(P1) && !Bases.count(P2)) {
						Bases.insert(P1);
					}
				}
			}
		}
	}

	if(Bases.size() < 1) {
		return false;
	}

	return true;
}

bool areMemDependenciesFoundSelectedInst(Loop *CLoop, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	DenseSet<pair<Instruction *, Instruction *>> &Insts, 
	DenseSet<Instruction *> &ConflictingLoads, 
	DenseSet<Instruction *> &LoadInsts, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, 
	DenseSet<Instruction *> LoadSet, 
	DenseSet<Instruction *> StoreSet) {

	//DenseSet<Instruction *> LoadInsts;
	DenseSet<Instruction *> StoreInsts;

	for (BasicBlock *BB : CLoop->blocks()) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		    Instruction *BI = &*I;
		    if(trackInstruction(BI)) {
		      	assert(isa<LoadInst>(BI) || isa<StoreInst>(BI));
		      	if(isa<LoadInst>(BI)) {
	               	LoadInsts.insert(BI);
				}
				else if(isa<StoreInst>(BI)) {
				  	StoreInsts.insert(BI);
				}
			}
		}
	}

	for(auto I1 : LoadSet) {
	    MemoryLocation Loc1 = getMemLoc(I1);
	    if(!Loc1.Ptr) {
	    	continue;
	    }

		for(auto I2 : StoreInsts) {
			MemoryLocation Loc2 = getMemLoc(I2);
			if(!Loc2.Ptr) {
		    	continue;
		    }
			AliasResult AR = AA->alias(Loc1, Loc2);
			if(AR == MustAlias) {
				ConflictingLoads.insert(I1);
			}
			if(AR == MayAlias) {
				auto Pair = make_pair(I1, I2);
				Insts.insert(Pair);
			}
		}
	}

	for(auto I1 : StoreSet) {
	    MemoryLocation Loc1 = getMemLoc(I1);
	    if(!Loc1.Ptr) {
	    	continue;
	    }

	    for(auto I2 : LoadInsts) {
	    	//This loop is required in case 
	    	//only store got benefitted.
			assert(I1 != I2);

			MemoryLocation Loc2 = getMemLoc(I2);
			if(!Loc2.Ptr) {
		    	continue;
		    }
			AliasResult AR = AA->alias(Loc1, Loc2);
			if(AR == MayAlias) {
				auto Pair = make_pair(I1, I2);
				Insts.insert(Pair);
			}
		}

		for(auto I2 : StoreInsts) {
			if(I1 == I2) {
				continue;
			}

			MemoryLocation Loc2 = getMemLoc(I2);
			if(!Loc2.Ptr) {
		    	continue;
		    }
			AliasResult AR = AA->alias(Loc1, Loc2);
			if(AR == MayAlias) {
				auto Pair = make_pair(I1, I2);
				Insts.insert(Pair);
			}
		}
	}

	if(Insts.size() < 1) {
		return false;
	}

	for(auto Pair: Insts) {
		auto I1 = Pair.first;
		auto I2 = Pair.second;

		MemoryLocation Loc1 = getMemLoc(I1);
		MemoryLocation Loc2 = getMemLoc(I2);
		assert(Loc1.Ptr && Loc2.Ptr);

		SmallVector<const Value *, 4> Objects1, Objects2;
		getBases(CLoop, const_cast<Value*>(Loc1.Ptr), Objects1, 
			LI, DT);
		if(Objects1.size() == 0) {
			return false;
		}

		getBases(CLoop, const_cast<Value*>(Loc2.Ptr), Objects2, 
			LI, DT);
		if(Objects2.size() == 0) {
			return false;
		}
		
		assert(Objects1.size() > 0 && Objects2.size() > 0);

		for (auto Obj1 : Objects1) {
			Value *V1 = const_cast<Value*>(Obj1);

			for (auto Obj2 : Objects2) {
				if (Obj2 && !isa<Constant>(Obj2)) {
					Value *V2 = const_cast<Value*>(Obj2);

					if(V1 == V2) {
						return false;
					}

					auto P1 = make_pair(V1, V2);
					auto P2 = make_pair(V2, V1);
					if(!Bases.count(P1) && !Bases.count(P2)) {
						Bases.insert(P1);
					}
				}
			}
		}
	}

	if(Bases.size() < 1) {
		return false;
	}

	return true;
}

void updatePredecessors(BasicBlock *LoopPreheader, BasicBlock *RuntimeChecks) {
	DenseSet<Instruction *> TermToUpdate;
	//Update the predecessors of Loop Preheader.
	for (auto it = pred_begin(LoopPreheader), et = pred_end(LoopPreheader); it != et; ++it) {
  		BasicBlock *Pred = *it;
  		assert(Pred);
  		if(Pred == RuntimeChecks) {
  			continue;
  		}

  		assert(Pred->getTerminator());
  		TermToUpdate.insert(Pred->getTerminator());
	}
		
	for(auto I : TermToUpdate) {
		if(BranchInst *BI = dyn_cast<BranchInst>(I)) {
	  		if(BI->isConditional()) {
	  			if(BI->getSuccessor(0) == LoopPreheader) {
		  			SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  					BI->getAllMetadata(MDs);
  					auto NewBI = BranchInst::Create(RuntimeChecks, BI->getSuccessor(1), BI->getCondition());
  					for(const auto &MI : MDs) {
    					NewBI->setMetadata(MI.first, MI.second);
  					}
    				ReplaceInstWithInst(BI, NewBI);
		  		}
		  		else {
		  			assert(BI->getSuccessor(1) == LoopPreheader);

		 			SmallVector<pair<unsigned, MDNode *>, 4> MDs;
		  			BI->getAllMetadata(MDs);
		  			auto NewBI = BranchInst::Create(BI->getSuccessor(0), RuntimeChecks, BI->getCondition());
		  			for(const auto &MI : MDs) {
		    			NewBI->setMetadata(MI.first, MI.second);
		  			}
		    		ReplaceInstWithInst(BI, NewBI);
		  		}
		  	}
		  	else {
		  		assert(BI->isUnconditional());
		  		//ReplaceInstWithInst(BI, BranchInst::Create(RuntimeChecks));
		 		SmallVector<pair<unsigned, MDNode *>, 4> MDs;
		  		BI->getAllMetadata(MDs);
		  		auto NewBI = BranchInst::Create(RuntimeChecks);
		  		for(const auto &MI : MDs) {
		    		NewBI->setMetadata(MI.first, MI.second);
		  		}
		    	ReplaceInstWithInst(BI, NewBI);
		  	}
		}
		else if(SwitchInst *SI = dyn_cast<SwitchInst>(I)) {
			for(unsigned i = 0; i < SI->getNumSuccessors(); i++) {
				if(SI->getSuccessor(i) == LoopPreheader) {
					SI->setSuccessor(i, RuntimeChecks);
				} 
			}
		}
		else {
			assert(0 && "Invalid terminator instruction.");
		}
	}

	if(BranchInst *BI = dyn_cast<BranchInst>(LoopPreheader->getTerminator())) {
	  	assert(BI->isConditional());
	  	assert(BI->getSuccessor(0) == RuntimeChecks);
		SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  		BI->getAllMetadata(MDs);
  		auto NewBI = BranchInst::Create(BI->getSuccessor(1));
  		for(const auto &MI : MDs) {
    		NewBI->setMetadata(MI.first, MI.second);
  		}
    	ReplaceInstWithInst(BI, NewBI);
    }
}

static BasicBlock *createBlockToAddRuntimeChecks(Function *F, Loop *CLoop, 
	DominatorTree *DT, LoopInfo *LI, bool performCloning) {
	/*LLVMContext &C = F->getContext();
	BasicBlock *RuntimeChecksBlock = BasicBlock::Create(C, 
		"runtime.memchecks", F, CLoop->getLoopPreheader());
  	assert(RuntimeChecksBlock);
  	updatePredecessors(CLoop->getLoopPreheader(), RuntimeChecksBlock);
  	DT->addNewBlock(RuntimeChecksBlock, CLoop->getLoopPreheader());*/
  	BasicBlock *BB = CLoop->getLoopPreheader();

  	if(performCloning) {
  		BB->setName("runtime.memchecks");
  	}
  	BasicBlock *NewBB = BB->splitBasicBlock(
  		BB->getTerminator());
  	assert(NewBB);
  	DT->addNewBlock(NewBB, BB);

  	//Update the header of the parent loop if required.
  	Loop *ParentLoop = CLoop->getParentLoop();
  	if(ParentLoop) {
  		ParentLoop->addBasicBlockToLoop(NewBB, *LI);
  		if(CLoop->getLoopPreheader() == ParentLoop->getHeader()) {
	  		ParentLoop->moveToHeader(BB);
	  	}
  	}
  	return BB;
}

static bool isUnhandledValue(Value *V) {
	/*if(V->hasName() && V->getName().equals("this")) {
		return true;
	}*/
	if(CallInst *CI = dyn_cast<CallInst>(V)) {
		if (CI && CI->getCalledFunction()) {
	        if(CI->getCalledFunction()->getName().equals("__ctype_b_loc") || 
	        	CI->getCalledFunction()->getName().equals("__ctype_toupper_loc")) {
	        	return true;
	        }
	    }
	}
	return false;
}

void createBitcastForPointerBases(Loop *CLoop, BasicBlock *RuntimeChecksBlock,
	DenseSet<pair<Value *, Value *>> &BasesBitcast,
	DenseSet<pair<Value *, Value *>> Bases, DenseSet<pair<Value *, Value *>> &AddedBases) {
	BasicBlock *Header = CLoop->getHeader();
	assert(Header);
	Instruction *TermInst = RuntimeChecksBlock->getTerminator();
	Module *M = RuntimeChecksBlock->getParent()->getParent();
	IntegerType *CharType = Type::getInt8Ty(M->getContext());
	IntegerType *Char64Type = Type::getInt64Ty(M->getContext());
	PointerType *CharStarType = PointerType::getUnqual(CharType);

	DenseMap<Value *, Value *> InstBitcast;
	for(auto Pair: Bases) {
		auto V1 = Pair.first;
		auto V2 = Pair.second;
		bool V1F = false, V2F = false;
		if(PointerType *PT = dyn_cast<PointerType>(V1->getType())) {
			if(IntegerType *IT = dyn_cast<IntegerType>(PT->getPointerElementType())) {
				if(IT->getBitWidth() == 8) {
					V1F = true;
				}
			}
		}
		if(PointerType *PT = dyn_cast<PointerType>(V2->getType())) {
			if(IntegerType *IT = dyn_cast<IntegerType>(PT->getPointerElementType())) {
				if(IT->getBitWidth() == 8) {
					V2F = true;
				}
			}
		}
		auto P1 = make_pair(V1, V2);
		auto P2 = make_pair(V2, V1);
		if(AddedBases.count(P1) || AddedBases.count(P2)) {
			//continue;
			assert(0 && "Duplicate bases.");
		}
		if(!InstBitcast.count(V1)) {
			if(V1F) {
				InstBitcast[V1] = V1;
			}
			else if(isUnhandledValue(V1)) {
				//IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(CharType, 64), CharStarType, "", TermInst);
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		InstBitcast[V1] = ITPI;
			}
			else {
				BitCastInst *BitI = new BitCastInst(V1, CharStarType, "", TermInst);
	    		assert(BitI);
	    		InstBitcast[V1] = BitI;
			}
		}

		if(!InstBitcast.count(V2)) {
			if(V2F) {
				InstBitcast[V2] = V2;
			}
			else if(isUnhandledValue(V2)) {
				//IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(CharType, 64), CharStarType, "", TermInst);
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		InstBitcast[V2] = ITPI;
			}
			else {
				BitCastInst *BitI = new BitCastInst(V2, CharStarType, "", TermInst);
	    		assert(BitI);
	    		InstBitcast[V2] = BitI;
			}
		}
		
		AddedBases.insert(P1);
		assert(InstBitcast[V1] && InstBitcast[V2]);
		BasesBitcast.insert(make_pair(InstBitcast[V1], InstBitcast[V2]));
		/*if(isa<AllocaInst>(V1)) {
			assert(!isa<AllocaInst>(V2));
			BasesBitcast.insert(make_pair(InstBitcast[V2], InstBitcast[V1]));
		}
		else {
			BasesBitcast.insert(make_pair(InstBitcast[V1], InstBitcast[V2]));
		}*/
		/*BasesBitcast.insert(V1F ? make_pair(InstBitcast[V2], InstBitcast[V1]) 
			: make_pair(InstBitcast[V1], InstBitcast[V2]));*/
	}
	//assert(Bases.size() == BasesBitcast.size());
}

void unionLoadStoreBases(DenseSet<pair<Value *, Value *>> LoadBases, DenseSet<pair<Value *, Value *>> StoreBases, 
	DenseSet<pair<Value *, Value *>> &FinalBases) {
	for(auto Pair: LoadBases) {
		auto V1 = Pair.first;
		auto V2 = Pair.second;
		auto P1 = make_pair(V1, V2);
		auto P2 = make_pair(V2, V1);
		if(FinalBases.count(P1) || FinalBases.count(P2)) {
			continue;
		}
		else {
			FinalBases.insert(Pair);
		}
	}
	if(StoreBases.size() > 0) {
		for(auto Pair: StoreBases) {
			auto V1 = Pair.first;
			auto V2 = Pair.second;
			auto P1 = make_pair(V1, V2);
			auto P2 = make_pair(V2, V1);
			if(FinalBases.count(P1) || FinalBases.count(P2)) {
				continue;
			}
			else {
				FinalBases.insert(Pair);
			}
		}
	}
}

static Value* getAlignment2k(Function &F, BasicBlock *BB, Value *Base, DenseMap<Value*, Value*> &BaseToAlignment) {
	if (BaseToAlignment.count(Base)) {
		return BaseToAlignment[Base];
	}
	IRBuilder<> IRB(BB->getTerminator());
	auto Fn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::alignment2k);
	auto Alignment = IRB.CreateCall(Fn, {IRB.CreateBitCast(Base, IRB.getInt8PtrTy())});
	BaseToAlignment[Base] = Alignment;
	return Alignment;
}

static bool isUnhandledValueSafe(const Value *UnhandledCall, const TargetLibraryInfo *TLI) {
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 8> WorkList;	
  WorkList.push_back(UnhandledCall);

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
      	if(CallInst *CI = dyn_cast<CallInst>(dyn_cast<Instruction>(UI.getUser()))) {
      		Function *Fn = CI->getCalledFunction();
      		if(Fn && Fn->getName().equals("_mi_dynamic_check")) {
      			continue;
      		}
      	}
        ImmutableCallSite CS(I);

        if (I->isLifetimeStartOrEnd())
          continue;
        if (dyn_cast<MemIntrinsic>(I)) {
        	continue;
        }

				LibFunc Func;
    		if (TLI->getLibFunc(ImmutableCallSite(CS), Func)) {
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

void instrumentLoopLog(Loop *NewLoop, DenseSet<pair<Value *, Value *>> Bases, string FileName, 
	Function *F, unsigned FID, unsigned LID, TargetLibraryInfo *TLI) {
	assert(!FileName.empty());

	assert(NewLoop->getLoopPreheader());
	Instruction *TermInst = NewLoop->getLoopPreheader()->getTerminator();
	assert(TermInst);

	unsigned LoopID = UINT_MAX;
	LoopID = getLoopID(F, FID, LID, FileName);
	assert(LoopID != UINT_MAX);

	if(!isgetASIDDefined) {
	  	vector<Type *> ArgTypes;
		ArgTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(TermInst->getContext())));
		//ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
		FunctionType *DTy = FunctionType::get(Type::getInt64Ty(TermInst->getContext()), ArgTypes, false);
		getASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "getASID", (TermInst->getModule()));
		getASIDFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadNone);
		isgetASIDDefined = true;
	}

	/*if(!ischeckASIDDefined) {
	  	vector<Type *> ArgTypes;
	  	ArgTypes.push_back(Type::getInt32Ty(F->getContext()));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	checkASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "checkASID", (F->getParent()));
	  	ischeckASIDDefined = true;
	}*/

	if(!isdynamicCheckDefined) {
	  	vector<Type *> ArgTypes;
	  	ArgTypes.push_back(Type::getInt32Ty(F->getContext()));
	  	ArgTypes.push_back(Type::getInt32Ty(F->getContext()));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	dynamicCheckFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "dynamicCheck", (F->getParent()));
	  	isdynamicCheckDefined = true;
	}

	if(!isstoreDynamicChecksDefined) {
	  	vector<Type *> ArgTypes;
	  	ArgTypes.push_back(Type::getInt32Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	storeDynamicChecksFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "storeDynamicChecks", (F->getParent()));
	  	isstoreDynamicChecksDefined = true;
	}

	IntegerType *Char64Type = Type::getInt64Ty(F->getContext());
	IntegerType *CharType = Type::getInt8Ty(F->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);

  	DenseMap<Value *, Value *> BasesBitcastMap;
	DenseSet<pair<Value *, Value *>> BasesBitcast;
	for(auto Pair: Bases) {
		if(!BasesBitcastMap.count(Pair.first)) {
			/*assert(!isUnhandledValue(Pair.first));
			BitCastInst *BI = new BitCastInst(Pair.first, CharStarType, "", TermInst);
			assert(BI);
			BasesBitcastMap[Pair.first] = BI;*/
			/*if(isa<AllocaInst>(Pair.first)) {
				//assert(0);
				BasesBitcastMap[Pair.first] = Pair.first;
			}
			else*/ if(isUnhandledValue(Pair.first)) {
				assert(isUnhandledValueSafe(Pair.first, TLI));
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		BasesBitcastMap[Pair.first] = ITPI;
			}
			else {
				BitCastInst *BI = new BitCastInst(Pair.first, CharStarType, "", TermInst);
				assert(BI);
				BasesBitcastMap[Pair.first] = BI;
			}
		}

		if(!BasesBitcastMap.count(Pair.second)) {
			/*assert(!isUnhandledValue(Pair.second));
			BitCastInst *BI = new BitCastInst(Pair.second, CharStarType, "", TermInst);
			assert(BI);
			BasesBitcastMap[Pair.second] = BI;*/
			/*if(isa<AllocaInst>(Pair.second)) {
				//assert(0);
				BasesBitcastMap[Pair.second] = Pair.second;
			}
			else*/ if(isUnhandledValue(Pair.second)) {
				assert(isUnhandledValueSafe(Pair.second, TLI));
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		BasesBitcastMap[Pair.second] = ITPI;
			}
			else {
				BitCastInst *BI = new BitCastInst(Pair.second, CharStarType, "", TermInst);
				assert(BI);
				BasesBitcastMap[Pair.second] = BI;
			}
		}

		assert(BasesBitcastMap.count(Pair.first) && BasesBitcastMap.count(Pair.second));
		BasesBitcast.insert(make_pair(BasesBitcastMap[Pair.first], BasesBitcastMap[Pair.second]));
	}

	/*DenseMap<Value*, Value*> BaseToAlignment;
	for(auto Pair : BasesBitcast) {
		if(!BaseToAlignment.count(Pair.first)) {
			getAlignment2k(*F, NewLoop->getLoopPreheader(), Pair.first, BaseToAlignment);
		}
		if(!BaseToAlignment.count(Pair.second)) {
			getAlignment2k(*F, NewLoop->getLoopPreheader(), Pair.second, BaseToAlignment);
		}
	}*/

	DenseMap<Value *, Value *> ValueASIDMap;
	for(auto Pair : BasesBitcast) {
		assert(Pair.first->getType()->isPointerTy() && Pair.second->getType()->isPointerTy());
		if(!ValueASIDMap.count(Pair.first)) {
			vector<Value*> Args;
			Args.push_back(Pair.first);
			ValueASIDMap[Pair.first] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			/*if(isa<AllocaInst>(Pair.first)) {
				vector<Value*> Args;
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 0xFFFFFFFE), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		Args.push_back(ITPI);
				ValueASIDMap[Pair.first] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			}
			else {
				//auto BI = dyn_cast<BitCastInst>(Pair.first);
				//assert(BI);
				vector<Value*> Args;
				Args.push_back(Pair.first);
				//assert(BaseToAlignment.count(Pair.first));
				//Args.push_back(BaseToAlignment[Pair.first]);
				ValueASIDMap[Pair.first] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			}*/
		}

		if(!ValueASIDMap.count(Pair.second)) {
			vector<Value*> Args;
			Args.push_back(Pair.second);
			ValueASIDMap[Pair.second] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			/*if(isa<AllocaInst>(Pair.second)) {
				vector<Value*> Args;
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 0xFFFFFFFE), CharStarType, "", TermInst);
	    		assert(ITPI);
	    		Args.push_back(ITPI);
				ValueASIDMap[Pair.second] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			}
			else {
				//auto BI = dyn_cast<BitCastInst>(Pair.second);
				//assert(BI);
				vector<Value*> Args;
				Args.push_back(Pair.second);
				//assert(BaseToAlignment.count(Pair.second));
				//Args.push_back(BaseToAlignment[Pair.second]);
				ValueASIDMap[Pair.second] = CallInst::Create(getASIDFunc, Args, "", TermInst);
			}*/
		}
	}

	/*for(auto Pair : BasesBitcast) {
		assert(ValueASIDMap.count(Pair.first));
		assert(ValueASIDMap.count(Pair.second));
		vector<Value*> Args;
		Args.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), LoopID));	
		Args.push_back(ValueASIDMap[Pair.first]);
		Args.push_back(ValueASIDMap[Pair.second]);
		CallInst::Create(checkASIDFunc, Args, "", TermInst);
	}*/

	for(auto Pair : BasesBitcast) {
		assert(ValueASIDMap.count(Pair.first));
		assert(ValueASIDMap.count(Pair.second));
		vector<Value*> Args;
		Args.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), LoopID));
		Args.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), BasesBitcast.size()));	
		Args.push_back(ValueASIDMap[Pair.first]);
		Args.push_back(ValueASIDMap[Pair.second]);
		CallInst::Create(dynamicCheckFunc, Args, "", TermInst);
	}

	if(BasesBitcast.size() > 0) {
		vector<Value*> Args;
		Args.push_back(ConstantInt::get(Type::getInt32Ty(F->getContext()), LoopID));
		CallInst::Create(storeDynamicChecksFunc, Args, "", TermInst);
	}
}

void emitRuntimeChecks(Function *F, unsigned FID, unsigned LID, Loop *CLoop, Loop *NewLoop, BasicBlock *RuntimeChecksBlock, 
	DenseSet<pair<Value *, Value *>> Bases, bool performCloning, DenseSet<pair<Value *, Value *>> &AddedBases, string FileName, 
	TargetLibraryInfo *TLI) {

	/*IRBuilder<> IRB(RuntimeChecksBlock);
	IRB.SetInsertPoint(RuntimeChecksBlock);
   	BranchInst *TermInst = IRB.CreateBr(CLoop->getLoopPreheader());
  	assert(TermInst && "Terminator instruction not found for runtime checks block.");*/
  	Instruction *TermInst = RuntimeChecksBlock->getTerminator();
  	assert(TermInst && "Terminator instruction not found for runtime checks block.");

  	//Create Bitcast for bases of load/store operands.
  	DenseSet<pair<Value *, Value *>> BasesBitcast;
  	createBitcastForPointerBases(CLoop, RuntimeChecksBlock, 
  		BasesBitcast, Bases, AddedBases);

  	Module *M = RuntimeChecksBlock->getParent()->getParent();
  	//Type *VoidType = Type::getVoidTy(M->getContext());
  	IntegerType *Int1Type = Type::getInt1Ty(M->getContext());
  	//IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	//IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	//PointerType *CharStarType = PointerType::getUnqual(CharType);

  	IRBuilder<> IRB1(TermInst);

  	/*if(!isFuncDefAdded) {
  		vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
		ArgTypes.push_back(CharStarType);
		//FunctionType *FTy = FunctionType::get(IntType, ArgTypes, false);
		FunctionType *FTy = FunctionType::get(Int1Type, ArgTypes, false);
		isNoAliasRTCheckFunc = Function::Create(FTy,
			GlobalValue::ExternalLinkage, "isNoAliasRTCheck", M);
		isNoAliasRTCheckFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadNone);
		isFuncDefAdded = true;
  	}*/

  	//Add runtime checks for the pointer pairs.
  	DenseSet<Value *> PointerPairReturn;
  	for(auto Pair : BasesBitcast) {
		assert(Pair.first->getType() == Pair.second->getType());
		std::vector<Value *> Args;
		Args.push_back(Pair.first);
		Args.push_back(Pair.second);
		//Type *Tys[] = {Pair.first->getType(), Pair.second->getType()};
		/*Function *DynamicCheckIntrisic = Intrinsic::getDeclaration(RuntimeChecksBlock->
	  		getParent()->getParent(), Intrinsic::dynamic_noalias_check);
	  	assert(DynamicCheckIntrisic);*/
		auto Fn = M->getOrInsertFunction("_mi_dynamic_check", 
         Int1Type, Pair.first->getType(), Pair.second->getType());
		Value *RTCall = IRB1.CreateCall(Fn, Args);
		PointerPairReturn.insert(RTCall);
  	}
  	
  	/*for(auto Pair : BasesBitcast) {
    	vector<Value *> ArgValue;
	    ArgValue.push_back(Pair.first);
	    ArgValue.push_back(Pair.second);
	    //Value *RTCall = IRB1.CreateCall(isNoAliasRTCheckFunc, ArgValue);
	    assert(Pair.first->getType() == Pair.second->getType());
		Value *RTCall = IRB1.CreateICmp(ICmpInst::ICMP_NE, Pair.first, Pair.second);
	    PointerPairReturn.insert(RTCall);
  	}*/

  	//Perform And operation for runtime checks.
  	int c1 = 0;
  	Value *O1 = nullptr, *O2 = nullptr;
  	for(auto Pair : PointerPairReturn) {
    	if(c1 == 0) {
      		O1 = Pair;
    	}
    	else {
      		assert(O1);
      		O2 = Pair;
      		O1 = IRB1.CreateAnd(O1, O2);
    	}
    	c1++;
  	}

  	if(O1) {
    	Value *RTCmp = IRB1.CreateICmp(ICmpInst::ICMP_EQ, 
                  		O1, ConstantInt::get(Int1Type, 0));
    	/*Value *RTCmp = IRB1.CreateICmp(ICmpInst::ICMP_EQ, 
                		O1, ConstantInt::get(IntType, 0));*/
    	//ReplaceInstWithInst(TermInst, BranchInst::Create(LoopPreheader, NewLoopPreheader, RTCmp));
    	if(performCloning) {
    		SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  		TermInst->getAllMetadata(MDs);
	  		auto NewBI = BranchInst::Create(CLoop->getLoopPreheader(), CLoop->getLoopPreheader(), RTCmp);
	  		for(const auto &MI : MDs) {
	    		NewBI->setMetadata(MI.first, MI.second);
	  		}
	    	ReplaceInstWithInst(TermInst, NewBI);
    	}
    	else {
    		if(BranchInst *BI = dyn_cast<BranchInst>(TermInst)) {
	    		assert(BI->isConditional());
	    		SmallVector<pair<unsigned, MDNode *>, 4> MDs;
			  	BI->getAllMetadata(MDs);
			  	auto NewBI = BranchInst::Create(CLoop->getLoopPreheader(), NewLoop->getLoopPreheader(), RTCmp);
			  	for(const auto &MI : MDs) {
			    	NewBI->setMetadata(MI.first, MI.second);
			  	}
			    ReplaceInstWithInst(BI, NewBI);
	    	}
    	}
    	if(!FileName.empty()) {
	  		instrumentLoopLog(NewLoop, Bases, FileName, F, FID, LID, TLI);
	  	}
  	}
}

void addCustomNoAliasIntrinsic(Function *F, Loop *NewLoop, DenseSet<pair<Value *, Value *>> Bases, 
	DenseSet<pair<Value *, Value *>> &AddedBases) {
	if(!isNoAliasIntrisicDefined) {
  		NoAliasIntrinsic = Intrinsic::getDeclaration(F->getParent(), Intrinsic::custom_noalias);
  		isNoAliasIntrisicDefined = true;
  		assert(NoAliasIntrinsic);
  	}

  	for (BasicBlock *BB : NewLoop->blocks()) {
  		Instruction *InsertLoc = dyn_cast<Instruction>(BB->getFirstNonPHI());
	  	assert(InsertLoc);
	  	IRBuilder<> IRB(InsertLoc);

	  	for(auto Pair : Bases) {
	  		auto B1 = Pair.first;
	  		auto B2 = Pair.second;
	  		auto P1 = make_pair(B1, B2);
	  		auto P2 = make_pair(B2, B1);
	  		if(AddedBases.count(P1) || AddedBases.count(P2)) {
	  			continue;
	  		}

			std::vector<Value *> Args;
			Value *MV1 = MetadataAsValue::get(F->getContext(), ValueAsMetadata::get(B1));
			Value *MV2 = MetadataAsValue::get(F->getContext(), ValueAsMetadata::get(B2));
			Args.push_back(MV1);
			Args.push_back(MV2);
			IRB.CreateCall(NoAliasIntrinsic, Args);
			AddedBases.count(P1);
	  	}
  	}

  	if(Bases.size() > 0) {
  		F->addFnAttr("custom_noalias_intrinsic");
  	}
}

void removeCustomNoAliasIntrinsic(Function *F) {
	DenseSet <Instruction *> InstToRemove;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
            if (CI && CI->getIntrinsicID() == Intrinsic::custom_noalias) {
            	InstToRemove.insert(BI);
            }
      	}
  	}

	for(auto I : InstToRemove) {
      	I->eraseFromParent();
	}
}

static BasicBlock* getOuterMostLoop(BasicBlock *BB, LoopInfo &LI, PostDominatorTree &PDT) {
	Loop *L = LI.getLoopFor(BB);
	BasicBlock *Ret = NULL;
	while (L) {
		Ret = L->getLoopPreheader();
		L = LI.getLoopFor(Ret);
	}

	if (Ret && PDT.dominates(BB, Ret)) {
		return Ret;
	}
	return NULL;
}

static void splitIntrinsic2(Function &F, CallInst *CI, DenseMap<Value*, Value*> &BaseToAlignment) {
	Value* Alignment = NULL;
	Value *Base = CI->getArgOperand(0)->stripPointerCasts();
	IRBuilder<> IRB(CI);

	if (BaseToAlignment.count(Base)) {
		Alignment = BaseToAlignment[Base];
	}
	else {
		auto Fn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::alignment2k);
		Alignment = IRB.CreateCall(Fn, {CI->getArgOperand(0)});
		BaseToAlignment[Base] = Alignment;
	}
	auto Fn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::noalias_check);
	auto NewCheck = IRB.CreateCall(Fn, {CI->getArgOperand(0), CI->getArgOperand(1), Alignment});
	CI->replaceAllUsesWith(NewCheck);
	CI->eraseFromParent();
}

static void splitIntrinsic1(Function &F, BasicBlock *BB, BasicBlock *Header, DominatorTree &DT) {
	DenseMap<Value*, Value*> BaseToAlignment;
	std::vector<CallInst*> RuntimeChecks;
	for (auto &Inst : *BB) {
		auto Intrin = dyn_cast<IntrinsicInst>(&Inst);
		if (Intrin && Intrin->getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
			RuntimeChecks.push_back(Intrin);
		}
	}

	if (Header) {
		for (auto Check : RuntimeChecks) {
			auto Base = Check->getArgOperand(0);
			Base = Base->stripPointerCasts();
			if (!isa<Instruction>(Base) || DT.dominates(cast<Instruction>(Base)->getParent(), Header)) {
				auto Alignment = getAlignment2k(F, Header, Base, BaseToAlignment);
				IRBuilder<> IRB(Check);
				auto Fn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::noalias_check);
				auto NewCheck = IRB.CreateCall(Fn, {Check->getArgOperand(0), Check->getArgOperand(1), Alignment});
				Check->replaceAllUsesWith(NewCheck);
				Check->eraseFromParent();
			}
			else {
				splitIntrinsic2(F, Check, BaseToAlignment);
			}
		}
	}
	else {
		for (auto Check : RuntimeChecks) {
			splitIntrinsic2(F, Check, BaseToAlignment);
		}
	}
}

bool splitIntrinsic(Function &F, LoopInfo &LI, DominatorTree &DT, PostDominatorTree &PDT) {
	DenseSet<BasicBlock*> BBSet;
	for (auto &BB : F) {
		for (auto &Inst : BB) {
			auto Intrin = dyn_cast<IntrinsicInst>(&Inst);
			if (Intrin && Intrin->getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
				BBSet.insert(&BB);
				break;
			}
		}
	}

	for (auto BB : BBSet) {
		auto Outer = getOuterMostLoop(BB, LI, PDT);
		splitIntrinsic1(F, BB, Outer, DT);
	}
	return !BBSet.empty();
}

void mergeAlignment(Function &F, DenseSet<CallInst*> &AlignSet) {
	DominatorTree DT(F);
	DenseSet<CallInst*> ToDelete;

	for (auto itr1 = AlignSet.begin(); itr1 != AlignSet.end(); itr1++) {
    for (auto itr2 = std::next(itr1); itr2 != AlignSet.end(); itr2++) {

      auto *Call1 = dyn_cast<CallInst>(*itr1);
      auto *Call2 = dyn_cast<CallInst>(*itr2);

      if (ToDelete.count(Call1) || ToDelete.count(Call2)) {
        continue;
      }

      auto Base1 = Call1->getArgOperand(0)->stripPointerCasts();
      auto Base2 = Call2->getArgOperand(0)->stripPointerCasts();


      if (Base1 == Base2) {
        if (DT.dominates(Call1, Call2)) {
          Call2->replaceAllUsesWith(Call1);
          ToDelete.insert(Call2);
        }
        else if (DT.dominates(Call2, Call1)) {
          Call1->replaceAllUsesWith(Call2);
          ToDelete.insert(Call1);
        }
      }
		}
	}
	for (auto Call : ToDelete) {
		Call->eraseFromParent();
	}
}

void mergeNoAliasSet(Function &F, DenseSet<CallInst*> &NoAliasSet) {
	DominatorTree DT(F);
	DenseSet<CallInst*> ToDelete;

	for (auto itr1 = NoAliasSet.begin(); itr1 != NoAliasSet.end(); itr1++) {
    for (auto itr2 = std::next(itr1); itr2 != NoAliasSet.end(); itr2++) {

      auto *Call1 = dyn_cast<CallInst>(*itr1);
      auto *Call2 = dyn_cast<CallInst>(*itr2);

      if (ToDelete.count(Call1) || ToDelete.count(Call2)) {
        continue;
      }

      auto Arg0_1 = Call1->getArgOperand(0)->stripPointerCasts();
      auto Arg1_1 = Call1->getArgOperand(1)->stripPointerCasts();
      auto Arg0_2 = Call2->getArgOperand(0)->stripPointerCasts();
      auto Arg1_2 = Call2->getArgOperand(1)->stripPointerCasts();


      if ((Arg0_1 == Arg0_2 && Arg1_1 == Arg1_2) || (Arg0_1 == Arg1_2 && Arg1_1 == Arg0_2)) {
        if (DT.dominates(Call1, Call2)) {
          Call2->replaceAllUsesWith(Call1);
          ToDelete.insert(Call2);
        }
        else if (DT.dominates(Call2, Call1)) {
          Call1->replaceAllUsesWith(Call2);
          ToDelete.insert(Call1);
        }
      }
		}
	}
	for (auto Call : ToDelete) {
		Call->eraseFromParent();
	}
}

void mergeIntrinsics(Function &F) {
	DenseSet<CallInst*> NoaliasSet;
	DenseSet<CallInst*> AlignSet;
	for (auto &BB : F) {
		for (auto &Inst : BB) {
			auto Intrin = dyn_cast<IntrinsicInst>(&Inst);
			if (Intrin) {
				auto ID = Intrin->getIntrinsicID();
				if (ID == Intrinsic::noalias_check) {
					NoaliasSet.insert(Intrin);
				}
				else if (ID == Intrinsic::alignment2k) {
					AlignSet.insert(Intrin);
				}
			}
		}
	}

	mergeAlignment(F, AlignSet);
	mergeNoAliasSet(F, NoaliasSet);
}

void removeInstrumentedFunctions(Function *F, bool ExecuteOptimizedPath, bool ExecuteUnoptimizedPath) {
	DenseSet <Instruction *> InstToRemove;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
	        if (CI && CI->getCalledFunction()) {
	        	if(CI->getCalledFunction()->getName().equals("isNoAliasRTCheck")) {
	        		InstToRemove.insert(BI);
	        	}
	        	else if((!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) 
	        		&& (CI->getCalledFunction()->getName().equals("returnsTrue") 
	        			|| CI->getCalledFunction()->getName().equals("returnsFalse"))) {
	        		InstToRemove.insert(BI);
	        	}
	        }
      	}
  	}

	for(auto I : InstToRemove) {
		assert(I->hasNUses(0));
      	I->eraseFromParent();
	}
}

static Loop *LoopClone(Loop *CLoop, BasicBlock *RuntimeChecksBlock,
	ValueToValueMapTy &VMap, DominatorTree *DT, LoopInfo *LI, bool performCloning) {
	assert(CLoop->getLoopPreheader());
	//Loop cloning logic.
	SmallVector<BasicBlock *, 8> NewLoopBlocks;
	Loop *NewLoop = cloneLoopWithPreheader(CLoop->getLoopPreheader(), 
		CLoop->getLoopPreheader(), CLoop, VMap, 
		".runtime.clone", LI, DT, NewLoopBlocks);
	remapInstructionsInBlocks(NewLoopBlocks, VMap);
	assert(NewLoop->getLoopPreheader());
	
	//Update runtime block terminator. 
	if(performCloning) {
		if(BranchInst *BI = dyn_cast<BranchInst>(RuntimeChecksBlock->getTerminator())) {
		  	assert(BI->isConditional());
		  	SmallVector<pair<unsigned, MDNode *>, 4> MDs;
			BI->getAllMetadata(MDs);
			auto NewBI = BranchInst::Create(CLoop->getLoopPreheader(), 
			  	NewLoop->getLoopPreheader(), BI->getCondition());
			for(const auto &MI : MDs) {
				NewBI->setMetadata(MI.first, MI.second);
			}
			ReplaceInstWithInst(BI, NewBI);
		}
	}
	/*else {
		if(BranchInst *BI = dyn_cast<BranchInst>(RuntimeChecksBlock->getTerminator())) {
		  	assert(BI->isUnconditional());
		  	SmallVector<pair<unsigned, MDNode *>, 4> MDs;
			BI->getAllMetadata(MDs);
			IntegerType *IntType = Type::getInt1Ty(NewLoop->getLoopPreheader()
				->getTerminator()->getContext());
			auto NewBI = BranchInst::Create(CLoop->getLoopPreheader(), 
			  	NewLoop->getLoopPreheader(), ConstantInt::getTrue(IntType));
			for(const auto &MI : MDs) {
				NewBI->setMetadata(MI.first, MI.second);
			}
			ReplaceInstWithInst(BI, NewBI);
		}
	}*/

	return NewLoop;
}

static void addIDMetadata(Loop *CLoop, Loop *NewLoop) {
	auto &Context = CLoop->getHeader()->getTerminator()->getContext();
	//Map instructions from old loop to new loop.
	unsigned count1 = 0;

	for (BasicBlock *BB : CLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *II = &*I;
      		if(trackInstruction(II)) {
	      		MDNode* N = MDNode::get(Context, MDString::get(Context, std::to_string(count1)));
          		(*II).setMetadata("Inst_ID", N);
	      		count1++;
      		}
      	}
	}

    //New loop.
	unsigned count2 = 0;

	for (BasicBlock *BB : NewLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *II = &*I;
      		if(trackInstruction(II)) {
      			MDNode* N = MDNode::get(Context, MDString::get(Context, std::to_string(count2)));
          		(*II).setMetadata("Inst_ID", N);
      			count2++;
      		}
      	}
	}
	
	assert(count1 == count2);
}

void mapInstructions(Loop *CLoop, Loop *NewLoop, 
	DenseMap<Instruction *, unsigned> &OldInstMap, 
	DenseMap<unsigned, Instruction *> &NewInstMap) {
	//auto &Context = CLoop->getHeader()->getTerminator()->getContext();
	for (BasicBlock *BB : CLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *II = &*I;
      		if(trackInstruction(II)) {
	      		if (MDNode* N = (*II).getMetadata("Inst_ID")) {
		  			unsigned ID = stoull(dyn_cast<MDString>(N->getOperand(0))->getString().str());
		  			OldInstMap[II] = ID;
				}
      		}
      	}
	}

    //New loop.
	for (BasicBlock *BB : NewLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *II = &*I;
      		if(trackInstruction(II)) {
      			if (MDNode* N = (*II).getMetadata("Inst_ID")) {
		  			unsigned ID = stoull(dyn_cast<MDString>(N->getOperand(0))->getString().str());
		  			NewInstMap[ID] = II;
				}
      		}
      	}
	}
}

static void addPHINodesForLoop(Loop *CLoop, Loop *NewLoop, 
	ValueToValueMapTy &VMap, LoopInfo *LI) {
	assert(CLoop->getExitBlock());
	auto DefsUsedOutside = findDefsUsedOutsideOfLoop(CLoop);

	//Get the values defined in the preheader and used outside.
	for (BasicBlock::iterator I = CLoop->getLoopPreheader()->begin(); 
		 I != CLoop->getLoopPreheader()->end(); ++I) {
      	Instruction *II = &*I;
      	auto Users = II->users();
       	if (any_of(Users, [&](User *U) {
            auto *Use = cast<Instruction>(U);
            return !CLoop->contains(Use->getParent());
        })) {
       		DefsUsedOutside.push_back(II);
        }
    }

  	BasicBlock *PHIBlock = CLoop->getExitBlock();
  	assert(PHIBlock && "No single successor to loop exit block");
  	PHINode *PN;

  	assert(!PHIBlock->hasNPredecessorsOrMore(3));
  	for (auto *Inst : DefsUsedOutside) {
	    for (auto I = PHIBlock->begin(); (PN = dyn_cast<PHINode>(I)); ++I) {
	      	if (PN->getIncomingValue(0) == Inst) {
	        	break;
	      	}
	    }

	    if (!PN) {
	      	PN = PHINode::Create(Inst->getType(), 2, Inst->getName() + ".lver",
	                           &PHIBlock->front());
	      	SmallVector<User*, 8> UsersToUpdate;
	      	for (User *U : Inst->users()) {
	        	if (!CLoop->contains(cast<Instruction>(U)->getParent()) && 
	        		(cast<Instruction>(U)->getParent() != CLoop->getLoopPreheader())) {
	          		UsersToUpdate.push_back(U);
	        	}
	      	}
	      	for (User *U : UsersToUpdate) {
	        	U->replaceUsesOfWith(Inst, PN);
	      	}
	      	PN->addIncoming(Inst, CLoop->getExitingBlock());
	    }
	}

	for (auto I = PHIBlock->begin(); (PN = dyn_cast<PHINode>(I)); ++I) {
	    Value *ClonedValue = PN->getIncomingValue(0);
	    auto Mapped = VMap.find(ClonedValue);
	    if (Mapped != VMap.end()) {
	      	ClonedValue = Mapped->second;
	    }
		PN->addIncoming(ClonedValue, NewLoop->getExitingBlock());
	}
}

void addScopedNoaliasMetadata(Loop *CLoop, Loop *NewLoop, 
		DenseSet<pair<Instruction *, Instruction *>> Insts, 
		DenseSet<Instruction *> ConflictingLoads, 
		DenseSet<Instruction *> LoadInsts,
		DenseMap<Instruction *, unsigned> OldInstMap, 
		DenseMap<unsigned, Instruction *> NewInstMap) {
	Instruction *I = CLoop->getHeader()->getTerminator();
	assert(I);
	MDBuilder MDB(I->getContext());
  	
  	DenseMap<Instruction *, MDNode *> InstScopeMap;
  	for(auto Pair: Insts) {
  		auto I1 = Pair.first;
  		auto I2 = Pair.second;

  		assert((isa<LoadInst>(I1) && isa<StoreInst>(I2)) 
  			|| (isa<StoreInst>(I1) && isa<StoreInst>(I2)) 
  			|| (isa<StoreInst>(I1) && isa<LoadInst>(I2)) );
  		assert(OldInstMap.count(I1) && OldInstMap.count(I2));

  		if(!NewInstMap.count(OldInstMap[I1]) || !NewInstMap.count(OldInstMap[I2])) {
  			continue;
  		}

  		Instruction *II1 = NewInstMap[OldInstMap[I1]];
  		Instruction *II2 = NewInstMap[OldInstMap[I2]];

  		if(!InstScopeMap.count(II1)) {
  			MDNode *NewDomain = MDB.createAnonymousAliasScopeDomain(
			  					II1->hasName() ? II1->getName() : "AOptDomain");
			StringRef Name = "AOptAliasScope";
			MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, Name);
			InstScopeMap[II1] = NewScope;
  		}

  		assert(InstScopeMap.count(II1));
  		SmallVector<Metadata *, 4> Scope;
		Scope.push_back(InstScopeMap[II1]);

		II1->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II1->getContext(), Scope)));
		II1->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II1->getContext(), Scope)));
		II2->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II2->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II2->getContext(), Scope)));
		II2->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II2->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II2->getContext(), Scope)));

  		/*II1->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II1->getContext(), Scope)));

  		//II2->setMetadata(LLVMContext::MD_alias_scope,
		//	MDNode::concatenate(II2->getMetadata(LLVMContext::MD_alias_scope),
		//	MDNode::get(II2->getContext(), Scope)));

  		II2->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II2->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II2->getContext(), Scope)));*/
  	}

  	/*for (auto I1: ConflictingLoads) {
	  	assert(OldInstMap.count(I1) && NewInstMap.count(OldInstMap[I1]));
	  	Instruction *II1 = NewInstMap[OldInstMap[I1]];
	  	//assert(isa<LoadInst>(I1) && isa<LoadInst>(II1));
  		II1->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II1->getContext(), Scope)));
  		II1->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II1->getContext(), Scope)));
	}*/

#if 0
  	if(ConflictingLoads.size() != LoadInsts.size()) {
  		for(auto I1: ConflictingLoads) {
	  		assert(OldInstMap.count(I1) && NewInstMap.count(OldInstMap[I1]));
	  		Instruction *II1 = NewInstMap[OldInstMap[I1]];
	  		assert(isa<LoadInst>(I1) && isa<LoadInst>(II1));

	  		/*MDBuilder MDB1(II1->getContext());
	  		SmallVector<Metadata *, 4> Scopes;
	  		MDNode *NewDomain1 = MDB1.createAnonymousAliasScopeDomain(
	  				II1->hasName() ? II1->getName() : "AOptDomain");
			StringRef Name1 = "AOptAliasScope";
			MDNode *NewScope1 = MDB1.createAnonymousAliasScope(NewDomain1, Name1);
			Scopes.push_back(NewScope1);*/

			II1->setMetadata(LLVMContext::MD_noalias,
				MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
				MDNode::get(II1->getContext(), Scopes)));	

				  	II1->setMetadata(LLVMContext::MD_alias_scope,
						MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
						MDNode::get(II1->getContext(), Scopes)));

			for(auto I2 : LoadInsts) {
				if(I1 != I2) {
					assert(OldInstMap.count(I2) && NewInstMap.count(OldInstMap[I2]));

				  	Instruction *II2 = NewInstMap[OldInstMap[I2]];
				  	assert(isa<LoadInst>(I2) && isa<LoadInst>(II2));

				  	II2->setMetadata(LLVMContext::MD_alias_scope,
						MDNode::concatenate(II2->getMetadata(LLVMContext::MD_alias_scope),
						MDNode::get(II2->getContext(), Scopes)));
			II2->setMetadata(LLVMContext::MD_noalias,
				MDNode::concatenate(II2->getMetadata(LLVMContext::MD_noalias),
				MDNode::get(II2->getContext(), Scopes)));	
				}
			}  
	  	}
  	}
#endif
}

void convertCallToCmp(BasicBlock *RuntimeChecksBlock) {
	for (BasicBlock::iterator I = RuntimeChecksBlock->begin(); I != RuntimeChecksBlock->end(); ++I) {
		Instruction *BI = &*I;
		CallInst *CI =  dyn_cast<CallInst>(BI);
		if(CI && CI->getCalledFunction() && CI->getCalledFunction()->hasName()) {
			if(CI->getCalledFunction()->getName().equals("isNoAliasRTCheck")) {
				assert(CI->getNumOperands() == 3);
				assert(CI->getOperand(0) && CI->getOperand(1));
				Value *O1 = CI->getOperand(0);
				Value *O2 = CI->getOperand(1);
				assert(O1->getType() == O2->getType());
				IRBuilder<> IRB(CI);
		        Value *CmpEq = IRB.CreateICmp(ICmpInst::ICMP_NE, O1, O2);
		        CI->replaceAllUsesWith(CmpEq);
			}
		}
	}
}

void instrumentWriteFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!isWriteDefAdded && F->getName().equals("main")) {
    	string StatsFile = FileName + "_LoopTakenStats";
	    Constant* strConstant = ConstantDataArray::getString(M->getContext(), StatsFile.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);
	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		writeLoopCounterFunc = Function::Create(FTy2,
				        GlobalValue::ExternalLinkage,
				        "writeLoopCounter", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeLoopCounterFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeLoopCounterFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	isWriteDefAdded = true;
    }
}

void instrumentLoopIterStatsFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!isWriteLoopIterStatsDefined && F->getName().equals("main")) {
    	string StatsFile = FileName + "_LoopIterStats";
	    Constant* strConstant = ConstantDataArray::getString(M->getContext(), StatsFile.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);
	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		writeLoopIterStatsFunc = Function::Create(FTy2,
				        GlobalValue::ExternalLinkage,
				        "writeLoopIterStats", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeLoopIterStatsFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeLoopIterStatsFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	isWriteLoopIterStatsDefined = true;
    }
}

void instrumentTimeWriteFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!isTimeWriteDefAdded && F->getName().equals("main")) {
    	string StatsFile = FileName + "_LoopTimeStats";
    	Constant* strConstant = ConstantDataArray::getString(M->getContext(), StatsFile.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);

	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy = FunctionType::get(VoidType, ArgTypes, false);
		writeTimeStatsFunc = Function::Create(FTy,
				        GlobalValue::ExternalLinkage,
				        "writeTimeStats", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeTimeStatsFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeTimeStatsFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	isTimeWriteDefAdded = true;
    }
}

void instrumentSameMACountWriteFunc(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!iswriteSameMACountFuncDefined && F->getName().equals("main")) {
    	string StatsFile = FileName + "_same_macount.txt";
    	Constant* strConstant = ConstantDataArray::getString(M->getContext(), StatsFile.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);

	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy = FunctionType::get(VoidType, ArgTypes, false);
		writeSameMACountFunc = Function::Create(FTy,
				        GlobalValue::ExternalLinkage,
				        "writeSameMACount", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeSameMACountFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeSameMACountFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	iswriteSameMACountFuncDefined = true;
    }
}

void instrumentFuncTimeWriteFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!isFuncTimeWriteDefAdded && F->getName().equals("main")) {
    	string StatsFile = FileName + "_FuncTimeStats";
    	Constant* strConstant = ConstantDataArray::getString(M->getContext(), StatsFile.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);

	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy = FunctionType::get(VoidType, ArgTypes, false);
		writeFuncTimeStatsFunc = Function::Create(FTy,
				        GlobalValue::ExternalLinkage,
				        "writeFuncStats", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeFuncTimeStatsFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeFuncTimeStatsFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	isFuncTimeWriteDefAdded = true;
    }
}

void instrumentDebugFunction(Loop *CLoop, unsigned index, unsigned LoopID) {
	assert(CLoop->getLoopPreheader());
	Function *F = CLoop->getLoopPreheader()->getParent();
	Module *M = F->getParent();
  	Type *VoidType = Type::getVoidTy(M->getContext());
  	//IntegerType *Int1Type = Type::getInt1Ty(M->getContext());
  	IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	//IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	//PointerType *CharStarType = PointerType::getUnqual(CharType);
  	
  	if(!isDebugFuncDefAdded) {
    	std::vector<Type *> ArgTypes;
	  	ArgTypes.push_back(IntType);
	  	ArgTypes.push_back(IntType);
	  	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
		updateLoopCounterFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "updateLoopCounter", M);

		ArgTypes.clear();
		FunctionType *FTy = FunctionType::get(VoidType, ArgTypes, false);
		exitFunc = Function::Create(FTy,
			GlobalValue::ExternalLinkage, "callExit", M);

  		isDebugFuncDefAdded = true;
  	}

    std::vector<Value *> ArgValue;
	ArgValue.push_back(ConstantInt::get(IntType, LoopID));
	ArgValue.push_back(ConstantInt::get(IntType, index));
	CallInst::Create(updateLoopCounterFunc, ArgValue, "", CLoop->getLoopPreheader()->getTerminator());

	/*if(index == 0) {
		ArgValue.clear();
		CallInst::Create(exitFunc, ArgValue, "", CLoop->getLoopPreheader()->getTerminator());
	}*/
}

void instrumentTimeFunction(Function *F, BasicBlock *StartTimeBlock, 
	BasicBlock *EndTimeBlock, unsigned LoopID) {
	assert(StartTimeBlock && EndTimeBlock);
	//Function *F = StartTimeBlock->getParent();
	Module *M = F->getParent();

  	Type *VoidType = Type::getVoidTy(M->getContext());
  	IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	
  	if(!isTimeFuncDefAdded) {
    	std::vector<Type *> ArgTypes;
	  	ArgTypes.push_back(IntType);
	  	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
		startTimeFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "rdtsc_s", M);

		FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		endTimeFunc = Function::Create(FTy2,
						GlobalValue::ExternalLinkage, 
						"rdtsc_e", M);

  		isTimeFuncDefAdded = true;
  	}

  	Instruction *InsertHere = StartTimeBlock->getTerminator();
  	for (BasicBlock::iterator I = StartTimeBlock->begin(); I != StartTimeBlock->end(); ++I) {
		Instruction *BI = &*I;
		if(isa<CmpInst>(BI)) {
			InsertHere = BI;
			break;
		}
		else if (isa<IntrinsicInst>(BI) &&
			cast<IntrinsicInst>(BI)->getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
			InsertHere = BI;
			break;
		}
	}
	//Instruction *InsertHere = StartTimeBlock->getFirstNonPHI();
	assert(InsertHere);

	PostDominatorTree PDT(*F);
    BasicBlock *EndBlock = NULL;
	std::vector<BasicBlock *> Worklist;
	if(BranchInst *BI = dyn_cast<BranchInst>(StartTimeBlock->getTerminator())) {
		for(unsigned i = 0; i < BI->getNumSuccessors(); i++) {
			Worklist.push_back(BI->getSuccessor(i));
		}
	}

    DenseSet<BasicBlock *> Visited;
	while(!Worklist.empty()) {
	    BasicBlock *CurBlock = Worklist.back();
	    Worklist.pop_back();
	    if(Visited.size() > 500) {
	        assert(0 && "End block not found.");
	    }

	    if(Visited.count(CurBlock)) {
	        continue;
	    }
	    Visited.insert(CurBlock);

	    if(PDT.dominates(CurBlock, StartTimeBlock)) {
	        Worklist.clear();
	        EndBlock = CurBlock;
	    }
	    else {
	        if(BranchInst *BI = dyn_cast<BranchInst>(CurBlock->getTerminator())) {
				for(unsigned i = 0; i < BI->getNumSuccessors(); i++) {
					Worklist.push_back(BI->getSuccessor(i));
				}
			}

			if(SwitchInst *SI = dyn_cast<SwitchInst>(CurBlock->getTerminator())) {
				for(unsigned i = 0; i < SI->getNumSuccessors(); i++) {
					Worklist.push_back(SI->getSuccessor(i));
				}
			}
	    }
	}
	assert(EndBlock);

	Instruction *InsertExitBefore = EndBlock->getTerminator();
	for (BasicBlock::iterator I = EndBlock->begin(); I != EndBlock->end(); ++I) {
        Instruction *BI = &*I;
        CallInst *CI = dyn_cast<CallInst>(BI);
	    if (CI && CI->getCalledFunction()) {
	       	if(CI->getCalledFunction()->getName().equals("rdtsc_s")) {
	        	InsertExitBefore = CI;
	        	break;
	        }
	    }
    }
    assert(InsertExitBefore);

    std::vector<Value *> ArgValue;
	ArgValue.push_back(ConstantInt::get(IntType, LoopID));
	CallInst::Create(startTimeFunc, ArgValue, "", InsertHere);
	CallInst::Create(endTimeFunc, ArgValue, "", InsertExitBefore);
}

void instrumentFuncTimeFunction(Function *F, unsigned FuncID) {
	Module *M = F->getParent();

  	Type *VoidType = Type::getVoidTy(M->getContext());
  	IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	
  	if(!isFuncTimeFuncDefAdded) {
    	std::vector<Type *> ArgTypes;
	  	ArgTypes.push_back(IntType);
	  	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
		startFuncTimeFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "rdtsc_s_f", M);

		FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		endFuncTimeFunc = Function::Create(FTy2,
						GlobalValue::ExternalLinkage, 
						"rdtsc_e_f", M);

  		isFuncTimeFuncDefAdded = true;
  	}

  	bool addedAtEntry = false;
  	for (BasicBlock::iterator I = F->getEntryBlock().begin(); I != F->getEntryBlock().end(); ++I) {
  		Instruction *BI = &*I;
  		CallInst *CI = dyn_cast<CallInst>(BI);
	    if (CI && CI->getCalledFunction()) {
	       	if(CI->getCalledFunction()->getName().equals("rdtsc_s_f")) {
	        	addedAtEntry = true;
	        	break;
	        }
	    }
  	}
  	
  	std::vector<Value *> ArgValue;
	ArgValue.push_back(ConstantInt::get(IntType, FuncID));
  	if(!addedAtEntry) {
  		CallInst::Create(startFuncTimeFunc, ArgValue, "", &*(F->getEntryBlock().getFirstInsertionPt()));
  	}

  	bool addedAtEnd = false;
  	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		    Instruction *II = &*I;
		    CallInst *CI = dyn_cast<CallInst>(II);
		    if (CI && CI->getCalledFunction()) {
		       	if(CI->getCalledFunction()->getName().equals("rdtsc_e_f")) {
		        	addedAtEnd = true;
		        	break;
		        }
		    }
		}
	}

	if(!addedAtEnd) {
		for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
			for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
			    Instruction *II = &*I;
			    if (isa<ReturnInst>(II)) {
			      	CallInst::Create(endFuncTimeFunc, ArgValue, "", II);
			    }

			    if (CallInst *CI = dyn_cast<CallInst>(II)) {
			       	Function *CalledFunc = CI->getCalledFunction();
			        if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
			        	CallInst::Create(endFuncTimeFunc, ArgValue, "", II);
			        }
			    }
			}
		}
	}
}

void restoreLoop(BasicBlock *RuntimeChecksBlock) {
	BranchInst *BI = dyn_cast<BranchInst>(RuntimeChecksBlock->getTerminator());
	assert(BI && BI->isConditional());
	IntegerType *IntType = Type::getInt1Ty(RuntimeChecksBlock->getTerminator()
				->getModule()->getContext());
	BI->setCondition(ConstantInt::getTrue(IntType));
}

void printNoAliasInfo(Function &F, AAResults *AA)
{
	errs() << "Printing start\n";
	DenseSet<Instruction*> LoadSet, StoreSet;
	for (auto &BB : F) {
		for (auto &II : BB) {
			if (isa<LoadInst>(&II)) {
				LoadSet.insert(cast<Instruction>(&II));
			}
			if (isa<StoreInst>(&II)) {
				StoreSet.insert(cast<Instruction>(&II));
			}
		}
	}

	for (auto LI1 : LoadSet) {
		for (auto LI2 : LoadSet) {
			if (LI1 != LI2) {
				errs() << "LI1: " << *LI1 << " LI2: " << *LI2;
	    		MemoryLocation Loc1 = getMemLoc(LI1);
	    		assert(Loc1.Ptr);
				MemoryLocation Loc2 = getMemLoc(LI2);
				assert(Loc2.Ptr);
				AliasResult AR = AA->alias(Loc1, Loc2);
				if (AR == MustAlias) {
					errs() << " MustAlias\n";
				}
				else if (AR == NoAlias) {
					errs() << " NoAlias\n";
				}
				else {
					errs() << " MayAlias\n";
				}
			}
		}

		for (auto LI2 : StoreSet) {
			if (LI1 != LI2) {
				errs() << "LI1: " << *LI1 << " LI2: " << *LI2;
	    		MemoryLocation Loc1 = getMemLoc(LI1);
	    		assert(Loc1.Ptr);
				MemoryLocation Loc2 = getMemLoc(LI2);
				assert(Loc2.Ptr);
				AliasResult AR = AA->alias(Loc1, Loc2);
				if (AR == MustAlias) {
					errs() << " MustAlias\n";
				}
				else if (AR == NoAlias) {
					errs() << " NoAlias\n";
				}
				else {
					errs() << " MayAlias\n";
				}
			}
		}
	}

	for (auto LI1 : StoreSet) {
		for (auto LI2 : StoreSet) {
			if (LI1 != LI2) {
				errs() << "LI1: " << *LI1 << " LI2: " << *LI2;
	    		MemoryLocation Loc1 = getMemLoc(LI1);
	    		assert(Loc1.Ptr);
				MemoryLocation Loc2 = getMemLoc(LI2);
				assert(Loc2.Ptr);
				AliasResult AR = AA->alias(Loc1, Loc2);
				if (AR == MustAlias) {
					errs() << " MustAlias\n";
				}
				else if (AR == NoAlias) {
					errs() << " NoAlias\n";
				}
				else {
					errs() << " MayAlias\n";
				}
			}
		}
	}

	errs() << F << "\n";
	errs() << "Printing done\n";
}

void addMetadata(Loop *L, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<Instruction *> ConflictingLoads, DenseSet<Instruction *> AllLoads,
	DenseSet<Instruction *> AllStores, 
	DenseMap<Instruction *, unsigned> OldInstMap, 
	DenseMap<unsigned, Instruction *> NewInstMap, 
	DenseMap<Instruction *, DenseSet<MDNode *>> &LoadDomainMap, 
	DenseMap<Instruction *, DenseSet<MDNode *>> &ConflictingLoadDomainMap, 
	DenseMap<Instruction *, DenseSet<MDNode *>> &StoreDomainMap, AAResults *AA) {
	Instruction *I = L->getHeader()->getTerminator();
	assert(I);
	MDBuilder MDB(I->getContext());

	for(auto Pair : InstSet) {
		auto I1 = Pair.first;
		auto I2 = Pair.second;

		assert(OldInstMap.count(I1) && OldInstMap.count(I2));
		assert(NewInstMap.count(OldInstMap[I1]) && NewInstMap.count(OldInstMap[I2]));

		auto II1 = NewInstMap[OldInstMap[I1]];
		auto II2 = NewInstMap[OldInstMap[I2]];

		MDNode *NewDomain = MDB.createAnonymousAliasScopeDomain("AOptDomain");
		MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, "AOptAliasScope");
		SmallVector<Metadata *, 4> Scope;
		Scope.push_back(NewScope);

		if(isa<LoadInst>(I1)) {
			LoadDomainMap[I1].insert(NewScope);
		}

		if(isa<StoreInst>(I1)) {
			StoreDomainMap[I1].insert(NewScope);
		}

		II1->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II1->getContext(), Scope)));
	    II1->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II1->getContext(), Scope)));

	    II2->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II2->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II2->getContext(), Scope)));
	    II2->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II2->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II2->getContext(), Scope)));
	}

	for(auto I1 : ConflictingLoads) {
		MemoryLocation Loc1 = getMemLoc(I1);
		assert(Loc1.Ptr);
		assert(OldInstMap.count(I1) && NewInstMap.count(OldInstMap[I1]));
		auto II1 = NewInstMap[OldInstMap[I1]];

		SmallVector<Metadata *, 4> Scope;
		DenseSet<Instruction *> MustAliasInst;
		for(auto I2 : AllStores) {
			MemoryLocation Loc2 = getMemLoc(I2);
			assert(Loc2.Ptr);

			AliasResult AR = AA->alias(Loc1, Loc2);
			if(AR == MustAlias) {
				assert(OldInstMap.count(I2) && NewInstMap.count(OldInstMap[I2]));
				auto II2 = NewInstMap[OldInstMap[I2]];
				if(auto M = II2->getMetadata(LLVMContext::MD_alias_scope)) {
					for (const MDOperand &MDOp : M->operands()) {
			    		if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
			    			ConflictingLoadDomainMap[I1].insert(const_cast<MDNode *>(MD));
			    			Scope.push_back(const_cast<MDNode *>(MD));
			    		}
			    	}
				}
				MustAliasInst.insert(II2);
			}
		}

		for(auto I2 : AllLoads) {
			if(I1 != I2) {
				MemoryLocation Loc2 = getMemLoc(I2);
				assert(Loc2.Ptr);
				AliasResult AR = AA->alias(Loc1, Loc2);
				if(AR == MustAlias) {
					assert(OldInstMap.count(I2) && NewInstMap.count(OldInstMap[I2]));
					auto II2 = NewInstMap[OldInstMap[I2]];
					if(auto M = II2->getMetadata(LLVMContext::MD_alias_scope)) {
						for (const MDOperand &MDOp : M->operands()) {
				    		if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
				    			ConflictingLoadDomainMap[I1].insert(const_cast<MDNode *>(MD));
				    			Scope.push_back(const_cast<MDNode *>(MD));
				    		}
				    	}
					}
					MustAliasInst.insert(II2);
				}
			}
		}

		II1->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(II1->getContext(), Scope)));
		II1->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(II1->getContext(), Scope)));

		Scope.clear();
		if(auto M = II1->getMetadata(LLVMContext::MD_alias_scope)) {
			for (const MDOperand &MDOp : M->operands()) {
				if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
				    Scope.push_back(const_cast<MDNode *>(MD));
				}
			}
		}

		for(auto II2 : MustAliasInst) {
			auto M = MDNode::get(II2->getContext(), Scope);
			II2->setMetadata(LLVMContext::MD_alias_scope, NULL);
			II2->setMetadata(LLVMContext::MD_noalias, NULL);

			II2->setMetadata(LLVMContext::MD_alias_scope, M);
			II2->setMetadata(LLVMContext::MD_noalias, M);

			//For verification purpose.
			SmallVector<Metadata *, 4> Scope1;
			if(auto M = II2->getMetadata(LLVMContext::MD_alias_scope)) {
				for (const MDOperand &MDOp : M->operands()) {
					if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
					    Scope1.push_back(const_cast<MDNode *>(MD));
					}
				}
			}
			assert(Scope.size() == Scope1.size());
		}

		/*for(auto I2 : AllLoads) {
			if(I1 != I2) {
				assert(OldInstMap.count(I2) && NewInstMap.count(OldInstMap[I2]));
				auto II2 = NewInstMap[OldInstMap[I2]];

				MDNode *NewDomain = MDB.createAnonymousAliasScopeDomain("AOptDomain");
				MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, "AOptAliasScope");
				SmallVector<Metadata *, 4> Scope;
				Scope.push_back(NewScope);

				ConflictingLoadDomainMap[I1].insert(NewScope);

				II1->setMetadata(LLVMContext::MD_alias_scope,
					MDNode::concatenate(II1->getMetadata(LLVMContext::MD_alias_scope),
					MDNode::get(II1->getContext(), Scope)));
			    II1->setMetadata(LLVMContext::MD_noalias,
					MDNode::concatenate(II1->getMetadata(LLVMContext::MD_noalias),
					MDNode::get(II1->getContext(), Scope)));

				II2->setMetadata(LLVMContext::MD_alias_scope,
					MDNode::concatenate(II2->getMetadata(LLVMContext::MD_alias_scope),
					MDNode::get(II2->getContext(), Scope)));
			    II2->setMetadata(LLVMContext::MD_noalias,
					MDNode::concatenate(II2->getMetadata(LLVMContext::MD_noalias),
					MDNode::get(II2->getContext(), Scope)));
			}
		}*/
	}
}

void removeMetadataFromAll(Loop *CLoop) {
	for (BasicBlock::iterator I = CLoop->getLoopPreheader()->begin(); 
		I != CLoop->getLoopPreheader()->end(); ++I) {
	    Instruction *BI = &*I;
		if (BI->getMetadata(LLVMContext::MD_alias_scope)) {
			BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		}
		if (BI->getMetadata(LLVMContext::MD_noalias)) {
			BI->setMetadata(LLVMContext::MD_noalias, NULL);
		}
	}

	for(BasicBlock *BB : CLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
	      	Instruction *BI = &*I;
	      	if (BI->getMetadata(LLVMContext::MD_alias_scope)) {
				BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
			}
			if (BI->getMetadata(LLVMContext::MD_noalias)) {
				BI->setMetadata(LLVMContext::MD_noalias, NULL);
			}
	    }
	}

	for (BasicBlock::iterator I = CLoop->getExitBlock()->begin(); 
		I != CLoop->getExitBlock()->end(); ++I) {
	    Instruction *BI = &*I;
		if (BI->getMetadata(LLVMContext::MD_alias_scope)) {
			BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		}
		if (BI->getMetadata(LLVMContext::MD_noalias)) {
			BI->setMetadata(LLVMContext::MD_noalias, NULL);
		}
	}
}

static void addDummyFunctionCall(Loop *L, Loop *NewLoop, BasicBlock *RuntimeChecksBlock, 
	bool ExecuteOptimizedPath, bool ExecuteUnoptimizedPath) {
	Instruction *TermInst = RuntimeChecksBlock->getTerminator();
  	assert(TermInst && "Terminator instruction not found for runtime checks block.");

  	Module *M = RuntimeChecksBlock->getParent()->getParent();
  	//Type *VoidType = Type::getVoidTy(M->getContext());
  	IntegerType *Int1Type = Type::getInt1Ty(M->getContext());
  	//IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	//IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	//PointerType *CharStarType = PointerType::getUnqual(CharType);

  	IRBuilder<> IRB1(TermInst);

  	if(!isDummyFuncDefAdded) {
  		vector<Type *> ArgTypes;
		FunctionType *FTy1 = FunctionType::get(Int1Type, ArgTypes, false);
		isDummyTrueFunc = Function::Create(FTy1,
			GlobalValue::ExternalLinkage, "returnsTrue", M);
		isDummyTrueFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadNone);

		FunctionType *FTy2 = FunctionType::get(Int1Type, ArgTypes, false);
		isDummyFalseFunc = Function::Create(FTy2,
			GlobalValue::ExternalLinkage, "returnsFalse", M);
		isDummyFalseFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadNone);
		isDummyFuncDefAdded = true;
  	}

  	assert(isDummyTrueFunc);
  	assert(isDummyFalseFunc);
  	Value *Return = nullptr;
  	if(ExecuteOptimizedPath) {
  		Return = IRB1.CreateCall(isDummyFalseFunc);
  	}
  	else {
  		Return = IRB1.CreateCall(isDummyTrueFunc);
  	}

  	assert(Return);
  	//Value *Return = IRB1.CreateCall(isDummyTrueFunc);
  	if(BranchInst *BI = dyn_cast<BranchInst>(RuntimeChecksBlock->getTerminator())) {
		assert(BI->isUnconditional());
		SmallVector<pair<unsigned, MDNode *>, 4> MDs;
		BI->getAllMetadata(MDs);
		//IntegerType *IntType = Type::getInt1Ty(NewLoop->getLoopPreheader()
			//	->getTerminator()->getContext());
		auto NewBI = BranchInst::Create(L->getLoopPreheader(), 
			  	NewLoop->getLoopPreheader(), Return);
		for(const auto &MI : MDs) {
			NewBI->setMetadata(MI.first, MI.second);
		}
		ReplaceInstWithInst(BI, NewBI);
	}
}

void getAllLoadStore(Loop *L, DenseSet<Instruction *> 
	&AllLoads, DenseSet<Instruction *> &AllStores) {
	for (BasicBlock *BB : L->blocks()) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		    Instruction *BI = &*I;
		    if(isa<LoadInst>(BI)) {
		    	AllLoads.insert(BI);
		    }
		    if(isa<StoreInst>(BI)) {
		    	AllStores.insert(BI);
		    }
		}
	}
}

bool getBasesPair(Loop *L, MemoryLocation Loc1, MemoryLocation Loc2, 
	DenseSet<pair<Value *, Value *>> &Bases, LoopInfo *LI, 
	DominatorTree *DT, DenseSet<Value *> &SafeBases, bool first, bool second) {
	assert(Loc1.Ptr && Loc2.Ptr);

	SmallVector<const Value *, 4> Objects1, Objects2;
	getBases(L, const_cast<Value*>(Loc1.Ptr), Objects1, 
			LI, DT);
	if(Objects1.size() == 0) {
		return false;
	}

	getBases(L, const_cast<Value*>(Loc2.Ptr), Objects2, 
			LI, DT);
	if(Objects2.size() == 0) {
		return false;
	}
		
	assert(Objects1.size() > 0 && Objects2.size() > 0);

	for (auto Obj1 : Objects1) {
		if (Obj1 && !isa<Constant>(Obj1)) {
			Value *V1 = const_cast<Value*>(Obj1);
			if(first) {
				SafeBases.insert(V1);
			}

			for (auto Obj2 : Objects2) {
				if (Obj2 && !isa<Constant>(Obj2)) {
					Value *V2 = const_cast<Value*>(Obj2);

					if(V1 == V2) {
						return false;
					}

					if(second) {
						SafeBases.insert(V2);
					}
					auto P1 = make_pair(V1, V2);
					auto P2 = make_pair(V2, V1);
					if(!Bases.count(P1) && !Bases.count(P2)) {
						Bases.insert(first ? P1 : P2);
					}
				}
			}
		}
	}

	if(Bases.size() == 0) {
		return false;
	}
	return true;
}

bool LoadDep(Loop *L, Instruction *Load, DenseSet<Instruction *> AllLoads, 
	DenseSet<Instruction *> AllStores, 
	DenseSet<Instruction *> &ConflictingLoads, 
	DenseSet<pair<Instruction *, Instruction *>> &InstSet, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, DenseSet<Value *> &SafeBases, 
	DenseSet<Instruction *> &ConflictingStores) {
	PostDominatorTree PDT(*Load->getFunction());

	DenseSet<pair<Instruction *, Instruction *>> TempInstSet; 
	DenseSet<pair<Value *, Value *>> TempBases;
	DenseSet<Value *> TempSafeBases;
	MemoryLocation Loc1 = getMemLoc(Load);
	if(!Loc1.Ptr) {
	   	return false;
	}
	if(isa<Constant>(Loc1.Ptr)) {
		return false;
	}
	for(auto I : AllStores) {
	    MemoryLocation Loc2 = getMemLoc(I);
		if(!Loc2.Ptr) {
		    return false;
		}
		if(isa<Constant>(Loc2.Ptr)) {
			return false;
		}
		AliasResult AR = AA->alias(Loc1, Loc2);
		if(AR == MustAlias) {
			ConflictingLoads.insert(Load);
		}
		if(AR == MayAlias) {
			if(!getBasesPair(L, Loc1, Loc2, TempBases, LI, DT, TempSafeBases, 
				true, true)) {
				return false;
			}

			auto P = make_pair(Load, I);
			TempInstSet.insert(P);
		}	
	}

	if(TempInstSet.size() == 0) {
		return false;
	}

	InstSet.insert(TempInstSet.begin(), TempInstSet.end());
	Bases.insert(TempBases.begin(), TempBases.end());
	SafeBases.insert(TempSafeBases.begin(), TempSafeBases.end());

	if(!ConflictingLoads.count(Load)) {
		for(auto I : AllLoads) {
			if(Load != I) {
				MemoryLocation Loc2 = getMemLoc(I);
				if(!Loc2.Ptr) {
				    return false;
				}
				AliasResult AR = AA->alias(Loc1, Loc2);
				if(AR == MustAlias) {
					ConflictingLoads.insert(Load);
					return true;
				}
			}	
		}
	}

	return true;
}

void StoreDep(Loop *L, Instruction *Store, DenseSet<Instruction *> AllStores,
	DenseSet<pair<Instruction *, Instruction *>> &InstSet, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, DenseSet<Value *> &SafeBases) {
	PostDominatorTree PDT(*Store->getFunction());
	DenseSet<pair<Instruction *, Instruction *>> TempInstSet; 
	DenseSet<pair<Value *, Value *>> TempBases;
	DenseSet<Value *> TempSafeBases;
	MemoryLocation Loc1 = getMemLoc(Store);
	if(!Loc1.Ptr) {
	   	return;
	}
	if(isa<Constant>(Loc1.Ptr)) {
		return;
	}
	for(auto I : AllStores) {
	    MemoryLocation Loc2 = getMemLoc(I);
		if(!Loc2.Ptr) {
		    return;
		}
		if(isa<Constant>(Loc2.Ptr)) {
			return;
		}
		AliasResult AR = AA->alias(Loc1, Loc2);
		if(AR == MayAlias) {
			if(!getBasesPair(L, Loc1, Loc2, TempBases, LI, DT, TempSafeBases, 
				true, true)) {
				return;
			}

			auto Pair = make_pair(Store, I);
			TempInstSet.insert(Pair);
		}	
	}

	InstSet.insert(TempInstSet.begin(), TempInstSet.end());
	Bases.insert(TempBases.begin(), TempBases.end());
	SafeBases.insert(TempSafeBases.begin(), TempSafeBases.end());
}

void getLoadStoreSet(Loop *L, Loop *NewLoop, 
	DenseSet<Instruction *> &LoadSet, 
	DenseSet<Instruction *> &StoreSet) {
	DenseSet<unsigned> Found;
	for (BasicBlock *BB : NewLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
	  		Instruction *II = &*I;
	  		if(isa<LoadInst>(II) || isa<StoreInst>(II)) {
	  			MDNode* N = (*II).getMetadata("Inst_ID");
	  			assert(N);
	  			unsigned ID = stoull(dyn_cast<MDString>(N->getOperand(0))->getString().str());
	  			Found.insert(ID);
	  		}
	  	}
	}

	for (BasicBlock *BB : L->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
	  		Instruction *II = &*I;
	  		if(isa<LoadInst>(II) || isa<StoreInst>(II)) {
	  			MDNode* N = (*II).getMetadata("Inst_ID");
	  			assert(N);
	  			unsigned ID = stoull(dyn_cast<MDString>(N->getOperand(0))->getString().str());
	  			if(!Found.count(ID)) {
	  				if(isa<LoadInst>(II)) {
				    	LoadSet.insert(II);
				    }
				    if(isa<StoreInst>(II)) {
				    	StoreSet.insert(II);
				    }
	  			}
	  		}
	  	}
	}
}

void removeDomain(Loop *L, DenseSet<MDNode *> Domains) {
	assert(L->getLoopPreheader());
	for (BasicBlock::iterator I = L->getLoopPreheader()->begin(); I != L->getLoopPreheader()->end(); ++I) {
		Instruction *BI = &*I;
		if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
		   	SmallVector<Metadata *, 4> Scope;
		    for (const MDOperand &MDOp : M->operands()) {
		    	if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
		    		if(!Domains.count(MD)) {
		    			Scope.push_back(const_cast<MDNode *>(MD));
		    		}
		    	}
		    }

		    BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		    BI->setMetadata(LLVMContext::MD_noalias, NULL);

		    if(Scope.size() > 0) {
		    	BI->setMetadata(LLVMContext::MD_alias_scope,
					MDNode::concatenate(BI->getMetadata(LLVMContext::MD_alias_scope),
					MDNode::get(BI->getContext(), Scope)));
				BI->setMetadata(LLVMContext::MD_noalias,
					MDNode::concatenate(BI->getMetadata(LLVMContext::MD_noalias),
					MDNode::get(BI->getContext(), Scope)));
		    }
		}
	}

	for (BasicBlock *BB : L->blocks()) {
		for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		    Instruction *BI = &*I;
		    if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
		    	SmallVector<Metadata *, 4> Scope;
		    	for (const MDOperand &MDOp : M->operands()) {
		    		if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
		    			if(!Domains.count(MD)) {
		    				Scope.push_back(const_cast<MDNode *>(MD));
		    			}
		    		}
		    	}

		    	BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		    	BI->setMetadata(LLVMContext::MD_noalias, NULL);

		    	if(Scope.size() > 0) {
		    		BI->setMetadata(LLVMContext::MD_alias_scope,
						MDNode::concatenate(BI->getMetadata(LLVMContext::MD_alias_scope),
						MDNode::get(BI->getContext(), Scope)));
				    BI->setMetadata(LLVMContext::MD_noalias,
						MDNode::concatenate(BI->getMetadata(LLVMContext::MD_noalias),
						MDNode::get(BI->getContext(), Scope)));
		    	}
			}
		}
	}

	assert(L->getExitBlock());
	for (BasicBlock::iterator I = L->getExitBlock()->begin(); I != L->getExitBlock()->end(); ++I) {
		Instruction *BI = &*I;
		if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
		   	SmallVector<Metadata *, 4> Scope;
		    for (const MDOperand &MDOp : M->operands()) {
		    	if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
		    		if(!Domains.count(MD)) {
		    			Scope.push_back(const_cast<MDNode *>(MD));
		    		}
		    	}
		    }

		    BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		    BI->setMetadata(LLVMContext::MD_noalias, NULL);

		    if(Scope.size() > 0) {
		    	BI->setMetadata(LLVMContext::MD_alias_scope,
					MDNode::concatenate(BI->getMetadata(LLVMContext::MD_alias_scope),
					MDNode::get(BI->getContext(), Scope)));
				BI->setMetadata(LLVMContext::MD_noalias,
					MDNode::concatenate(BI->getMetadata(LLVMContext::MD_noalias),
					MDNode::get(BI->getContext(), Scope)));
		    }
		}
	}
}

void removeConflictingLoadDomain(Instruction *I, DenseSet<MDNode *> Domains) {
	SmallVector<Metadata *, 4> Scope;
	if(auto M = I->getMetadata(LLVMContext::MD_alias_scope)) {
		for (const MDOperand &MDOp : M->operands()) {
		   	if (const MDNode *MD = dyn_cast<MDNode>(MDOp)) {
		    	if(!Domains.count(MD)) {
		    		Scope.push_back(const_cast<MDNode *>(MD));
		    	}
		    }
		}
	}

	I->setMetadata(LLVMContext::MD_alias_scope, NULL);
	I->setMetadata(LLVMContext::MD_noalias, NULL);

	if(Scope.size() > 0) {
		I->setMetadata(LLVMContext::MD_alias_scope,
			MDNode::concatenate(I->getMetadata(LLVMContext::MD_alias_scope),
			MDNode::get(I->getContext(), Scope)));
		I->setMetadata(LLVMContext::MD_noalias,
			MDNode::concatenate(I->getMetadata(LLVMContext::MD_noalias),
			MDNode::get(I->getContext(), Scope)));
	}
}

void removeDummyFunctionCall(Function *F) {
	IntegerType *IntType = Type::getInt1Ty(F->getContext());

	DenseSet <Instruction *> InstToRemove;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
	        if (CI && CI->getCalledFunction()) {
	        	if(CI->getCalledFunction()->getName().equals("returnsTrue")) {
	        		CI->replaceAllUsesWith(ConstantInt::getTrue(IntType));
	        		InstToRemove.insert(BI);
	        	}

	        	if(CI->getCalledFunction()->getName().equals("returnsFalse")) {
	        		CI->replaceAllUsesWith(ConstantInt::getFalse(IntType));
	        		InstToRemove.insert(BI);
	        	}
	        }
      	}
  	}

	for(auto I : InstToRemove) {
		assert(I->hasNUses(0));
      	I->eraseFromParent();
	}
}

/*unsigned getLoopID(string filename) {
	const char *LogDirEnv = getenv("HOME");
	assert(LogDirEnv);
	string LogDir = LogDirEnv;
	LogDir = LogDir + "/stats/" + filename + "_UniqueID.txt";

	ifstream File1;
	File1.open(LogDir.c_str());
	if(!File1.good()) {
		File1.close();
		unsigned LoopID = 1;
		ofstream File2;
		File2.open(LogDir.c_str());
		File2 << LoopID << "\n";
		File2.close();
		assert(LoopID > 0);
		return LoopID - 1;
	}
	else {
		File1.close();
		ifstream File3;
		File3.open(LogDir.c_str());
		string str2;
		unsigned LoopID = UINT_MAX;
		while(getline(File3, str2)) {
			istringstream ss(str2);
			string ID;
			ss >> ID;
			LoopID = stoi(ID);
		}

		File3.close();
		assert(LoopID != UINT_MAX);
		LoopID++;
		ofstream File2;
		File2.open(LogDir.c_str());
		File2 << LoopID << "\n";
		File2.close();
		assert(LoopID > 0);
		return LoopID - 1;
	}
	return 0;
}*/

unsigned getLoopID(Function *F, unsigned FuncID, unsigned LoopID, string filename) {
	const char *ModDirEnv = getenv("HOME");
	assert(ModDirEnv);
	string ModDir = ModDirEnv;
	ModDir = ModDir + "/stats/" + filename + "_ModuleInfo.txt";
	ifstream File1;
	File1.open(ModDir.c_str());
	assert(File1.good());

	string str1;
	string name1, startStr1, endStr1;
	unsigned startTmp1 = UINT_MAX;
	while(getline(File1, str1)) {
		istringstream ss(str1);
		ss >> name1;
		ss >> startStr1;
		ss >> endStr1;
		if(F->getParent()->getName().equals(name1)) {
			startTmp1 = stoul(startStr1);
		}
	}
	File1.close();
	assert(startTmp1 != UINT_MAX);
	unsigned FID = startTmp1 + FuncID;

	const char *FuncDirEnv = getenv("HOME");
	assert(FuncDirEnv);
	string FuncDir = FuncDirEnv;
	FuncDir = FuncDir + "/stats/" + filename + "_FunctionInfo.txt";
	ifstream File2;
	File2.open(FuncDir.c_str());
	assert(File2.good());

	string str2;
	string id2, name2, startStr2, endStr2;
	unsigned startTmp2 = UINT_MAX;
	while(getline(File2, str2)) {
		istringstream ss(str2);
		ss >> id2;
		ss >> name2;
		ss >> startStr2;
		ss >> endStr2;
		if(FID == stoul(id2)) {
			startTmp2 = stoul(startStr2);
		}
	}
	File2.close();
	assert(startTmp2 != UINT_MAX);
	unsigned LID = UINT_MAX;
	LID = startTmp2 + LoopID;
	return LID;
}

void collectInnermostLoops(LoopInfo *LI, vector<Loop *> &Loops) {
	for (Loop *L : *LI) {
		if(L->empty()) {
			Loops.push_back(L);
		}
		else {
			std::vector<Loop *> Worklist;
	      	Worklist.push_back(L);
	      	while(!Worklist.empty()) {
	        	Loop *CurLoop = Worklist.back();
	        	Worklist.pop_back();
	        	for (Loop *InnerL : *CurLoop) {
	        		if(InnerL->empty()) {
	        			Loops.push_back(InnerL);
	        		}
	        		else {
	        			Worklist.push_back(InnerL);
	        		} 
	        	}
	      	}
		}
	}
}

void assignFunctionID(Module *M, string filename) {
	unsigned FuncCount = 0;
	for (Module::iterator F = M->begin(); F != M->end(); ++F) {
		Function *Func = dyn_cast<Function>(F);
		if (!Func->isDeclaration()) {
			FuncCount++;
		}
	}

	const char *ModDirEnv = getenv("HOME");
	assert(ModDirEnv);
	string ModDir = ModDirEnv;
	string ModD = ModDir + "/stats";
	ModDir = ModDir + "/stats/" + filename + "_ModuleInfo.txt";
	synchronize(ModD.c_str(), true);

	fstream File1;
	unsigned start = 0, end = 0;
	File1.open(ModDir.c_str(), ios::in | ios::app);
	assert(File1.good());

	if(File1.peek() != EOF) {
		string str;
		string name, startStr, endStr;
		unsigned startTmp = UINT_MAX, endTmp = UINT_MAX;
		while(getline(File1, str)) {
			endTmp = UINT_MAX;
			istringstream ss(str);
			ss >> name;
			ss >> startStr;
			ss >> endStr;
			endTmp = stoul(endStr);
			if(M->getName().equals(name)) {
				assert(endTmp != UINT_MAX);
				startTmp = stoul(startStr);
				assert(endTmp == startTmp + FuncCount);
				File1.close();
				synchronize(ModD.c_str(), false);
				return;
			}
		}
		
		assert(endTmp != UINT_MAX);
		start += endTmp;
	}
	
	File1.clear();
	end = start + FuncCount;
	File1 << M->getName().str() << " " << start << " " << end << "\n";
	File1.close();
	synchronize(ModD.c_str(), false);
}

void assignLoopID(Function *F, unsigned FuncID, unsigned LoopCount, string filename) {
	const char *ModDirEnv = getenv("HOME");
	assert(ModDirEnv);
	string ModDir = ModDirEnv;
	string ModD = ModDir + "/stats";
	ModDir = ModDir + "/stats/" + filename + "_ModuleInfo.txt";
	synchronize(ModD.c_str(), true);
	ifstream File1;
	File1.open(ModDir.c_str());
	assert(File1.good());

	string str;
	string name, startStr, endStr;
	unsigned startTmp = UINT_MAX;
	while(getline(File1, str)) {
		istringstream ss(str);
		ss >> name;
		ss >> startStr;
		ss >> endStr;
		if(F->getParent()->getName().equals(name)) {
			startTmp = stoul(startStr);
		}
	}
	File1.close();
	assert(startTmp != UINT_MAX);
	unsigned FID = startTmp + FuncID;
	
	const char *FuncDirEnv = getenv("HOME");
	assert(FuncDirEnv);
	string FuncDir = FuncDirEnv;
	FuncDir = FuncDir + "/stats/" + filename + "_FunctionInfo.txt";

	fstream File2;
	unsigned start = 0, end = 0;
	File2.open(FuncDir.c_str(), ios::in | ios::app);
	if(File2.peek() != EOF) {
		string str;
		string id, name, startStr, endStr;
		unsigned startTmp = UINT_MAX, endTmp = UINT_MAX;
		while(getline(File2, str)) {
			endTmp = UINT_MAX;
			istringstream ss(str);
			ss >> id;
			ss >> name;
			ss >> startStr;
			ss >> endStr;
			endTmp = stoul(endStr);
			if(stoul(id) == FID) {
				startTmp = stoul(startStr);
				assert(F->getName().equals(name));
				assert(endTmp != UINT_MAX);
				assert(endTmp == startTmp + LoopCount);
				File2.close();
				synchronize(ModD.c_str(), false);
				return;
			}
		}
		
		assert(endTmp != UINT_MAX);
		start += endTmp;
	}

	File2.clear();
	end = start + LoopCount;
	File2 << FID << " " << F->getName().str() << " " << start << " " << end << "\n";
	File2.close();
	synchronize(ModD.c_str(), false);
}

unsigned getFuncID(Function *F, unsigned FuncID, string filename) {
	const char *ModDirEnv = getenv("HOME");
	assert(ModDirEnv);
	string ModDir = ModDirEnv;
	ModDir = ModDir + "/stats/" + filename + "_ModuleInfo.txt";
	ifstream File1;
	File1.open(ModDir.c_str());
	assert(File1.good());

	string str1;
	string name1, startStr1, endStr1;
	unsigned startTmp1 = UINT_MAX;
	while(getline(File1, str1)) {
		istringstream ss(str1);
		ss >> name1;
		ss >> startStr1;
		ss >> endStr1;
		if(F->getParent()->getName().equals(name1)) {
			startTmp1 = stoul(startStr1);
		}
	}
	File1.close();
	assert(startTmp1 != UINT_MAX);
	unsigned FID = startTmp1 + FuncID;
	return FID;
}

void synchronize(const char *path, bool lock) {
	static FILE *LockFile = NULL;
	int ret;
	if (!lock) {
		assert(LockFile);
		ret = flock(fileno(LockFile), LOCK_UN);
		assert(ret != -1);
		fclose(LockFile);
		LockFile = NULL;
	}
	else {
		assert(!LockFile);
		char Name[128];
		int len;
		len = snprintf(Name, 128, "%s/lock", path);
		assert(len < 128);
		LockFile = fopen(Name, "w");
		assert(LockFile);
		ret = flock(fileno(LockFile), LOCK_EX);
		assert(ret != -1);
	}
}

void replaceCondWithTrue(Loop *L, BasicBlock *RuntimeChecksBlock) {
	if(BranchInst *BI = dyn_cast<BranchInst>(RuntimeChecksBlock->getTerminator())) {
		assert(BI->isConditional());
		IntegerType *IntType = Type::getInt1Ty(L->getLoopPreheader()
									->getTerminator()->getContext());
		BI->setCondition(ConstantInt::getTrue(IntType));
	}
	else {
		assert(0 && "Invalid terminator instruction.");
	}
}

void createTimeStatsMap(string filename, DenseMap<unsigned, unsigned long long> &UnoptimizedTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> &OptimizedTimeStatsMap) {
	const char *DirEnv = getenv("HOME");
	assert(DirEnv);
	string OldDir = DirEnv;
	OldDir = OldDir + "/stats/native/" + filename + "_LoopTimeStats.txt";

	string NewDir = DirEnv;
	NewDir = NewDir + "/stats/runtime/" + filename + "_LoopTimeStats.txt";

	fstream File1;
	File1.open(OldDir.c_str(), ios::in);
	assert(File1.good() && File1.peek() != EOF);

	string str1;
	while(getline(File1, str1)) {
		int ID;
		unsigned long long time;
		if(sscanf(str1.c_str(), "%d %llu", &ID, &time) == 2) {
			assert(ID > -1);
			assert(!UnoptimizedTimeStatsMap.count(ID));
			UnoptimizedTimeStatsMap[ID] = time;
		}
	}
	
	File1.clear();

	fstream File2;
	File2.open(NewDir.c_str(), ios::in);
	assert(File2.good() && File2.peek() != EOF);

	string str2;
	while(getline(File2, str2)) {
		int ID;
		unsigned long long time;
		if(sscanf(str2.c_str(), "%d %llu", &ID, &time) == 2) {
			assert(ID > -1);
			assert(!OptimizedTimeStatsMap.count(ID));
			assert(UnoptimizedTimeStatsMap.count(ID));
			OptimizedTimeStatsMap[ID] = time;
		}
	}
	
	File2.clear();
	assert(UnoptimizedTimeStatsMap.size() == OptimizedTimeStatsMap.size());

	for(auto Pair : OptimizedTimeStatsMap) {
		assert(UnoptimizedTimeStatsMap.count(Pair.first));
	}
}

void createFuncTimeStatsMap(string filename, DenseMap<unsigned, unsigned long long> &UnoptimizedFuncTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> &OptimizedFuncTimeStatsMap) {
	const char *DirEnv = getenv("HOME");
	assert(DirEnv);
	string OldDir = DirEnv;
	OldDir = OldDir + "/stats/native/" + filename + "_FuncTimeStats.txt";

	string NewDir = DirEnv;
	NewDir = NewDir + "/stats/runtime/" + filename + "_FuncTimeStats.txt";

	fstream File1;
	File1.open(OldDir.c_str(), ios::in);
	assert(File1.good() && File1.peek() != EOF);

	string str1;
	while(getline(File1, str1)) {
		int ID;
		unsigned long long time;
		if(sscanf(str1.c_str(), "%d %llu", &ID, &time) == 2) {
			assert(ID > -1);
			assert(!UnoptimizedFuncTimeStatsMap.count(ID));
			UnoptimizedFuncTimeStatsMap[ID] = time;
		}
	}
	
	File1.clear();

	fstream File2;
	File2.open(NewDir.c_str(), ios::in);
	assert(File2.good() && File2.peek() != EOF);

	string str2;
	while(getline(File2, str2)) {
		int ID;
		unsigned long long time;
		if(sscanf(str2.c_str(), "%d %llu", &ID, &time) == 2) {
			assert(ID > -1);
			assert(!OptimizedFuncTimeStatsMap.count(ID));
			assert(UnoptimizedFuncTimeStatsMap.count(ID));
			OptimizedFuncTimeStatsMap[ID] = time;
		}
	}
	
	File2.clear();
	assert(UnoptimizedFuncTimeStatsMap.size() == OptimizedFuncTimeStatsMap.size());

	for(auto Pair : OptimizedFuncTimeStatsMap) {
		assert(UnoptimizedFuncTimeStatsMap.count(Pair.first));
	}
}

void createLoopIterStatsMap(string filename, DenseMap<unsigned, unsigned long long> &LoopIterStatsMap) {
	const char *DirEnv = getenv("HOME");
	assert(DirEnv);
	string OldDir = DirEnv;
	OldDir = OldDir + "/stats/" + filename + "_LoopIterStats.txt";

	fstream File1;
	File1.open(OldDir.c_str(), ios::in);
	assert(File1.good() && File1.peek() != EOF);

	string str1;
	while(getline(File1, str1)) {
		unsigned ID = 0;
		unsigned count = 0;
		unsigned long long indicount = 0;
		unsigned long long aver = 0;
		if(sscanf(str1.c_str(), "%u %u %llu %llu", &ID, &count, &indicount, &aver) == 4) {
			assert(count > 0);
			assert(!LoopIterStatsMap.count(ID));
			LoopIterStatsMap[ID] = indicount;
		}
	}
	
	File1.clear();
}

bool isLoopBenefited(unsigned LoopID, DenseMap<unsigned, unsigned long long> UnoptimizedTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> OptimizedTimeStatsMap, unsigned threshold) {
	if(UnoptimizedTimeStatsMap.count(LoopID)) {
		assert(OptimizedTimeStatsMap.count(LoopID));
	}
	else {
		assert(!OptimizedTimeStatsMap.count(LoopID));
		return false;
	}
	assert(UnoptimizedTimeStatsMap[LoopID] > 0 && OptimizedTimeStatsMap[LoopID] > 0);
	long long result = (long long)(((long long)(UnoptimizedTimeStatsMap[LoopID] - 
		OptimizedTimeStatsMap[LoopID]) * 100) / (long long)UnoptimizedTimeStatsMap[LoopID]);
	if(result > (long long)threshold) {
		return true;
	}
	else {
		return false;
	}
}

bool isFuncBenefited(unsigned FID, DenseMap<unsigned, unsigned long long> UnoptimizedFuncTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> OptimizedFuncTimeStatsMap, unsigned threshold) {
	if(UnoptimizedFuncTimeStatsMap.count(FID)) {
		assert(OptimizedFuncTimeStatsMap.count(FID));
	}
	else {
		assert(!OptimizedFuncTimeStatsMap.count(FID));
		return false;
	}
	assert(UnoptimizedFuncTimeStatsMap[FID] > 0 && OptimizedFuncTimeStatsMap[FID] > 0);
	long long result = (long long)(((long long)(UnoptimizedFuncTimeStatsMap[FID] - 
		OptimizedFuncTimeStatsMap[FID]) * 100) / (long long)UnoptimizedFuncTimeStatsMap[FID]);
	if(result > (long long)threshold) {
		return true;
	}
	else {
		return false;
	}
}

void removeDeadLoop(Loop *L) {
    auto DefsUsedOutside = findDefsUsedOutsideOfLoop(L);
    for(auto val : DefsUsedOutside) {
    	auto *Undef = UndefValue::get(val->getType());
    	for (Value::use_iterator UI = val->use_begin(), E = val->use_end();
             UI != E;) {
           	Use &U = *UI;
        	++UI;
        	U.set(Undef);
       }
    }

	/*for (Loop::block_iterator block = L->block_begin(), end = L->block_end(); block != end; block++) {
        BasicBlock * bb = *block;
        for (BasicBlock::iterator II = bb->begin(); II != bb->end(); ++II) {
            Instruction *insII = &(*II);
            if(!isa<BranchInst>(insII) && !isa<SwitchInst>(insII)) {
            	insII->dropAllReferences();
            }
        }
    }*/
}

/*void convertDynamicCheckIntrToCall(Function *F) {
	Module *M = F->getParent();
  	IntegerType *Int1Type = Type::getInt1Ty(M->getContext());
  	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);

  	if(!isFuncDefAdded) {
  		vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
		ArgTypes.push_back(CharStarType);
		//FunctionType *FTy = FunctionType::get(IntType, ArgTypes, false);
		FunctionType *FTy = FunctionType::get(Int1Type, ArgTypes, false);
		isNoAliasRTCheckFunc = Function::Create(FTy,
			GlobalValue::ExternalLinkage, "isNoAliasRTCheck", M);
		isNoAliasRTCheckFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadOnly);
		isFuncDefAdded = true;
  	}

  	DenseMap<Value *, Value *> ReplacementMap;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	CallInst *CI = dyn_cast<CallInst>(BI);
            if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
            	IRBuilder<> IRB1(BI);
            	assert(CI->getOperand(0)->getType() == CI->getOperand(1)->getType());
            	vector<Value *> ArgValue;
	    		ArgValue.push_back(CI->getOperand(0));
	    		ArgValue.push_back(CI->getOperand(1));
	    		Value *RTCall = IRB1.CreateCall(isNoAliasRTCheckFunc, ArgValue);
	    		assert(RTCall);
	    		ReplacementMap[BI] = RTCall; 
            }
      	}
  	}

  	for(auto Pair: ReplacementMap) {
  		Pair.first->replaceAllUsesWith(Pair.second);
  		Instruction *I = dyn_cast<Instruction>(Pair.first);
  		I->eraseFromParent();
  	}
}*/

bool areBasePairsSafe(DenseSet<Value *> SafeBases,
	DenseSet<pair<Value *, Value *>> Bases) {
	assert(Bases.size() > 0);
	for(auto Pair : Bases) {
		if(!SafeBases.count(Pair.first) && !SafeBases.count(Pair.second)) {
			return false;
		}
	}
	return true;
}

void setGlobalAddressVariable(Instruction *I) {
	IRBuilder<> IRB1(I);
  	Type *VoidType = Type::getVoidTy(I->getModule()->getContext());

  	vector<Type *> ArgTypes;
	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
	Function *setGlobal = Function::Create(FTy1,
			GlobalValue::ExternalLinkage, "setGlobalVar", I->getModule());

  	assert(setGlobal);
  	IRB1.CreateCall(setGlobal);
}

bool isIndStepFound(Loop *L, ScalarEvolution *SE) {
	auto IndVar = L->getInductionVariable(*SE);
	if(IndVar == NULL) {
		return false;
	}
	
  	auto Bounds = L->getBounds(*SE);
  	if(Bounds == None) {
    	return false;
  	}

  	auto Dir = Bounds->getDirection();
  	if(Dir == Loop::LoopBounds::Direction::Unknown) {
    	return false;
  	}

  	auto StepValue = Bounds->getStepValue();
  	if(!StepValue || !isa<ConstantInt>(StepValue)) {
    	return false;
  	}

  	return true;
}

void getBasesLVBenefited(Loop *L, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT) {
	DenseSet<pair<Value *, Value *>> TempFinalBases;
	DenseSet<Value *> TempSafeBases;
	for(auto Pair: InstSet) {
		assert(isa<StoreInst>(Pair.first) || isa<StoreInst>(Pair.second));
		PostDominatorTree PDT(*(dyn_cast<Instruction>(Pair.first))->getFunction());
		Instruction *I1 = NULL, *I2 = NULL;
		if(isa<StoreInst>(Pair.first)) {
			I1 = Pair.first;
			I2 = Pair.second;
		}
		else {
			assert(isa<StoreInst>(Pair.second));
			I1 = Pair.second;
			I2 = Pair.first;
		}
		assert(I1 && I2);
		if(!PDT.dominates(I1->getParent(), L->getLoopPreheader())) {
			return;
		}

		MemoryLocation Loc1 = getMemLoc(I1);
		assert(Loc1.Ptr && !isa<Constant>(Loc1.Ptr));

		MemoryLocation Loc2 = getMemLoc(I2);
		assert(Loc2.Ptr && !isa<Constant>(Loc2.Ptr));

		if(!getBasesPair(L, Loc1, Loc2, TempFinalBases, LI, DT, TempSafeBases, 
			true, true)) {
			return;
		}
	}

	FinalBases.insert(TempFinalBases.begin(), TempFinalBases.end());
}

void getBasesStoreBenefited(Loop *L, Instruction *Store, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT) {
	DenseSet<pair<Value *, Value *>> TempFinalBases;
	DenseSet<Value *> TempSafeBases;
	//To avoid adding duplicate bases.
	if(FinalBases.size() > 0) {
		TempFinalBases.insert(FinalBases.begin(), FinalBases.end());
	}

	for(auto Pair: InstSet) {
		if((isa<StoreInst>(Pair.first) || isa<StoreInst>(Pair.second)) 
			&& (Pair.first == Store || Pair.second == Store)) {
			Instruction *I1 = NULL, *I2 = NULL;
			if(Pair.first == Store) {
				I1 = Pair.first;
				I2 = Pair.second;
			}
			else {
				assert(Pair.second == Store);
				I1 = Pair.second;
				I2 = Pair.first;
			}
			assert(I1 && I2);
			MemoryLocation Loc1 = getMemLoc(I1);
			assert(Loc1.Ptr && !isa<Constant>(Loc1.Ptr));

			MemoryLocation Loc2 = getMemLoc(I2);
			assert(Loc2.Ptr && !isa<Constant>(Loc2.Ptr));

			if(!getBasesPair(L, Loc1, Loc2, TempFinalBases, LI, DT, TempSafeBases, 
				true, true)) {
				return;
			}
		}
	}

	FinalBases.insert(TempFinalBases.begin(), TempFinalBases.end());
}

void getBasesLoadBenefited(Loop *L, Instruction *Load, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT) {
	DenseSet<pair<Value *, Value *>> TempFinalBases;
	DenseSet<Value *> TempSafeBases;
	//To avoid adding duplicate bases.
	if(FinalBases.size() > 0) {
		TempFinalBases.insert(FinalBases.begin(), FinalBases.end());
	}

	for(auto Pair: InstSet) {
		if((isa<LoadInst>(Pair.first) || isa<LoadInst>(Pair.second)) 
			&& (Pair.first == Load || Pair.second == Load)) {
			Instruction *I1 = NULL, *I2 = NULL;
			if(Pair.first == Load) {
				I1 = Pair.first;
				I2 = Pair.second;
			}
			else {
				assert(Pair.second == Load);
				I1 = Pair.second;
				I2 = Pair.first;
			}
			assert(I1 && I2);
			
			MemoryLocation Loc1 = getMemLoc(I1);
			assert(Loc1.Ptr && !isa<Constant>(Loc1.Ptr));

			MemoryLocation Loc2 = getMemLoc(I2);
			assert(Loc2.Ptr && !isa<Constant>(Loc2.Ptr));

			if(!getBasesPair(L, Loc1, Loc2, TempFinalBases, LI, DT, TempSafeBases, 
				true, true)) {
				return;
			}
		}
	}

	FinalBases.insert(TempFinalBases.begin(), TempFinalBases.end());
}


void createCloneToGetChecks(Loop *L, LoopInfo *LI, 
		DominatorTree *DT, AAResults *AA, Loop **NL, 
		BasicBlock **RCB, DenseSet<Instruction *> &AllLoads,
		DenseSet<Instruction *> &AllStores, 
		DenseSet<pair<Value *, Value *>> &LoadBases,
		DenseSet<pair<Value *, Value *>> &StoreBases, 
		DenseMap<Instruction *, DenseSet<MDNode *>> &LoadDomainMap,
		DenseMap<Instruction *, DenseSet<MDNode *>> &ConflictingLoadDomainMap,
		DenseMap<Instruction *, DenseSet<MDNode *>> &StoreDomainMap, 
		bool ExecuteOptimizedPath, bool ExecuteUnoptimizedPath, DenseSet<Value *> &SafeBases,
		DenseSet<pair<Instruction *, Instruction *>> &InstSet) {
	assert(isInnermostLoop(L));
	assert(L->getLoopPreheader() && L->getHeader());
	assert(L->getNumBackEdges() == 1);

	if(!L->getExitBlock() || !L->getExitingBlock() 
		|| !L->isSafeToClone()) {
		return;
	}

	//DenseSet<Instruction *> AllLoads;
	//DenseSet<Instruction *> AllStores;
	getAllLoadStore(L, AllLoads, AllStores);

	if(AllLoads.size() == 0 || AllStores.size() == 0) {
		return;
	}

	Function *F = L->getHeader()->getParent();

	DenseSet<Instruction *> ConflictingLoads;
	DenseSet<Instruction *> ConflictingStores;
	//DenseSet<pair<Instruction *, Instruction *>> InstSet;

	bool AllLoadsAreChecked = true;

	for(auto Load : AllLoads) {
		AllLoadsAreChecked &= LoadDep(L, Load, AllLoads, 
			AllStores, ConflictingLoads, InstSet, LoadBases, LI, DT, AA, SafeBases, ConflictingStores);
	}
	if(InstSet.size() == 0) {
		assert(LoadBases.size() == 0);
		return;
	}

	if(!AllLoadsAreChecked) {
		ConflictingLoads.clear();
	}

	if(AllLoadsAreChecked) {
		assert(LoadBases.size() > 0);
		for(auto Store : AllStores) {
			StoreDep(L, Store, AllStores, InstSet, StoreBases, LI, DT, AA, SafeBases);
		}
	}

	BasicBlock *RuntimeChecksBlock = 
	createBlockToAddRuntimeChecks(F, L, DT, LI, false);

	DT->recalculate(*F);
	//moveInstToRuntimeBlock(L, RuntimeChecksBlock);
	ValueToValueMapTy VMap;
	Loop *NewLoop = LoopClone(L, RuntimeChecksBlock, VMap, DT, LI, false);
	Loop *ParentLoop = L->getParentLoop();
	Loop *NewParentLoop = NewLoop->getParentLoop();

	if(ParentLoop && !NewParentLoop) {
	  	ParentLoop->addChildLoop(NewLoop);
	}
	
	addDummyFunctionCall(L, NewLoop, RuntimeChecksBlock, ExecuteOptimizedPath, ExecuteUnoptimizedPath);

	addIDMetadata(L, NewLoop);

	//Creates a map from old to new instructions.
	DenseMap<Instruction *, unsigned> OldInstMap;
	DenseMap<unsigned, Instruction *> NewInstMap;
	mapInstructions(L, NewLoop, OldInstMap, NewInstMap);

	addPHINodesForLoop(L, NewLoop, VMap, LI);

	addMetadata(L, InstSet, ConflictingLoads, AllLoads, AllStores, OldInstMap, 
		NewInstMap, LoadDomainMap, ConflictingLoadDomainMap, StoreDomainMap, AA);

	DenseSet<pair<Value *, Value *>> AddedBases;
	addCustomNoAliasIntrinsic(F, NewLoop, LoadBases, AddedBases);
	addCustomNoAliasIntrinsic(F, NewLoop, StoreBases, AddedBases);

	//errs() << "PrintingFunction After Scoped No_Alias::\n";
	//printNoAliasInfo(*F, AA);

	if (verifyFunction(*F, &errs())) {
        errs() << "Not able to verify!\n";
        errs() << *F << "\n";
        assert(0);
    }
    
    *NL = NewLoop;
    *RCB = RuntimeChecksBlock;
    
	//return NewLoop;
}

void createCloneToVectorize(Loop *L, LoopInfo *LI, 
		DominatorTree *DT, AAResults *AA, Loop **NL, 
		BasicBlock **RCB, DenseSet<Instruction *> LoadSet, 
		DenseSet<Instruction *> StoreSet, bool calledFromAV) {
	return;
	/*assert(isInnermostLoop(L));
	assert(L->getLoopPreheader() && L->getHeader());
	assert(L->getNumBackEdges() == 1);

	if(!L->getExitBlock() || !L->getExitingBlock()) {
		return;
	}

	if(!L->isSafeToClone()) {
		return; 
	}

	Function *F = L->getHeader()->getParent();

	DenseSet<pair<Value *, Value *>> Bases;
	DenseSet<pair<Instruction *, Instruction *>> Insts;
	DenseSet<Instruction *> ConflictingLoads;
	DenseSet<Instruction *> LoadInsts;

	if(!calledFromAV && !areMemDependenciesFound(L, Bases, Insts, 
		ConflictingLoads, LI, DT, AA, LoadSet, StoreSet)) {
	  	return;
	}

	if(calledFromAV && !areMemDependenciesFoundSelectedInst(L, Bases, Insts, 
		ConflictingLoads, LoadInsts, LI, DT, AA, LoadSet, StoreSet)) {
		return;
	}

	assert(Insts.size() > 0 && Bases.size() > 0);

	BasicBlock *RuntimeChecksBlock = 
	createBlockToAddRuntimeChecks(F, L, DT, LI, true);

	Loop *NewLoop;
	DenseSet<pair<Value *, Value *>> AddedBases;
	emitRuntimeChecks(F, 0, 0, L, NewLoop, RuntimeChecksBlock, Bases, true, AddedBases, "", NULL);
	DT->recalculate(*F);
	//moveInstToRuntimeBlock(L, RuntimeChecksBlock);
	ValueToValueMapTy VMap;
	NewLoop = LoopClone(L, RuntimeChecksBlock, VMap, DT, LI, true);
	Loop *ParentLoop = L->getParentLoop();
	Loop *NewParentLoop = NewLoop->getParentLoop();

	if(ParentLoop && !NewParentLoop) {
	  	ParentLoop->addChildLoop(NewLoop);
	}
	addIDMetadata(L, NewLoop);

	//Creates a map from old to new instructions.
	DenseMap<Instruction *, unsigned> OldInstMap;
	DenseMap<unsigned, Instruction *> NewInstMap;
	mapInstructions(L, NewLoop, OldInstMap, NewInstMap);

	addPHINodesForLoop(L, NewLoop, VMap, LI);

	addScopedNoaliasMetadata(L, NewLoop, Insts, 
		ConflictingLoads, LoadInsts, 
		OldInstMap, NewInstMap);

	//errs() << "PrintingFunction After Scoped No_Alias::\n";
	//printNoAliasInfo(*F, AA);

	if (verifyFunction(*F, &errs())) {
        errs() << "Not able to verify!\n";
        errs() << *F << "\n";
        assert(0);
    }
    
    *NL = NewLoop;
    *RCB = RuntimeChecksBlock;*/
    
	//return NewLoop;
}

bool isAlignedAlloc(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("aligned_alloc")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("aligned_alloc")) {
				return true;
			}
		}
	}
	return false;
}

bool isMalloc(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("malloc")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("malloc")) {
				return true;
			}
		}
	}
	return false;
}

bool isCalloc(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("calloc")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("calloc")) {
				return true;
			}
		}
	}
	return false;
}

bool isRealloc1(Instruction *I, Value* &Oper) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("realloc")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("realloc")) {
				return true;
			}
		}
	}
	return false;
}

bool isRealloc(Instruction *I) {

	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("realloc")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("realloc")) {
				return true;
			}
		}
	}
	return false;
}

bool isNewOperator(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("_Znam")) {
				return true;
			}
			if(F->getName().equals("_Znwm")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("_Znam")) {
				return true;
			}
			if(F->getName().equals("_Znwm")) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("_Znam")) {
				return true;
			}
			if(F->getName().equals("_Znwm")) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("_Znam")) {
				return true;
			}
			if(F->getName().equals("_Znwm")) {
				return true;
			}
		}
	}
	return false;
}

bool isFree(Instruction *I, Value* &Oper) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		if (CI->isIndirectCall()) {
			if (CI->arg_size() == 1 && CI->getArgOperand(0)->getType()->isPointerTy() && CI->getType()->isVoidTy()) {
				Oper = CI->getCalledOperand()->stripPointerCasts();
				return true;
			}
			return false;
		}
	}

	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("free")) {
				return true;
			}
			if(F->getName().equals("delete")) {
				return true;
			}
			if(F->getName().equals("_ZdaPv")) {
				return true;
			}
			if(F->getName().equals("_ZdlPv")) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("free")) {
				return true;
			}
			if(F->getName().equals("delete")) {
				return true;
			}
			if(F->getName().equals("_ZdaPv")) {
				return true;
			}
			if(F->getName().equals("_ZdlPv")) {
				return true;
			}
		}
	}
	return false;
}

bool isPosixMemalign(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->getName().equals("posix_memalign")) {
				return true;
			}
		}
	}

	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->getName().equals("posix_memalign")) {
				return true;
			}
		}
	}
	return false;
}

bool isAllocationSite(Instruction *I) {
	if(isAlignedAlloc(I)) {
		return true;
	}
	
	if(isMalloc(I)) {
		return true;
	}

	if(isCalloc(I)) {
		return true;
	}

	if(isRealloc(I)) {
		return true;
	}

	if(isNewOperator(I)) {
		return true;
	}

	if(isPosixMemalign(I)) {
		return true;
	}

	return false;
}

static bool isWrapperFuncAnnotation(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		if(CI->hasFnAttr(Attribute::MemoryAllocWrapperFunc)) {
	    	return true;
	    }
	}

	if(InvokeInst *II = dyn_cast<InvokeInst>(I)) {
    	if(II->hasFnAttr(Attribute::MemoryAllocWrapperFunc)) {
      		return true;
      	}
    }
	return false;
}

static bool isIndirectCallInst(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		if(CI->isIndirectCall() && I->getType()->isPointerTy()) {
			return true;
		}
	}
	return false;
}

static void instrumentSetASIDFunctionWithWrapper(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, TargetLibraryInfo *TLI) {
	if(!issetASIDDefined) {
  		vector<Type *> ArgTypes;
	  	ArgTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(F->getContext())));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	setASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "setASID", (F->getParent()));
	  	issetASIDDefined = true;
  	}

    auto Int8PtrTy = Type::getInt8PtrTy(F->getContext());
    auto Int64Ty = Type::getInt64Ty(F->getContext());
    auto VoidTy = Type::getVoidTy(F->getContext());
    auto setASIDNoAlignedFunc = F->getParent()->getOrInsertFunction("setASIDNoAligned", VoidTy, Int8PtrTy, Int64Ty);

  	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		if(isWrapperFuncAnnotation(AI) || isIndirectCallInst(AI)) {
  			if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
  				continue;
  			}
  			Type *Int64Ty = Type::getInt64Ty(F->getContext());
          	if(!isUniqueIDGlobalCreated) {
          		UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
				ConstantInt::get(Int64Ty, 0), "");
				isUniqueIDGlobalCreated = true;
          	}
          	IRBuilder<> IRB(AI);
          	IRB.CreateStore(ConstantInt::get(Int64Ty, ASID), UniqueASIDGlobal);
          	if(InvokeInst *II = dyn_cast<InvokeInst>(AI)) {
          		auto NB = II->getNormalDest();
	  			assert(&*(NB->getFirstInsertionPt()));
	  			Instruction *NI = &*(NB->getFirstInsertionPt());
	  			assert(NI);
	  			DominatorTree DT(*F);
	  			if(DT.dominates(II, NI)) {
	  				IRB.SetInsertPoint(NI);
	          		//Set the global variable to 0 just to ensure that wrong ASIDs are not used. 
	          		IRB.CreateStore(ConstantInt::get(Int64Ty, 0), UniqueASIDGlobal);
	  			}
          	}
          	else {
          		assert(isa<CallInst>(AI));
          		assert(AI->getNextNode());
	          	IRB.SetInsertPoint(AI->getNextNode());
	          	//Set the global variable to 0 just to ensure that wrong ASIDs are not used. 
	          	IRB.CreateStore(ConstantInt::get(Int64Ty, 0), UniqueASIDGlobal);
          	}
        }
  		else if(InvokeInst *II = dyn_cast<InvokeInst>(AI)) {
  			auto NB = II->getNormalDest();
  			auto UB = II->getUnwindDest();
  			assert(&*(NB->getFirstInsertionPt()));
  			assert(&*(UB->getFirstInsertionPt()));
  			Instruction *NI = &*(NB->getFirstInsertionPt());
  			//Instruction *UI = &*(UB->getFirstInsertionPt());
  			DominatorTree DT(*F);
  			if(DT.dominates(II, NI)) {
  				vector<Value*> Args;
		  		auto BIInst1 = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", NI);
		  		Args.push_back(BIInst1);
		  		if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
		  			assert(BIInst1->getNextNode());
		  			Type *Int64Ty = Type::getInt64Ty(F->getContext());
		          	if(!isUniqueIDGlobalCreated) {
		          		UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
						ConstantInt::get(Int64Ty, 0), "");
						isUniqueIDGlobalCreated = true;
		          	}
		  			IRBuilder<> IRB(BIInst1->getNextNode());
		  			LoadInst *Load = IRB.CreateLoad(UniqueASIDGlobal);
					Args.push_back(Load);
					assert(Load->getNextNode());
					CallInst::Create(setASIDNoAlignedFunc, Args, "", Load->getNextNode());
				}
				else {
					Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
		  			assert(BIInst1->getNextNode());
					CallInst::Create(setASIDNoAlignedFunc, Args, "", BIInst1->getNextNode());
				}
  			}
  		}
  		else if(isPosixMemalign(AI)) {
  			assert(AI->getNextNode());
  			assert(AI->getOperand(0));
  			IRBuilder<> IRB(AI->getNextNode());
  			vector<Value*> Args;
  			LoadInst *Load = IRB.CreateLoad(AI->getOperand(0));
  			assert(Load->getType() == Type::getInt8PtrTy(F->getContext()));
  			Args.push_back(Load);
  			if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
		  		Type *Int64Ty = Type::getInt64Ty(F->getContext());
		        if(!isUniqueIDGlobalCreated) {
		          	UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
								ConstantInt::get(Int64Ty, 0), "");
					isUniqueIDGlobalCreated = true;
		        }
		        assert(Load->getNextNode());
		        IRBuilder<> IRB(Load->getNextNode());
		  		LoadInst *LoadG = IRB.CreateLoad(UniqueASIDGlobal);
				Args.push_back(LoadG);
				assert(LoadG->getNextNode());
				CallInst::Create(setASIDFunc, Args, "", LoadG->getNextNode());
			}
			else {
				Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
  				CallInst::Create(setASIDFunc, Args, "", Load->getNextNode());
			}
  		}
      else if(isAlignedAlloc(AI)) {
        assert(AI->getNextNode());
        vector<Value*> Args;
        auto BIInst = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", AI->getNextNode());
        Args.push_back(BIInst);
        if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
          assert(BIInst->getNextNode());
          Type *Int64Ty = Type::getInt64Ty(F->getContext());
              if(!isUniqueIDGlobalCreated) {
                UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage,
          ConstantInt::get(Int64Ty, 0), "");
          isUniqueIDGlobalCreated = true;
              }
          IRBuilder<> IRB(BIInst->getNextNode());
          LoadInst *Load = IRB.CreateLoad(UniqueASIDGlobal);
        Args.push_back(Load);
        assert(Load->getNextNode());
        CallInst::Create(setASIDFunc, Args, "", Load->getNextNode());
      }
      else {
        Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
        assert(BIInst->getNextNode());
        CallInst::Create(setASIDFunc, Args, "", BIInst->getNextNode());
      }
      }
  		else {
  			assert(AI->getNextNode());
	  		vector<Value*> Args;
	  		auto BIInst = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", AI->getNextNode());
	  		Args.push_back(BIInst);
	  		if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
	  			assert(BIInst->getNextNode());
	  			Type *Int64Ty = Type::getInt64Ty(F->getContext());
	          	if(!isUniqueIDGlobalCreated) {
	          		UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
					ConstantInt::get(Int64Ty, 0), "");
					isUniqueIDGlobalCreated = true;
	          	}
	  			IRBuilder<> IRB(BIInst->getNextNode());
	  			LoadInst *Load = IRB.CreateLoad(UniqueASIDGlobal);
				Args.push_back(Load);
				assert(Load->getNextNode());
				CallInst::Create(setASIDNoAlignedFunc, Args, "", Load->getNextNode());
			}
			else {
				Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
				assert(BIInst->getNextNode());
				CallInst::Create(setASIDNoAlignedFunc, Args, "", BIInst->getNextNode());
			}
  		}
  	}
}

static void instrumentSetASIDFunctionWithoutWrapper(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, TargetLibraryInfo *TLI) {
	if(!issetASIDDefined) {
  		vector<Type *> ArgTypes;
	  	ArgTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(F->getContext())));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	setASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "setASID", (F->getParent()));
	  	issetASIDDefined = true;
  	}

  	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		if(isWrapperFuncAnnotation(AI)) {
  			continue;
  		}
  		else if(InvokeInst *II = dyn_cast<InvokeInst>(AI)) {
  			auto NB = II->getNormalDest();
  			auto UB = II->getUnwindDest();
  			assert(&*(NB->getFirstInsertionPt()));
  			assert(&*(UB->getFirstInsertionPt()));
  			Instruction *NI = &*(NB->getFirstInsertionPt());
  			//Instruction *UI = &*(UB->getFirstInsertionPt());
  			DominatorTree DT(*F);
  			if(DT.dominates(II, NI)) {
  				vector<Value*> Args;
		  		auto BIInst1 = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", NI);
		  		Args.push_back(BIInst1);
		  		Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
		  		assert(BIInst1->getNextNode());
				CallInst::Create(setASIDFunc, Args, "", BIInst1->getNextNode());
  			}
  		}
  		else if(isPosixMemalign(AI)) {
  			assert(AI->getNextNode());
  			assert(AI->getOperand(0));
  			IRBuilder<> IRB(AI->getNextNode());
  			vector<Value*> Args;
  			LoadInst *Load = IRB.CreateLoad(AI->getOperand(0));
  			assert(Load->getType() == Type::getInt8PtrTy(F->getContext()));
  			Args.push_back(Load);
  			Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
  			CallInst::Create(setASIDFunc, Args, "", Load->getNextNode());
  		}
  		else {
  			assert(AI->getNextNode());
	  		vector<Value*> Args;
	  		auto BIInst = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", AI->getNextNode());
	  		Args.push_back(BIInst);
	  		Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), ASID));
			assert(BIInst->getNextNode());
			CallInst::Create(setASIDFunc, Args, "", BIInst->getNextNode());
  		}
  	}
}

void instrumentSetASIDFunction(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, TargetLibraryInfo *TLI, bool EnableWrapperAnnotation) {
	if(EnableWrapperAnnotation) {
		instrumentSetASIDFunctionWithWrapper(F, AllocationIDMap, TLI);
	}
	else {
		instrumentSetASIDFunctionWithoutWrapper(F, AllocationIDMap, TLI);
	}
}	

void instrumentRegionIDFunction(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[]) {
	if(!issetASIDDefined) {
  		vector<Type *> ArgTypes;
	  	ArgTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(F->getContext())));
	  	ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
	  	FunctionType *DTy = FunctionType::get(Type::getVoidTy(F->getContext()), ArgTypes, false);
	  	setASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "setASID", (F->getParent()));
	  	issetASIDDefined = true;
  	}

  	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		if(AllocaRegionMap[ASID] > 0) {
  			assert(AI->getNextNode());
	  		IRBuilder<> IRB(AI->getNextNode());

	  		vector<Value*> Args;
	  		auto BIInst = new BitCastInst(AI, PointerType::getUnqual(Type::getInt8Ty(F->getContext())), "", AI->getNextNode());
	  		Args.push_back(BIInst);
	  		Args.push_back(ConstantInt::get(Type::getInt64Ty(F->getContext()), AllocaRegionMap[ASID]));
	  		assert(BIInst->getNextNode());
			CallInst::Create(setASIDFunc, Args, "", BIInst->getNextNode());
  		}
  	}
}

static Value* getGlobalVar(Module *M, const char *varname, Type *Ty)
{
  auto StackPtr =
      dyn_cast_or_null<GlobalVariable>(M->getNamedValue(varname));

  if (!StackPtr) { 
    StackPtr = new GlobalVariable(
        *M, Ty, false, GlobalValue::ExternalLinkage, nullptr,
        varname, nullptr, GlobalValue::InitialExecTLSModel);
  }
  return StackPtr;
}

void addDummyStore(Function &F)
{
  Module *M = F.getParent();
  Type *Ty = Type::getInt64Ty(M->getContext());
	Type *Int64Ty = Type::getInt64Ty(M->getContext());
  Value *GVar = getGlobalVar(M, "__mi_dummy", Ty);

  for (auto &BB : F) {
    for (auto &II : BB) {
      Instruction *AI = &II;
      if (isMalloc(AI) || isNewOperator(AI) || isCalloc(AI) || isRealloc(AI) || isAlignedAlloc(AI) || isPosixMemalign(AI)) {
        IRBuilder<> IRB(AI);
        IRB.CreateStore(ConstantInt::get(Int64Ty, 0), GVar);
      }
    }
  }
}

static void setRegionID(Function *F, Instruction *I, unsigned long long ID, Value *GVar)
{
	IRBuilder<> IRB(I);
  Type *Int64Ty = Type::getInt64Ty(F->getContext());
	IRB.CreateStore(ConstantInt::get(Int64Ty, ID), GVar);
}

static Instruction *getInsertPt(AllocaInst *AI) {
	DenseSet<Instruction *> AllUses;
	BasicBlock *BB = AI->getParent();

	for (Use &UI : AI->uses()) {
		auto I = cast<Instruction>(UI.getUser());
		if (BB == I->getParent()) {
			AllUses.insert(I);
		}
	}

	if (AllUses.empty()) {
		return BB->getTerminator();
	}

	if(AllUses.size() == 1) {
		for(auto I: AllUses) {
			return I;
		}
	}

	for (auto &I : *BB) {
		if (AllUses.count(&I)) {
			return &I;
		}
	}
	assert(false);
	return NULL;
}

static void replaceAllocas(Function *F, Module *M, DenseSet<AllocaInst*> AllocaSet[]) {
  	auto Int32Ty = Type::getInt32Ty(M->getContext());
	auto Int64Ty = Type::getInt64Ty(M->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());
	int i;
	Instruction *InsertPt[16] = {NULL};

#if 0
	for (i = 0; i < 16; i++) {
		if (!AllocaSet[i].empty()) {
			auto AI = *(AllocaSet[i].begin());
			InsertPt[i] = getInsertPt(AI);
			assert(InsertPt[i]);
			IRBuilder<> IRB(InsertPt[i]);

			auto StackPtr = getStackPtr(M, "stackptr_s");
			auto Region = ConstantInt::get(Int32Ty, i);
			auto Regi = IRB.CreateGEP(StackPtr, {Region});
			auto Start = IRB.CreateLoad(Regi);
	        	auto Cmp = IRB.CreateICmp(ICmpInst::ICMP_EQ, Start, Constant::getNullValue(Start->getType()));   
	       		Instruction *IfTerm;
	       		IfTerm = SplitBlockAndInsertIfThen(Cmp, InsertPt[i], false);
	       		IRB.SetInsertPoint(IfTerm);
			auto Fn = M->getOrInsertFunction("mi_stack_init", RetTy, Int32Ty);
	       		IRB.CreateCall(Fn, {Region});
		}
	}
#endif

	for (i = 0; i < 16; i++) {
		if (!AllocaSet[i].empty()) {
			auto AI = *(AllocaSet[i].begin());
			InsertPt[i] = getInsertPt(AI);
			assert(InsertPt[i]);

			IRBuilder<> IRB(InsertPt[i]);
      Type *StackPtrTy = Type::getInt8PtrTy(M->getContext());
			auto StackPtr = getGlobalVar(M, "stackptr_s", StackPtrTy);
			auto Region = ConstantInt::get(Int32Ty, i);
			auto Regi = IRB.CreateGEP(StackPtr, {Region});
			auto Start = IRB.CreateLoad(Regi);
			Value *Cur = Start;

			for (auto AI : AllocaSet[i]) {
  				size_t alignment = AI->getAlignment();
				size_t Sz = getAllocaSizeInBytes(*AI);
				Value *Size = ConstantInt::get(Int32Ty, Sz);

				if (alignment > 8) {
					assert((alignment % 8) == 0);
					auto StartInt = IRB.CreatePtrToInt(Cur, Int64Ty);
					auto NewVal = IRB.CreateAdd(StartInt, ConstantInt::get(Int64Ty, alignment-1));
					NewVal = IRB.CreateAnd(NewVal, ConstantInt::get(Int64Ty, ~(alignment-1)));
					Cur = IRB.CreateIntToPtr(NewVal, Cur->getType());
				}

	       			auto End = IRB.CreateGEP(Cur, {Size});
				auto Ptr = IRB.CreateBitCast(Cur, AI->getType());
				if(Instruction *PtrI = dyn_cast<Instruction>(Ptr)) {
					SmallVector<pair<unsigned, MDNode *>, 4> MDs;
					AI->getAllMetadata(MDs);
					for(const auto &MI : MDs) {
						PtrI->setMetadata(MI.first, MI.second);
					}
				}
				AI->replaceAllUsesWith(Ptr);
				AI->eraseFromParent();
				Cur = End;
			}
	       		IRB.CreateStore(Cur, Regi);

			for (auto &BB : *F) {
				auto Term = BB.getTerminator();
				if (isa<ReturnInst>(Term)) {
					IRB.SetInsertPoint(Term);
					IRB.CreateStore(Start, Regi);
				}
			}
		}
	}
}

static void replaceMemoryAllocationsWithoutWrapper(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA) {
	Module *M = F->getParent();
  	auto Int32Ty = Type::getInt32Ty(M->getContext());
	auto Int8PtrTy = Type::getInt8PtrTy(M->getContext());
	auto Int64Ty = Type::getInt64Ty(M->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());

	//DenseMap<CallInst*, unsigned> ReallocMap;
	DenseSet<AllocaInst*> AllocaSet[16];

	if (F->getName() == "main") {
		unsigned n_region = getStackRegionReq();
		if (n_region > 1) {
  			Instruction *Entry = dyn_cast<Instruction>(F->begin()->getFirstInsertionPt());
			auto Fn = M->getOrInsertFunction("mi_init_stack", RetTy, Int32Ty);
			CallInst::Create(Fn, {ConstantInt::get(Int32Ty, n_region)}, "", Entry);
		}
	}

	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		//if(AllocaRegionMap[ASID] > 0) {
  			if(isMalloc(AI) || isNewOperator(AI)) {
  				if(CallInst *CI = dyn_cast<CallInst>(AI)) {
  					if(AllocaRegionMap[ASID] > 1) {
  						SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  					AI->getAllMetadata(MDs);
	  					auto AS = CI->getAttributes();
  						IRBuilder<> IRB(AI);
						auto Fn = M->getOrInsertFunction("mi_region_malloc", Int8PtrTy, CI->getOperand(0)->getType(), Int64Ty);
						auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
						for(const auto &MI : MDs) {
    						Call->setMetadata(MI.first, MI.second);
  						}
  						Call->setAttributes(AS);
  						if(CI->isTailCall()) {
  							Call->setTailCall();
  						}
						AI->replaceAllUsesWith(Call);
						AI->eraseFromParent();
						ReplacedMA++;
  					}
  				}
  				else if(InvokeInst *II = dyn_cast<InvokeInst>(AI)) {
  					if(AllocaRegionMap[ASID] > 1) {
  						SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  					AI->getAllMetadata(MDs);
	  					auto AS = II->getAttributes();
  						IRBuilder<> IRB(AI);
						auto Fn = M->getOrInsertFunction("mi_region_malloc", Int8PtrTy, II->getOperand(0)->getType(), Int64Ty);
						auto Call = IRB.CreateInvoke(Fn, II->getNormalDest(), II->getUnwindDest(), 
								{II->getOperand(0), ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
						for(const auto &MI : MDs) {
    						Call->setMetadata(MI.first, MI.second);
  						}
  						Call->setAttributes(AS);
						AI->replaceAllUsesWith(Call);
						AI->eraseFromParent();
	  					ReplacedMA++;
  					}
  				}
  				else {
  					assert(0);
  				}
  			}
  			else if(isCalloc(AI)) {
  				if(AllocaRegionMap[ASID] > 1) {
  					CallInst *CI = dyn_cast<CallInst>(AI);
  					assert(CI);
  					SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  				AI->getAllMetadata(MDs);
	  				auto AS = CI->getAttributes();
  					IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_calloc", Int8PtrTy, CI->getOperand(0)->getType(), 
							CI->getOperand(1)->getType(), Int64Ty);
					auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), 
							ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
  					if(CI->isTailCall()) {
  						Call->setTailCall();
  					}
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();
	  				ReplacedMA++;
  				}
  			}
  			else if(isRealloc(AI)) {
  				if(AllocaRegionMap[ASID] > 1) {
  					CallInst *CI = dyn_cast<CallInst>(AI);
	  				assert(CI);
	  				SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  				AI->getAllMetadata(MDs);
	  				auto AS = CI->getAttributes();
	  				IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_realloc", CI->getType(), CI->getOperand(0)->getType(), CI->getOperand(1)->getType(), Int64Ty);
					auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
  					if(CI->isTailCall()) {
  						Call->setTailCall();
  					}
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();
	  				ReplacedMA++;	
  				}
			}
			else if(isAlignedAlloc(AI)) {
  				if(AllocaRegionMap[ASID] > 1) {
  					CallInst *CI = dyn_cast<CallInst>(AI);
	  				assert(CI);
	  				SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  				AI->getAllMetadata(MDs);
	  				auto AS = CI->getAttributes();
	  				IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_aligned_alloc", CI->getType(), CI->getOperand(0)->getType(), CI->getOperand(1)->getType(), Int64Ty);
					auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
  					if(CI->isTailCall()) {
  						Call->setTailCall();
  					}
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();
	  				ReplacedMA++;	
  				}
			}
			else if(isPosixMemalign(AI)) {
				if(AllocaRegionMap[ASID] > 1) {
					CallInst *CI = dyn_cast<CallInst>(AI);
					assert(CI);
					SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	  				AI->getAllMetadata(MDs);
	  				auto AS = CI->getAttributes();
					IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_posix_memalign", AI->getType(), CI->getOperand(0)->getType(), 
							CI->getOperand(1)->getType(), CI->getOperand(2)->getType(), Int64Ty);
					auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), CI->getOperand(2), 
							ConstantInt::get(Int64Ty, AllocaRegionMap[ASID])});
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
  					if(CI->isTailCall()) {
  						Call->setTailCall();
  					}
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();	
	  				ReplacedMA++;
				}
			}
			else if(AllocaInst *ALI = dyn_cast<AllocaInst>(AI)) {
				/*if(F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
					size_t alignment = ALI->getAlignment();
					size_t Sz = getAllocaSizeInBytes(*ALI);
					Value *Size = ConstantInt::get(Int32Ty, Sz);
					IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_aligned_alloc", AI->getType(), Int32Ty, Size->getType(), Int64Ty);
					if(!isUniqueIDGlobalCreated) {
				    	UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
										ConstantInt::get(Int64Ty, 0), "");
				    	isUniqueIDGlobalCreated = true;
				    }
				    LoadInst *Load = IRB.CreateLoad(UniqueASIDGlobal);
					auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Int32Ty, alignment), Size, Load});
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();

					Fn = M->getOrInsertFunction("mi_free", RetTy, Call->getType());

					for (auto &BB : *F) {
						auto Term = BB.getTerminator();
						if (isa<ReturnInst>(Term)) {
							IRB.SetInsertPoint(Term);
							IRB.CreateCall(Fn, {Call});
						}
					}
					ReplacedMA++;
  				}
  				else*/ if(AllocaRegionMap[ASID] > 1) {
				auto ID = AllocaRegionMap[ASID];
				assert(ALI->isStaticAlloca() && !isSafeAlloca(ALI, *TLI));
				assert(ID < 16);
				AllocaSet[ID].insert(ALI);
#if 0
  				size_t alignment = ALI->getAlignment();
					size_t Sz = getAllocaSizeInBytes(*ALI) + (alignment-1);
					Value *Size = ConstantInt::get(Int32Ty, Sz);
					Value *Size1 = ConstantInt::get(Int32Ty, 0x100000ULL);
					auto *InPt = getInsertPt(ALI);
					IRBuilder<> IRB(const_cast<Instruction*>(InPt));

//#if 0
					auto StackPtr = getStackPtr(M, "stackptr_s");
					auto Region = ConstantInt::get(Int32Ty, AllocaRegionMap[ASID]);

				  auto Regi = IRB.CreateGEP(StackPtr, {Region});
					auto Start = IRB.CreateLoad(Regi);

	        auto Cmp = cast<Instruction>(IRB.CreateICmp(ICmpInst::ICMP_EQ, Start, Constant::getNullValue(Start->getType())));

	       	auto InsertPt = Cmp->getNextNode();
	       	Instruction *IfTerm;
	       	IfTerm = SplitBlockAndInsertIfThen(Cmp, InsertPt, false);
	       	IRB.SetInsertPoint(IfTerm);
		auto Fn = M->getOrInsertFunction("mi_stack_init", Int8PtrTy, Int32Ty, Regi->getType());
	       	auto NewStart = IRB.CreateCall(Fn, {Region, Regi});

		//auto Fn = M->getOrInsertFunction("malloc", Start->getType(), Size1->getType());
		//auto NewStart = IRB.CreateCall(Fn, {Size1});


					
	       	IRB.SetInsertPoint(InsertPt);
	  			PHINode *Call = IRB.CreatePHI(Int8PtrTy, 2);
	  			Call->addIncoming(Start, Start->getParent());
	  			Call->addIncoming(NewStart, IfTerm->getParent());

		Value *Cur = Call;
		if (alignment > 8) {
			assert((alignment % 8) == 0);
			auto StartInt = IRB.CreatePtrToInt(Call, Int64Ty);
			auto NewVal = IRB.CreateAdd(StartInt, ConstantInt::get(Int64Ty, alignment-1));
			NewVal = IRB.CreateAnd(NewVal, ConstantInt::get(Int64Ty, ~(alignment-1)));
			Cur = IRB.CreateIntToPtr(NewVal, Call->getType());
		}

	       	auto End = IRB.CreateGEP(Cur, {Size});
	       	IRB.CreateStore(End, Regi);

				
				  auto Ptr = IRB.CreateBitCast(Cur, ALI->getType());
				  ALI->replaceAllUsesWith(Ptr);
				  ALI->eraseFromParent();

		//auto Fn1 = M->getOrInsertFunction("mi_stack_verify", RetTy, Int8PtrTy, Int8PtrTy, Int8PtrTy, Int32Ty, Int32Ty);
	       	//IRB.CreateCall(Fn1, {Call, Cur, End, Region,  ConstantInt::get(Int32Ty, alignment)});

					//Fn = M->getOrInsertFunction("mi_free", RetTy, Call->getType());
		if (!SeenAllocaInRegion[AllocaRegionMap[ASID]]) {
			SeenAllocaInRegion[AllocaRegionMap[ASID]] = true;
				  for (auto &BB : *F) {
				    auto Term = BB.getTerminator();
				    if (isa<ReturnInst>(Term)) {
				      IRB.SetInsertPoint(Term);
				      IRB.CreateStore(Call, Regi);
				      //IRB.CreateCall(Fn, {Call});
				    }
				  }
		}
#endif
#if 0
					auto Fn = M->getOrInsertFunction("mi_region_aligned_alloc", AI->getType(), Int32Ty, Size->getType(), Int64Ty);
					auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Int32Ty, alignment), Size, ConstantInt::get(Int64Ty, 0 /*AllocaRegionMap[ASID]*/)});
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();

					Fn = M->getOrInsertFunction("mi_free", RetTy, Call->getType());

					for (auto &BB : *F) {
						auto Term = BB.getTerminator();
						if (isa<ReturnInst>(Term)) {
							IRB.SetInsertPoint(Term);
							IRB.CreateCall(Fn, {Call});
						}
					}
#endif
					ReplacedMA++;	
  				}
			}
  		//}
  	}
	replaceAllocas(F, M, AllocaSet);
}

static bool handleAllocationWithSameID(Function *F, unsigned ASID, Instruction *AI, unsigned threshold, 
	DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap, Value *GVar) {
	if(!StartIDMap.count(ASID)) {
		return false;
	}
	assert(EndIDMap.count(ASID));
	Module *M = F->getParent();
	auto Int64Ty = Type::getInt64Ty(F->getContext());
	unsigned startID = StartIDMap[ASID];//MAXREGION + 1 + ((AllocaRegionMap[ASID] - 1) * REGIONLIMIT);
	unsigned endID = EndIDMap[ASID];//MAXREGION + (AllocaRegionMap[ASID] * REGIONLIMIT);
	assert(startID < threshold && endID < threshold);
	GlobalVariable *RegionNumber = new GlobalVariable(*M, Int64Ty, false, GlobalValue::InternalLinkage,
				ConstantInt::get(Int64Ty, startID), Twine(ASID).str());
	assert(RegionNumber);
	IRBuilder<> IRB(AI);
	LoadInst *Load = IRB.CreateLoad(RegionNumber);
	Value *SelLoadValue = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_EQ, Load, 
				IRB.getInt64(0)), IRB.getInt64(startID), Load);
	IRB.CreateStore(SelLoadValue, GVar);
	Value *Inc = IRB.CreateAdd(IRB.getInt64(1), SelLoadValue);
	Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc,
    IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
	IRB.CreateStore(Sel, RegionNumber);
	return true;
}

static void replaceMemoryAllocationsWithWrapper(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, unsigned threshold, 
	DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap) {
	Module *M = F->getParent();
	assert(M);
  auto Int32Ty = Type::getInt32Ty(F->getContext());
	auto Int8PtrTy = Type::getInt8PtrTy(F->getContext());
	auto Int64Ty = Type::getInt64Ty(F->getContext());
	auto RetTy = Type::getVoidTy(F->getContext());

	//DenseMap<CallInst*, unsigned> ReallocMap;
	DenseSet<AllocaInst*> AllocaSet[16];

	if (F->getName() == "main") {
		unsigned n_region = getStackRegionReq();
		if (n_region > 1) {
  			Instruction *Entry = dyn_cast<Instruction>(F->begin()->getFirstInsertionPt());
			auto Fn = M->getOrInsertFunction("mi_init_stack", RetTy, Int32Ty);
			CallInst::Create(Fn, {ConstantInt::get(Int32Ty, n_region)}, "", Entry);
		}
	}

  Value *GVar = NULL;

  if (!AllocationIDMap.empty()) {
    Type *Ty = Type::getInt64Ty(M->getContext());
    GVar = getGlobalVar(M, "__mi_region", Ty);
  }

	for (auto Pair: AllocationIDMap) {
    Instruction *AI = Pair.first;
    unsigned ASID = Pair.second;

    if(StartIDMap.size() > 0 && handleAllocationWithSameID(F, ASID, AI, threshold, StartIDMap, EndIDMap, GVar)) {
    	errs() << "handleAllocationWithSameID returns true\n";
    	continue;
    }

    unsigned long long ID = AllocaRegionMap[ASID];

    if (ID > 1) {
	    ReplacedMA++;
      if (isWrapperFuncAnnotation(AI)) {
    	  assert(!F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc));
        setRegionID(F, AI, ID, GVar);
	    }
	    else if(AllocaInst *ALI = dyn_cast<AllocaInst>(AI)) {
	  	  assert(ALI->isStaticAlloca() && !isSafeAlloca(ALI, *TLI));
	  	  assert(ID < 16);
	  	  AllocaSet[ID].insert(ALI);
	    }
	    else if(isIndirectCallInst(AI)) {
	    	setRegionID(F, AI, ID, GVar);
	    }
    	else if (!F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
        if (isMalloc(AI) || isNewOperator(AI) || isCalloc(AI) || isRealloc(AI) || isAlignedAlloc(AI) || isPosixMemalign(AI)) {
          setRegionID(F, AI, ID, GVar);
        }
    	}
    }
  }
	replaceAllocas(F, M, AllocaSet);
}

void replaceMemoryAllocations(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, bool EnableWrapperAnnotation, 
	unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap) {
	if(EnableWrapperAnnotation) {
		replaceMemoryAllocationsWithWrapper(F, AllocationIDMap, AllocaRegionMap, TLI, ReplacedMA, threshold, StartIDMap, EndIDMap);
	}
	else {
		replaceMemoryAllocationsWithoutWrapper(F, AllocationIDMap, AllocaRegionMap, TLI, ReplacedMA);
	}
}

void assignASID(Function *F, unsigned FID, string filename) {
	unsigned ASIDCount = 0;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	if(isAllocationSite(BI) || isWrapperFuncAnnotation(BI) || isIndirectCallInst(BI)) {
          		ASIDCount++;
          	}
      	}
  	}
  	if(ASIDCount == 0) {
  		return;
  	}

	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	string LASTIDDir = ASIDDir + "/stats/as_last_id.txt";
	string ASIDD = ASIDDir + "/stats";
	ASIDDir = ASIDDir + "/stats/" + filename + "_ASIDInfo.txt";
	synchronize(ASIDD.c_str(), true);

	fstream File1;
	unsigned start = 0, end = 0;
	if(access(ASIDDir.c_str(), F_OK) == -1) {
		start = 1;
	}
	File1.open(ASIDDir.c_str(), ios::in | ios::app);
	assert(File1.good());

	if(File1.peek() != EOF) {
		string str;
		string fidstr, name, startStr, endStr;
		unsigned startTmp = UINT_MAX, endTmp = UINT_MAX;
		while(getline(File1, str)) {
			endTmp = UINT_MAX;
			istringstream ss(str);
			ss >> fidstr;
			ss >> name;
			ss >> startStr;
			ss >> endStr;
			endTmp = stoul(endStr);
			unsigned fid = stoul(fidstr);
			if(FID == fid) {
				assert(F->getName().equals(name));
				assert(endTmp != UINT_MAX);
				startTmp = stoul(startStr);
				assert(endTmp == startTmp + ASIDCount);
				File1.close();
				synchronize(ASIDD.c_str(), false);
				return;
			}
		}
		
		assert(endTmp != UINT_MAX);
		start += endTmp;
	}
	
	File1.clear();
	end = start + ASIDCount;
	File1 << FID << " " << F->getName().str() << " " << start << " " << end << "\n";
	File1.close();
	synchronize(ASIDD.c_str(), false);

	fstream File2;
	File2.open(LASTIDDir.c_str(), ios::out);
	assert(File2.good());
	File2 << end << "\n";
	File2.close();
}

static unsigned getASID(Function *F, unsigned FuncID, unsigned ASIDLocal, string filename) {
	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	ASIDDir = ASIDDir + "/stats/" + filename + "_ASIDInfo.txt";
	ifstream File1;
	File1.open(ASIDDir.c_str());
	assert(File1.good());
	//errs() << "Name:: " << F->getName() << "\n";

	string str1;
	string fid1, name1, startStr1, endStr1;
	unsigned startTmp1 = UINT_MAX;
	while(getline(File1, str1)) {
		istringstream ss(str1);
		ss >> fid1;
		ss >> name1;
		ss >> startStr1;
		ss >> endStr1;
		unsigned fid = stoul(fid1);
		if(FuncID == fid) {
			assert(F->getName().equals(name1));
			startTmp1 = stoul(startStr1);
		}
	}
	File1.close();
	assert(startTmp1 != UINT_MAX);
	unsigned ASID = startTmp1 + ASIDLocal;
	return ASID;
}

void createStoreToGlobal(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI) {
	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		if(AllocaRegionMap[ASID] > 0) {
  			if(isWrapperFuncAnnotation(AI)) {
	  			Type *Int64Ty = Type::getInt64Ty(F->getContext());
	          	if(!isUniqueIDGlobalCreated) {
	          		UniqueASIDGlobal = new GlobalVariable(*AI->getModule(), Int64Ty, false, GlobalValue::ExternalLinkage, 
					ConstantInt::get(Int64Ty, 0), "");
					isUniqueIDGlobalCreated = true;
	          	}
	          	IRBuilder<> IRB(AI);
	          	assert(AllocaRegionMap[ASID] > 0);
	          	IRB.CreateStore(ConstantInt::get(Int64Ty, AllocaRegionMap[ASID]), UniqueASIDGlobal);
	          	assert(AI->getNextNode());
	          	IRBuilder<> IRB1(AI->getNextNode());
	          	//Set the global variable to 0 just to ensure that wrong ASIDs are not used. 
	          	IRB1.CreateStore(ConstantInt::get(Int64Ty, 0), UniqueASIDGlobal);
	        }
  		}
    }
}

void mapAllocationSitesToID(Function *F, unsigned FID, DenseMap<Instruction *, 
	unsigned> &AllocationIDMap, string FileName) {
	unsigned ASIDCount = 0;
	DenseMap<unsigned, Instruction *> IDAllocationMap;
	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
        for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
          	Instruction *BI = &*I;
          	if(isAllocationSite(BI) || isWrapperFuncAnnotation(BI) || isIndirectCallInst(BI)) {
          		assert(!AllocationIDMap.count(BI));
          		AllocationIDMap[BI] = getASID(F, FID, ASIDCount, FileName);
          		assert(AllocationIDMap[BI] > 0);
          		assert(!IDAllocationMap.count(AllocationIDMap[BI]));
          		IDAllocationMap[AllocationIDMap[BI]] = BI;
          		ASIDCount++;
          	}
      	}
  	}
}

void instrumentWriteSameASIDFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!isWriteSameASIDDefAdded && F->getName().equals("main")) {
	    Constant* strConstant = ConstantDataArray::getString(M->getContext(), FileName.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);
	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		writeSameASIDFunc = Function::Create(FTy2,
				        GlobalValue::ExternalLinkage,
				        "writeSameASIDStats", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeSameASIDFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeSameASIDFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	isWriteSameASIDDefAdded = true;
    }
}

void instrumentWriteLoopDynamicChecksLogFunction(Function *F, string FileName) {
	Module *M = F->getParent();
	if(!iswriteLoopDynamicChecksLogDefined && F->getName().equals("main")) {
	    Constant* strConstant = ConstantDataArray::getString(M->getContext(), FileName.c_str());
	    GlobalVariable *TGV = new GlobalVariable(*M, strConstant->getType(), true,
	                GlobalValue::InternalLinkage, strConstant, "");
	    assert(TGV);
	    Type *VoidType = Type::getVoidTy(M->getContext());
    	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  		PointerType *CharStarType = PointerType::getUnqual(CharType);

  		std::vector<Type *> ArgTypes;
		ArgTypes.push_back(CharStarType);
	  	FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		writeLoopDynamicChecksLogFunc = Function::Create(FTy2,
				        GlobalValue::ExternalLinkage,
				        "writeLoopDynamicChecksLog", M);

    	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
		    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
		      	Instruction *II = &*I;
		      	if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
		      		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", RI);
		      		assert(BCI);
		      		std::vector<Value *> ArgValue;
			        ArgValue.push_back(BCI);
			        CallInst::Create(writeLoopDynamicChecksLogFunc, ArgValue, "", RI);
		      	}

		      	if (CallInst *CI = dyn_cast<CallInst>(II)) {
		       		Function *CalledFunc = CI->getCalledFunction();
		        	if (CalledFunc && (CalledFunc->getName().equals("exit") || CalledFunc->getName().equals("WM_exit") )) {
		        		BitCastInst *BCI = new BitCastInst(TGV, CharStarType, "", CI);
			      		assert(BCI);
			      		std::vector<Value *> ArgValue;
				        ArgValue.push_back(BCI);
				        CallInst::Create(writeLoopDynamicChecksLogFunc, ArgValue, "", CI);
		        	}
		        }
		    }
		}

	   	iswriteLoopDynamicChecksLogDefined = true;
    }
}

void instrumentTimeFunction2(Function *F, Loop *L, Loop *NewLoop, unsigned LoopID) {
	assert(L->getLoopPreheader() && NewLoop->getLoopPreheader());
	assert(L->getExitBlock() && NewLoop->getExitBlock());
	Module *M = F->getParent();

  	Type *VoidType = Type::getVoidTy(M->getContext());
  	IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	
  	if(!isTimeFuncDefAdded) {
    	std::vector<Type *> ArgTypes;
	  	ArgTypes.push_back(IntType);
	  	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
		startTimeFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "rdtsc_s", M);

		FunctionType *FTy2 = FunctionType::get(VoidType, ArgTypes, false);
		endTimeFunc = Function::Create(FTy2,
						GlobalValue::ExternalLinkage, 
						"rdtsc_e", M);

  		isTimeFuncDefAdded = true;
  	}

  	Instruction *PreTermInst1 = L->getLoopPreheader()->getTerminator();
  	Instruction *PreTermInst2 = NewLoop->getLoopPreheader()->getTerminator();
  	Instruction *ExitFirstInst1 = &*(L->getExitBlock()->getFirstInsertionPt());
  	Instruction *ExitFirstInst2 = &*(NewLoop->getExitBlock()->getFirstInsertionPt());

  	vector<Value *> ArgValue;
	ArgValue.push_back(ConstantInt::get(IntType, LoopID));
	CallInst::Create(startTimeFunc, ArgValue, "", PreTermInst1);
	CallInst::Create(startTimeFunc, ArgValue, "", PreTermInst2);
	CallInst::Create(endTimeFunc, ArgValue, "", ExitFirstInst1);
	CallInst::Create(endTimeFunc, ArgValue, "", ExitFirstInst2);
}

//Region Based Pointer Disambiguation.
void LoadCheckInfo(string filename, DenseMap<unsigned, 
	vector<pair<unsigned long long, unsigned long long>>> 
	&DynamicCheckMap, DenseMap<unsigned, unsigned> 
	&LoopIDIndiMap, DenseMap<unsigned, unsigned> &LoopIDTotalMap) {
	assert(!filename.empty());
	string LogFile = filename;

	string LogDir;
	const char *LogDirEnv = getenv("HOME");
	assert(LogDirEnv);
	LogDir = LogDirEnv;
	LogDir = LogDir + "/stats/" + LogFile + "_dynamic_checks.txt";

	ifstream File(LogDir);
	if(!File.good()) {
		errs() << "File not found. Filename:: " << LogDir << "\n";
		return;
	}

	if(!File.is_open()) {
		errs() << "Error in opening file. Filename:: " << LogDir << "\n";
		return;
	}

	string str;
	while (getline(File, str)) {
		istringstream ss(str);
		string LoopIDStr;
		string IndividualStr;
		string TotalStr;

		ss >> LoopIDStr;
		ss >> IndividualStr;
		ss >> TotalStr;

		unsigned LoopID;
		unsigned Individual;
		unsigned Total;

		LoopID = stoul(LoopIDStr);
		Individual = stoul(IndividualStr);
		Total = stoul(TotalStr);

		LoopIDIndiMap[LoopID] = Individual;
		LoopIDTotalMap[LoopID] = Total;

		string ID1, ID2;
		while (ss >> ID1 >> ID2) {
			DynamicCheckMap[LoopID].push_back(make_pair(stoull(ID1), stoull(ID2)));
		}
	}
	File.close();
}

bool isCheckInfoFound(string filename) {
	assert(!filename.empty());
	string LogFile = filename;

	string LogDir;
	const char *LogDirEnv = getenv("HOME");
	assert(LogDirEnv);
	LogDir = LogDirEnv;
	LogDir = LogDir + "/stats/" + LogFile + "_dynamic_checks.txt";

	ifstream File(LogDir);
	if(!File.good()) {
		errs() << "File not found. Filename:: " << LogDir << "\n";
		return false;
	}

	if(!File.is_open()) {
		errs() << "Error in opening file. Filename:: " << LogDir << "\n";
		return false;
	}

	string str;
	unsigned count = 0;
	while (getline(File, str)) {
		count++;
	}
	File.close();
	if(count > 0) {
		return true;
	}
	else {
		return false;
	}
}

void CreateGraph(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> DynamicCheckMap, 
	DenseMap<unsigned long long, DenseSet<unsigned long long>> &VertexNeighborMap, unsigned &MAXALLOCAID) {
	for(auto CheckInfo: DynamicCheckMap) {
		for(auto &Pair: CheckInfo.second) {
			MAXALLOCAID = MAXALLOCAID < Pair.first ? Pair.first : MAXALLOCAID;
			MAXALLOCAID = MAXALLOCAID < Pair.second ? Pair.second : MAXALLOCAID;
			if(Pair.first != Pair.second) {
				VertexNeighborMap[Pair.first].insert(Pair.second);
				VertexNeighborMap[Pair.second].insert(Pair.first);
			}
		}
	}
}

void CreateGraphWithSameID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> DynamicCheckMap, 
	DenseMap<unsigned long long, DenseSet<unsigned long long>> &VertexNeighborMap, DenseSet<unsigned> MASameID) {
	for(auto CheckInfo: DynamicCheckMap) {
		for(auto &Pair: CheckInfo.second) {
			if(Pair.first != Pair.second && MASameID.count(Pair.first) && MASameID.count(Pair.second)) {
				VertexNeighborMap[Pair.first].insert(Pair.second);
				VertexNeighborMap[Pair.second].insert(Pair.first);
			}
		}
	}
}

void AssignRegionWithSameID(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID) {
	if(VertexNeighborMap.size() == 0) {
		for(auto ID: MASameID) {
			AllocaRegionMap[ID] = 1;
		}
	}
	for(auto Node: VertexNeighborMap) {
		unsigned Vertex = Node.first;
		if(Vertex == 0) {
			continue;
		}
		assert(MASameID.count(Vertex));
		assert(Vertex < MAXALLOCAID);
		assert(AllocaRegionMap[Vertex] == 0);
		if(Node.second.size() == 0) {
			AllocaRegionMap[Vertex] = 1;
			continue;
		}
		unsigned curregion = 0;
		for(unsigned i = 1; i <  MAXREGION + 1; i++) {
			bool isAssigned = false;
			for(auto Neighbor: Node.second) {
				assert(Neighbor < MAXALLOCAID);
				assert(Vertex != Neighbor);
				if(AllocaRegionMap[Neighbor] == i) {
					isAssigned = true;
					break;
				}
			}

			if(!isAssigned) {
				curregion = i;
				break;
			}
		}
		if(curregion == 0) {
			MAXREGION++;
			AllocaRegionMap[Vertex] = MAXREGION;
		}
		else {
			AllocaRegionMap[Vertex] = curregion;
		}
	}
	if(VertexNeighborMap.size() > 0) {
		assert(MAXREGION > 0);
	}
	if(MAXREGION == 0 && MASameID.size() > 0) {
		MAXREGION = 1;
	}
	assert(MAXREGION < MAXALLOCAID);
}

void AssignRegionToAlloca(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, unsigned &MAXStackRegion) {
	//Assign regions to alloca instructions.
	for(auto Node: VertexNeighborMap) {
		unsigned Vertex = Node.first;
		if(!StackIDSet.count(Vertex)) {
			continue;
		}
		if(Vertex == 0) {
			continue;
		}
		if(MASameID.count(Vertex)) {
			continue;
		}
		assert(Vertex < MAXALLOCAID);
		assert(AllocaRegionMap[Vertex] == 0);
		unsigned curregion = 0;
		for(unsigned i = 1; i <  MAXREGION + 1; i++) {
			bool isAssigned = false;
			for(auto Neighbor: Node.second) {
				assert(Neighbor < MAXALLOCAID);
				assert(Vertex != Neighbor);
				if(AllocaRegionMap[Neighbor] == i) {
					isAssigned = true;
					break;
				}
			}

			if(!isAssigned) {
				curregion = i;
				break;
			}
		}
		if(curregion == 0) {
			MAXREGION++;
			AllocaRegionMap[Vertex] = MAXREGION;
		}
		else {
			AllocaRegionMap[Vertex] = curregion;
		}
	}
	MAXStackRegion = MAXREGION;
}

void AssignRegionToRealloc(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, DenseSet<unsigned> ReallocIDSet) {
	//Assign regions to other instructions.
	for(auto Node: VertexNeighborMap) {
		unsigned Vertex = Node.first;
		if(!ReallocIDSet.count(Vertex)) {
			continue;
		}
		assert(!StackIDSet.count(Vertex));
		if(Vertex == 0) {
			continue;
		}
		if(MASameID.count(Vertex)) {
			continue;
		}
		assert(Vertex < MAXALLOCAID);
		assert(AllocaRegionMap[Vertex] == 0);
		unsigned curregion = 0;
		for(unsigned i = 1; i <  MAXREGION + 1; i++) {
			bool isAssigned = false;
			for(auto Neighbor: Node.second) {
				assert(Neighbor < MAXALLOCAID);
				assert(Vertex != Neighbor);
				if(AllocaRegionMap[Neighbor] == i) {
					isAssigned = true;
					break;
				}
			}

			if(!isAssigned) {
				curregion = i;
				break;
			}
		}
		if(curregion == 0) {
			MAXREGION++;
			AllocaRegionMap[Vertex] = MAXREGION;
		}
		else {
			AllocaRegionMap[Vertex] = curregion;
		}
	}
}

void AssignRegion(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, DenseSet<unsigned> ReallocIDSet) {
	//Assign regions to other instructions.
	for(auto Node: VertexNeighborMap) {
		unsigned Vertex = Node.first;
		if(ReallocIDSet.count(Vertex)) {
			continue;
		}
		if(StackIDSet.count(Vertex)) {
			continue;
		}
		if(Vertex == 0) {
			continue;
		}
		if(MASameID.count(Vertex)) {
			continue;
		}
		assert(Vertex < MAXALLOCAID);
		assert(AllocaRegionMap[Vertex] == 0);
		unsigned curregion = 0;
		for(unsigned i = 1; i <  MAXREGION + 1; i++) {
			bool isAssigned = false;
			for(auto Neighbor: Node.second) {
				assert(Neighbor < MAXALLOCAID);
				assert(Vertex != Neighbor);
				if(AllocaRegionMap[Neighbor] == i) {
					isAssigned = true;
					break;
				}
			}

			if(!isAssigned) {
				curregion = i;
				break;
			}
		}
		if(curregion == 0) {
			MAXREGION++;
			AllocaRegionMap[Vertex] = MAXREGION;
		}
		else {
			AllocaRegionMap[Vertex] = curregion;
		}
	}
}

void createRegionBasedChecks(Function *F, BasicBlock *RuntimeChecksBlock, unsigned LoopID, 
	DenseMap<unsigned, unsigned> LoopIDIndiMap) {
	DenseSet<CallInst*> CheckInst;
	for (BasicBlock::iterator I = RuntimeChecksBlock->begin(); I != RuntimeChecksBlock->end(); ++I) {
		Instruction *BI = &*I;
		if(CallInst *CI = dyn_cast<CallInst>(BI)) {
			Function *Fn = CI->getCalledFunction();
			if(Fn && Fn->getName().equals("_mi_dynamic_check")) {
				CheckInst.insert(CI);
			}
		}
		/*if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
			CheckInst.insert(CI);
		}*/
	}
	assert(CheckInst.size() > 0);
	assert(LoopIDIndiMap[LoopID] == CheckInst.size());

	DenseMap<Value *, Value *> PtrToIntMap;
	for(auto CI: CheckInst) {
		IRBuilder<> IRB(CI);
		auto Base1 = CI->getArgOperand(0);
		auto Base2 = CI->getArgOperand(1);

		Value *O1 = IRB.CreatePtrToInt(Base1, IRB.getInt64Ty());
		Value *O2 = IRB.CreatePtrToInt(Base2, IRB.getInt64Ty());
		assert(O1 && O2);

		Value *ResXOR = IRB.CreateXor(O1, O2);
		Value *RTCall = IRB.CreateICmp(ICmpInst::ICMP_UGE, ResXOR, ConstantInt::get(IRB.getInt64Ty(), REGIONDIST));
		CI->replaceAllUsesWith(RTCall);
		CI->eraseFromParent();
	}
}

void createRegionBasedChecksForOverhead(Function *F, BasicBlock *RuntimeChecksBlock, unsigned LoopID, 
	DenseMap<unsigned, unsigned> LoopIDIndiMap) {
	DenseSet<CallInst*> CheckInst;
	CallInst *Last = NULL;
	for (BasicBlock::iterator I = RuntimeChecksBlock->begin(); I != RuntimeChecksBlock->end(); ++I) {
    Instruction *BI = &*I;
    if(CallInst *CI = dyn_cast<CallInst>(BI)) {
      Function *Fn = CI->getCalledFunction();
      if(Fn && Fn->getName().equals("_mi_dynamic_check")) {
        Last = CI;
        CheckInst.insert(CI);
      }
    }
  }
	assert(CheckInst.size() > 0);
	assert(LoopIDIndiMap[LoopID] == CheckInst.size());
	assert(Last);

	DenseMap<Value *, Value *> PtrToIntMap;
	for(auto CI: CheckInst) {
		if(Last == CI) {
			continue;
		}
		IRBuilder<> IRB(CI);
		auto Base1 = CI->getArgOperand(0);
		auto Base2 = CI->getArgOperand(1);

		Value *O1 = IRB.CreatePtrToInt(Base1, IRB.getInt64Ty());
		Value *O2 = IRB.CreatePtrToInt(Base2, IRB.getInt64Ty());
		assert(O1 && O2);

		Value *ResXOR = IRB.CreateXor(O1, O2);
		Value *RTCall = IRB.CreateICmp(ICmpInst::ICMP_UGE, ResXOR, ConstantInt::get(IRB.getInt64Ty(), REGIONDIST));
		CI->replaceAllUsesWith(RTCall);
		CI->eraseFromParent();
	}

	IRBuilder<> IRB(Last);
	auto Base1 = Last->getArgOperand(0);
	auto Base2 = Last->getArgOperand(1);

	Value *O1 = IRB.CreatePtrToInt(Base1, IRB.getInt64Ty());
	Value *O2 = IRB.CreatePtrToInt(Base2, IRB.getInt64Ty());
	assert(O1 && O2);

	Value *ResXOR = IRB.CreateXor(O1, O2);
	Value *RTCall = IRB.CreateICmp(ICmpInst::ICMP_EQ, ResXOR, ConstantInt::get(IRB.getInt64Ty(), 1));
	Last->replaceAllUsesWith(RTCall);
	Last->eraseFromParent();
}

static void createGetASIDArg(Value *V, DenseMap<Value *, Value *> &ValueASIDMap, 
	Instruction *FirstInst) {
	IntegerType *Char64Type = Type::getInt64Ty(FirstInst->getContext());
	IntegerType *CharType = Type::getInt8Ty(FirstInst->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);

	if(!ValueASIDMap.count(V)) {
		if(BitCastInst *BI1 = dyn_cast<BitCastInst>(V)) {
			assert(BI1->getOperand(0));
			if(isa<AllocaInst>(BI1->getOperand(0))) {
				vector<Value*> Args;
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 0xFFFFFFFE), CharStarType, "", FirstInst);
		    	assert(ITPI);
		    	Args.push_back(ITPI);
				ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
			}
			else if(isUnhandledValue(BI1->getOperand(0))) {
				IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", FirstInst);
	    		assert(ITPI);
	    		vector<Value*> Args;
	    		Args.push_back(ITPI);
				ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
			}
			else {
				vector<Value*> Args;
				Args.push_back(V);
				ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
			}
		}
		else if(isa<AllocaInst>(V)) {
			vector<Value*> Args;
			IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 0xFFFFFFFE), CharStarType, "", FirstInst);
		    assert(ITPI);
		    Args.push_back(ITPI);
			ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
		}
		else if(isUnhandledValue(V)) {
			IntToPtrInst *ITPI = new IntToPtrInst(ConstantInt::get(Char64Type, 64), CharStarType, "", FirstInst);
	    	assert(ITPI);
	    	vector<Value*> Args;
	    	Args.push_back(ITPI);
			ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
		}
		else {
			vector<Value*> Args;
			BitCastInst *BI = new BitCastInst(V, CharStarType, "", FirstInst);
			Args.push_back(BI);
			ValueASIDMap[V] = CallInst::Create(getASIDFunc, Args, "", FirstInst);
		}
	}
}

void createRegionBasedChecksWithID(Function *F, BasicBlock *RuntimeChecksBlock) {
	Instruction *TermInst = RuntimeChecksBlock->getTerminator();
	DenseSet<CallInst*> CheckInst;
	for (BasicBlock::iterator I = RuntimeChecksBlock->begin(); I != RuntimeChecksBlock->end(); ++I) {
		Instruction *BI = &*I;
		CallInst *CI = dyn_cast<CallInst>(BI);
		if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
			CheckInst.insert(CI);
		}
	}
	assert(CheckInst.size() > 0);

	Instruction *FirstInst = nullptr;
	for (BasicBlock::iterator I = RuntimeChecksBlock->begin(); I != RuntimeChecksBlock->end(); ++I) {
		Instruction *BI = &*I;
		CallInst *CI = dyn_cast<CallInst>(BI);
		if (CI && cast<CallInst>(*BI).getIntrinsicID() == Intrinsic::dynamic_noalias_check) {
			FirstInst = BI;
			break;
		}
	}
	assert(FirstInst);

	if(!isgetASIDDefined) {
	  	vector<Type *> ArgTypes;
		ArgTypes.push_back(PointerType::getUnqual(Type::getInt8Ty(TermInst->getContext())));
		//ArgTypes.push_back(Type::getInt64Ty(F->getContext()));
		FunctionType *DTy = FunctionType::get(Type::getInt64Ty(TermInst->getContext()), ArgTypes, false);
		getASIDFunc = Function::Create(DTy, GlobalValue::ExternalLinkage, "getASID", (TermInst->getModule()));
		getASIDFunc->addAttribute(AttributeList::FunctionIndex, Attribute::ReadNone);
		isgetASIDDefined = true;
	}

	DenseMap<Value *, Value *> PtrToIntMap;
	DenseMap<Value *, Value *> ValueASIDMap;

	for(auto CI: CheckInst) {
		assert(CI->getArgOperand(0) && CI->getArgOperand(1));
		auto Base1 = CI->getArgOperand(0);
		auto Base2 = CI->getArgOperand(1);

		createGetASIDArg(Base1, ValueASIDMap, FirstInst);
		createGetASIDArg(Base2, ValueASIDMap, FirstInst);
		assert(ValueASIDMap.count(Base1) && ValueASIDMap.count(Base2));
	}

	for(auto CI: CheckInst) {
		IRBuilder<> IRB(FirstInst);
		auto Base1 = CI->getArgOperand(0);
		auto Base2 = CI->getArgOperand(1);
		auto Cmp = IRB.CreateICmp(ICmpInst::ICMP_NE, ValueASIDMap[Base1], ValueASIDMap[Base2]);
		CI->replaceAllUsesWith(Cmp);
	}

	for(auto CI: CheckInst) {
		CI->eraseFromParent();
	}
}

static void allocaToMalloc(Function *F, AllocaInst *AI, unsigned &ConvertedStackAllocations) {
	auto M = F->getParent();
	auto Int32Ty = Type::getInt32Ty(M->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());
	size_t alignment = 0;
	Value *Size;

	alignment = AI->getAlignment();

	if (AI->isStaticAlloca()) {
		ConvertedStackAllocations++;
		size_t Sz = getAllocaSizeInBytes(*AI);
		/*if (Sz <= 64) {
			AI->setAlignment(MaybeAlign(64));
			return;
		}*/
		Size = ConstantInt::get(Int32Ty, Sz);
		errs() << "AI to Malloc:: " << *AI << " Size: " << Sz << " " << F->getName() <<  "\n";
	}
	else {
		//AI->setAlignment(MaybeAlign(64));
  		//Size = getAllocaSize(AI);
		// FIXME::
		return;
	}


  	Instruction *Entry = dyn_cast<Instruction>(F->begin()->getFirstInsertionPt());
	IRBuilder<> IRB(Entry);
	auto Fn = M->getOrInsertFunction("aligned_alloc", AI->getType(), Int32Ty, Size->getType());
	auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Int32Ty, alignment), Size});
	SmallVector<pair<unsigned, MDNode *>, 4> MDs;
	AI->getAllMetadata(MDs);
	for(const auto &MI : MDs) {
		Call->setMetadata(MI.first, MI.second);
	}
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

bool doesExternalCall(Function &F) {
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

#if 0
static Value* getGlobalUL(Module *M) {
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
#endif

/*static void setGlobalLimit(Function &F) {
  Instruction *Entry = dyn_cast<Instruction>(F.begin()->getFirstInsertionPt());
	IRBuilder<> IRB(Entry);
	auto GlobalLimit = getGlobalUL(F.getParent());
	IRB.CreateStore(ConstantInt::get(IRB.getInt64Ty(), 0xFFFFFFFF), GlobalLimit);
}*/

void convertAllocaToMalloc(Function &F, TargetLibraryInfo *TLI, unsigned &ConvertedStackAllocations) {
	vector<AllocaInst *> AISet;
	for (auto &BB : F) {
		for (auto &I : BB) {
			AllocaInst *AI = dyn_cast<AllocaInst>(&I);
			if (AI && !isSafeAlloca(AI, *TLI)) {
				AISet.push_back(AI);
			}
		}
	}

	for (auto AI : AISet) {
		allocaToMalloc(&F, AI, ConvertedStackAllocations);
	}
			
	/*if (F.getName() == "main") {
		setGlobalLimit(F);
	}*/
}

bool ifFileExists(string filename) {
	const char *DirEnv = getenv("HOME");
	assert(DirEnv);
	string ModFile = DirEnv;
	ModFile += "/stats/" + filename + "_ModuleInfo.txt";
	if(access(ModFile.c_str(), F_OK) == -1) {
		return false;
	}

	string FuncFile = DirEnv;
	FuncFile += "/stats/" + filename + "_FunctionInfo.txt";
	if(access(FuncFile.c_str(), F_OK) == -1) {
		return false;
	}

	string ASIDFile = DirEnv;
	ASIDFile += "/stats/" + filename + "_ASIDInfo.txt";
	if(access(ASIDFile.c_str(), F_OK) == -1) {
		return false;
	}

	string ChecksFile = DirEnv;
	ChecksFile += "/stats/" + filename + "_dynamic_checks.txt";
	if(access(ChecksFile.c_str(), F_OK) == -1) {
		return false;
	}

	/*string StackInfoFile = DirEnv;
	StackInfoFile += "/stats/" + filename + "_StackInfo.txt";
	if(access(StackInfoFile.c_str(), F_OK) == -1) {
		return false;
	}*/

	return true;
}

void DiscardLoopsWithSameID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> 
	DynamicCheckMap, DenseMap<unsigned, unsigned> LoopIDIndiMap, 
	DenseMap<unsigned, unsigned> LoopIDTotalMap, DenseMap<unsigned, bool> &CanAddCheckLoop) {
	for(auto LoopCheck: DynamicCheckMap) {
		auto LoopID = LoopCheck.first;
		unsigned SameIDCount = 0;
		bool isIDSame = false;
		unsigned i = 0;
		for(auto Check: LoopCheck.second) {
			if(Check.first == Check.second) {
				isIDSame = true;
			}
			i++;
			if(i == LoopIDIndiMap[LoopID]) {
				i = 0;
				if(isIDSame) {
					SameIDCount++;
				}
				isIDSame = false;
			}
		}
		if(SameIDCount == (LoopIDTotalMap[LoopID] / LoopIDIndiMap[LoopID])) {
			CanAddCheckLoop[LoopID] = false;
		}
		else {
			CanAddCheckLoop[LoopID] = true;
		}
	}
}

void getSameMAID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> 
	DynamicCheckMap, DenseSet<unsigned> &MASameID) {
	for(auto LoopCheck: DynamicCheckMap) {
		//auto LoopID = LoopCheck.first;
		for(auto Check: LoopCheck.second) {
			if(Check.first > 0 && Check.second > 0 && Check.first == Check.second && !MASameID.count(Check.first)) {
				MASameID.insert(Check.first);
			}
		}
	}
}

void getRegionIDForSameID(unsigned MAXREGION, unsigned AllocaRegionMap[], unsigned threshold, 
	DenseSet<unsigned> MASameID, DenseMap<unsigned, unsigned> &StartIDMap,
	DenseMap<unsigned, unsigned> &EndIDMap, unsigned MAXREGION1) {
	assert(MAXREGION1 > 0);
	DenseMap<unsigned, pair<unsigned, unsigned>> RegionMap;
	unsigned AvailableRegions = threshold - (MAXREGION + 1);
	unsigned quo = AvailableRegions / MAXREGION1;
	unsigned rem = AvailableRegions % MAXREGION1;

	unsigned startid = MAXREGION + 1;
	for (unsigned i = 1; i < MAXREGION1 + 1; i++) {
		unsigned endid = startid + quo - 1;
		if(rem > 0) {
			endid++;
			rem--;
		}
		RegionMap[i] = make_pair(startid, endid);
		startid = endid + 1;
		assert(RegionMap[i].first < threshold && RegionMap[i].second < threshold);
	}

	for(auto ASID: MASameID) {
		assert(AllocaRegionMap[ASID] > 0);
		assert(RegionMap.count(AllocaRegionMap[ASID]));
		StartIDMap[ASID] = RegionMap[AllocaRegionMap[ASID]].first;
		EndIDMap[ASID] = RegionMap[AllocaRegionMap[ASID]].second;
	}
}

void replaceMemoryAllocationsWithSameID(Function *F, unsigned MAXREGION, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap,
	DenseMap<unsigned, unsigned> EndIDMap) {
	Module *M = F->getParent();
	auto Int32Ty = Type::getInt32Ty(F->getContext());
	auto Int8PtrTy = Type::getInt8PtrTy(F->getContext());
	auto Int64Ty = Type::getInt64Ty(F->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());

	//DenseMap<CallInst *, unsigned> ReallocMap;
	for(auto Pair: AllocationIDMap) {
		Instruction *AI = Pair.first;
		unsigned ASID = Pair.second;
		//if(AllocaRegionMap[ASID] > 0) {
		if(StartIDMap.count(ASID)) {
			assert(EndIDMap.count(ASID));
			unsigned startID = StartIDMap[ASID];//MAXREGION + 1 + ((AllocaRegionMap[ASID] - 1) * REGIONLIMIT);
			unsigned endID = EndIDMap[ASID];//MAXREGION + (AllocaRegionMap[ASID] * REGIONLIMIT);
			assert(startID < threshold && endID < threshold);
			Type *RegionNumberTy = Type::getInt64Ty(F->getContext());
			GlobalVariable *RegionNumber = new GlobalVariable(*M, RegionNumberTy, false, GlobalValue::InternalLinkage, 
				ConstantInt::get(Int64Ty, startID), "");
			assert(RegionNumber);

			if(isMalloc(AI) || isNewOperator(AI)) {
	  			if(CallInst *CI = dyn_cast<CallInst>(AI)) {
	  				SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  					AI->getAllMetadata(MDs);
  					auto AS = CI->getAttributes();
	  				IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_malloc", Int8PtrTy, CI->getOperand(0)->getType(), Int64Ty);
					LoadInst *Load = IRB.CreateLoad(RegionNumber);
					auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), Load});
					Value *Inc = IRB.CreateAdd(IRB.getInt64(1), Load);
					Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
						IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
					IRB.CreateStore(Sel, RegionNumber);
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
  					if(CI->isTailCall()) {
  						Call->setTailCall();
  					}
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();
					ReplacedMA++;
	  			}
	  			else if(InvokeInst *II = dyn_cast<InvokeInst>(AI)) {
	  				SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  					AI->getAllMetadata(MDs);
  					auto AS = II->getAttributes();
	  				IRBuilder<> IRB(AI);
					auto Fn = M->getOrInsertFunction("mi_region_malloc", Int8PtrTy, II->getOperand(0)->getType(), Int64Ty);
					LoadInst *Load = IRB.CreateLoad(RegionNumber);
					auto Call = IRB.CreateInvoke(Fn, II->getNormalDest(), II->getUnwindDest(), 
						{II->getOperand(0), Load});
					IRBuilder<> IRB1(II->getNormalDest()->getFirstNonPHI());
					Value *Inc = IRB1.CreateAdd(IRB1.getInt64(1), Load);
					Value *Sel = IRB1.CreateSelect(IRB1.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
						IRB1.getInt64(endID)), IRB1.getInt64(startID), Inc);
					IRB1.CreateStore(Sel, RegionNumber);
					for(const auto &MI : MDs) {
    					Call->setMetadata(MI.first, MI.second);
  					}
  					Call->setAttributes(AS);
					AI->replaceAllUsesWith(Call);
					AI->eraseFromParent();
					ReplacedMA++;
	  			}
	  			else {
	  				assert(0);
	  			}
	  		}
	  		else if(isCalloc(AI)) {
	  			CallInst *CI = dyn_cast<CallInst>(AI);
	  			assert(CI);
	  			SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  				AI->getAllMetadata(MDs);
  				auto AS = CI->getAttributes();
	  			IRBuilder<> IRB(AI);
				auto Fn = M->getOrInsertFunction("mi_region_calloc", Int8PtrTy, CI->getOperand(0)->getType(), 
					CI->getOperand(1)->getType(), Int64Ty);
				LoadInst *Load = IRB.CreateLoad(RegionNumber);
				auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), Load});
				Value *Inc = IRB.CreateAdd(IRB.getInt64(1), Load);
				Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
						IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
				IRB.CreateStore(Sel, RegionNumber);
				for(const auto &MI : MDs) {
    				Call->setMetadata(MI.first, MI.second);
  				}
  				Call->setAttributes(AS);
  				if(CI->isTailCall()) {
  					Call->setTailCall();
  				}
				AI->replaceAllUsesWith(Call);
				AI->eraseFromParent();
				ReplacedMA++;
	  		}
	  		else if(isRealloc(AI)) {
	  			CallInst *CI = dyn_cast<CallInst>(AI);
  				assert(CI);
	  			SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  				AI->getAllMetadata(MDs);
  				auto AS = CI->getAttributes();
  				IRBuilder<> IRB(AI);
				LoadInst *Load = IRB.CreateLoad(RegionNumber);
				auto Fn = M->getOrInsertFunction("mi_region_realloc", CI->getType(), CI->getOperand(0)->getType(), CI->getOperand(1)->getType(), Int64Ty);
				auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), Load});
				Value *Inc = IRB.CreateAdd(IRB.getInt64(1), Load);
				Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
							IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
				IRB.CreateStore(Sel, RegionNumber);
				for(const auto &MI : MDs) {
    				Call->setMetadata(MI.first, MI.second);
  				}
  				Call->setAttributes(AS);
  				if(CI->isTailCall()) {
  					Call->setTailCall();
  				}
				AI->replaceAllUsesWith(Call);
				AI->eraseFromParent();
				ReplacedMA++;
			}
	  		else if(isAlignedAlloc(AI)) {
  				CallInst *CI = dyn_cast<CallInst>(AI);
  				assert(CI);
  				SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  				AI->getAllMetadata(MDs);
  				auto AS = CI->getAttributes();
  				IRBuilder<> IRB(AI);
				LoadInst *Load = IRB.CreateLoad(RegionNumber);
				auto Fn = M->getOrInsertFunction("mi_region_aligned_alloc", CI->getType(), CI->getOperand(0)->getType(), CI->getOperand(1)->getType(), Int64Ty);
				auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), Load});
				Value *Inc = IRB.CreateAdd(IRB.getInt64(1), Load);
				Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
							IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
				IRB.CreateStore(Sel, RegionNumber);
				for(const auto &MI : MDs) {
    				Call->setMetadata(MI.first, MI.second);
  				}
  				Call->setAttributes(AS);
  				if(CI->isTailCall()) {
  					Call->setTailCall();
  				}
				AI->replaceAllUsesWith(Call);
				AI->eraseFromParent();
				ReplacedMA++;
			}
	  		else if(isPosixMemalign(AI)) {
	  			CallInst *CI = dyn_cast<CallInst>(AI);
	  			assert(CI);
	  			SmallVector<pair<unsigned, MDNode *>, 4> MDs;
  				AI->getAllMetadata(MDs);
  				auto AS = CI->getAttributes();
	  			IRBuilder<> IRB(AI);
	  			LoadInst *Load = IRB.CreateLoad(RegionNumber);
	  			auto Fn = M->getOrInsertFunction("mi_region_posix_memalign", AI->getType(), CI->getOperand(0)->getType(), 
	  					CI->getOperand(1)->getType(), CI->getOperand(2)->getType(), Int64Ty);
	  			auto Call = IRB.CreateCall(Fn, {CI->getOperand(0), CI->getOperand(1), CI->getOperand(2), Load});
	  			Value *Inc = IRB.CreateAdd(IRB.getInt64(1), Load);
	  			Value *Sel = IRB.CreateSelect(IRB.CreateICmp(ICmpInst::ICMP_UGT, Inc, 
	  					IRB.getInt64(endID)), IRB.getInt64(startID), Inc);
	  			StoreInst *Store = IRB.CreateStore(Sel, RegionNumber);
	  			for(const auto &MI : MDs) {
    				Call->setMetadata(MI.first, MI.second);
  				}
  				Call->setAttributes(AS);
  				if(CI->isTailCall()) {
  					Call->setTailCall();
  				}
	  			AI->replaceAllUsesWith(Call);
	  			AI->eraseFromParent();
	  			ReplacedMA++;
	  		}
		}
	}
}

void instrumentSameCountFunc(DenseSet<unsigned> MASameID, DenseMap<Instruction *, unsigned> AllocationIDMap) {
	for(auto Pair: AllocationIDMap) {
  		Instruction *AI = Pair.first;
  		unsigned ASID = Pair.second;
  		if(MASameID.count(ASID)) {
  			assert(AI->getModule());
  			auto M = AI->getModule();
  			IRBuilder<> IRB(AI);
			auto Fn = M->getOrInsertFunction("computeSameMACount", Type::getVoidTy(M->getContext()), 
				Type::getInt64Ty(M->getContext()));
			auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Type::getInt64Ty(M->getContext()), ASID)});
  		}
  	}
}

static void allocaToMallocForUse(Function *F, AllocaInst *AI, DenseMap<Instruction *, Instruction *> &AlignedAllocToAllocMap) {
	auto M = F->getParent();
	auto Int32Ty = Type::getInt32Ty(M->getContext());
	auto RetTy = Type::getVoidTy(M->getContext());
	size_t alignment = 0;
	Value *Size;

	alignment = AI->getAlignment();

	if (AI->isStaticAlloca()) {
		size_t Sz = getAllocaSizeInBytes(*AI);
		Size = ConstantInt::get(Int32Ty, Sz);
		errs() << "AI to Malloc:: " << *AI << " Size: " << Sz << " " << F->getName() <<  "\n";
	}
	else {
		//AI->setAlignment(MaybeAlign(64));
  		//Size = getAllocaSize(AI);
		// FIXME::
		return;
	}


  	Instruction *Entry = dyn_cast<Instruction>(F->begin()->getFirstInsertionPt());
	IRBuilder<> IRB(Entry);
	auto Fn = M->getOrInsertFunction("aligned_alloc", AI->getType(), Int32Ty, Size->getType());
	auto Call = IRB.CreateCall(Fn, {ConstantInt::get(Int32Ty, alignment), Size});
	AlignedAllocToAllocMap[Call] = AI;
}

void convertAllocaToMallocForUse(Function &F, TargetLibraryInfo *TLI, DenseMap<Instruction *, Instruction *> &AlignedAllocToAllocMap) {
	vector<AllocaInst *> AISet;
	for (auto &BB : F) {
		for (auto &I : BB) {
			AllocaInst *AI = dyn_cast<AllocaInst>(&I);
			if (AI && !isSafeAlloca(AI, *TLI)) {
				AISet.push_back(AI);
			}
		}
	}

	for (auto AI : AISet) {
		allocaToMallocForUse(&F, AI, AlignedAllocToAllocMap);
	}
}

void mapAlignedAllocIDToAlloc(DenseMap<Instruction *, unsigned> &AllocationIDMap, DenseMap<Instruction *, unsigned> 
	AllocationIDMap1, DenseMap<Instruction *, Instruction *> AlignedAllocToAllocMap) {
	for(auto Pair: AllocationIDMap1) {
		if(AlignedAllocToAllocMap.count(Pair.first)) {
			AllocationIDMap[AlignedAllocToAllocMap[Pair.first]] = Pair.second;
		}
		else {
			AllocationIDMap[Pair.first] = Pair.second;
		}
	}

	for(auto Pair: AlignedAllocToAllocMap) {
		Pair.first->eraseFromParent();
	}
}

void getStackID(string filename, DenseSet<unsigned> &StackIDSet) {
	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	string ASIDD = ASIDDir + "/stats";
	string StackInfoFilename = ASIDD + "/" + filename + "_StackInfo.txt";
	synchronize(ASIDD.c_str(), true);

	if(access(StackInfoFilename.c_str(), F_OK) == -1) {
		synchronize(ASIDD.c_str(), false);
		return;
	}

	fstream File1;
	File1.open(StackInfoFilename.c_str(), ios::in);
	assert(File1.good());

	if(File1.peek() != EOF) {
		string str, IDstr;
		unsigned ID = UINT_MAX;
		while(getline(File1, str)) {
			istringstream ss(str);
			ss >> IDstr;
			ID = stoul(IDstr);
			StackIDSet.insert(ID);
		}
	}
	File1.close();
	synchronize(ASIDD.c_str(), false);
}

void storeStackID(string filename, DenseMap<Instruction *, unsigned> AllocationIDMap) {
	if(AllocationIDMap.size() == 0) {
		return;
	}

	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	string ASIDD = ASIDDir + "/stats";
	string StackInfoFilename = ASIDD + "/" + filename + "_StackInfo.txt";
	/*static DenseSet<unsigned> StackIDSet;
	if(!isStackInfoLoaded) {
		getStackID(filename, StackIDSet);
		isStackInfoLoaded = true;
	}*/
	synchronize(ASIDD.c_str(), true);

	fstream File1;
	File1.open(StackInfoFilename.c_str(), ios::app);
	assert(File1.good());
	for(auto Pair: AllocationIDMap) {
		if(/*!StackIDSet.count(Pair.second) &&*/ isAlignedAlloc(Pair.first)) {
			File1 << Pair.second << "\n";
		}
	}
	File1.close();
	synchronize(ASIDD.c_str(), false);
}

void getReallocID(string filename, DenseSet<unsigned> &ReallocIDSet) {
	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	string ASIDD = ASIDDir + "/stats";
	string ReallocInfoFilename = ASIDD + "/" + filename + "_ReallocInfo.txt";
	synchronize(ASIDD.c_str(), true);

	if(access(ReallocInfoFilename.c_str(), F_OK) == -1) {
		synchronize(ASIDD.c_str(), false);
		return;
	}

	fstream File1;
	File1.open(ReallocInfoFilename.c_str(), ios::in);
	assert(File1.good());

	if(File1.peek() != EOF) {
		string str, IDstr;
		unsigned ID = UINT_MAX;
		while(getline(File1, str)) {
			istringstream ss(str);
			ss >> IDstr;
			ID = stoul(IDstr);
			ReallocIDSet.insert(ID);
		}
	}
	File1.close();
	synchronize(ASIDD.c_str(), false);
}

void storeReallocID(string filename, DenseMap<Instruction *, unsigned> AllocationIDMap) {
	if(AllocationIDMap.size() == 0) {
		return;
	}

	const char *ASIDDirEnv = getenv("HOME");
	assert(ASIDDirEnv);
	string ASIDDir = ASIDDirEnv;
	string ASIDD = ASIDDir + "/stats";
	string ReallocInfoFilename = ASIDD + "/" + filename + "_ReallocInfo.txt";
	synchronize(ASIDD.c_str(), true);

	fstream File1;
	File1.open(ReallocInfoFilename.c_str(), ios::app);
	assert(File1.good());
	for(auto Pair: AllocationIDMap) {
		if(isRealloc(Pair.first)) {
			File1 << Pair.second << "\n";
		}
	}
	File1.close();
	synchronize(ASIDD.c_str(), false);
}

void setStackRegionReq(unsigned MAXStackRegion) {
	StackRegionReq = MAXStackRegion;
}

unsigned getStackRegionReq() {
	return StackRegionReq;
}

static bool handleAllocationWithSameIDForNative(Function *F, unsigned ASID, Instruction *AI, unsigned threshold, 
	DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap, Value *GVar) {
	if(!StartIDMap.count(ASID)) {
		return false;
	}
	assert(EndIDMap.count(ASID));
	Module *M = F->getParent();
	auto Int64Ty = Type::getInt64Ty(F->getContext());
	unsigned startID = StartIDMap[ASID];//MAXREGION + 1 + ((AllocaRegionMap[ASID] - 1) * REGIONLIMIT);
	unsigned endID = EndIDMap[ASID];//MAXREGION + (AllocaRegionMap[ASID] * REGIONLIMIT);
	assert(startID < threshold && endID < threshold);
	GlobalVariable *RegionNumber = new GlobalVariable(*M, Int64Ty, false, GlobalValue::InternalLinkage,
				ConstantInt::get(Int64Ty, 0), Twine(ASID).str());
	assert(RegionNumber);
	IRBuilder<> IRB(AI);
	LoadInst *Load = IRB.CreateLoad(RegionNumber);
	IRB.CreateStore(Load, GVar);
	return true;
}

void replaceMemoryAllocationsForNative(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, bool EnableWrapperAnnotation, 
	unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap) {
	Module *M = F->getParent();
	assert(M);
  auto Int32Ty = Type::getInt32Ty(F->getContext());
	auto Int8PtrTy = Type::getInt8PtrTy(F->getContext());
	auto Int64Ty = Type::getInt64Ty(F->getContext());
	auto RetTy = Type::getVoidTy(F->getContext());

  Value *GVar = NULL;

  if (!AllocationIDMap.empty()) {
    Type *Ty = Type::getInt64Ty(M->getContext());
    GVar = getGlobalVar(M, "__mi_region", Ty);
  }

	for (auto Pair: AllocationIDMap) {
    Instruction *AI = Pair.first;
    unsigned ASID = Pair.second;

    if(StartIDMap.size() > 0 && handleAllocationWithSameIDForNative(F, ASID, AI, threshold, StartIDMap, EndIDMap, GVar)) {
    	errs() << "handleAllocationWithSameIDForNative returns true\n";
    	continue;
    }

    unsigned long long ID = AllocaRegionMap[ASID];

    if (ID > 1) {
    	ID = 0;
	    ReplacedMA++;
      if (isWrapperFuncAnnotation(AI)) {
    	  assert(!F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc));
        setRegionID(F, AI, ID, GVar);
	    }
	    else if(isIndirectCallInst(AI)) {
	    	setRegionID(F, AI, ID, GVar);
	    }
    	else if (!F->hasFnAttribute(Attribute::MemoryAllocWrapperFunc)) {
        if (isMalloc(AI) || isNewOperator(AI) || isCalloc(AI) || isRealloc(AI) || isAlignedAlloc(AI) || isPosixMemalign(AI)) {
          setRegionID(F, AI, ID, GVar);
        }
    	}
    }
  }
}

void instrumentUpdateLoopIterFunction(Loop *CLoop, unsigned LoopID) {
	assert(CLoop->getLoopPreheader());
	Function *F = CLoop->getLoopPreheader()->getParent();
	Module *M = F->getParent();
  Type *VoidType = Type::getVoidTy(M->getContext());
  IntegerType *IntType = Type::getInt32Ty(M->getContext());
  	
  	if(!isUpdateLoopIterAdded) {
    	std::vector<Type *> ArgTypes;
	  	ArgTypes.push_back(IntType);
	  	FunctionType *FTy1 = FunctionType::get(VoidType, ArgTypes, false);
			updateLoopReachedFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "updateLoopReached", M);
			updateLoopIterationsFunc = Function::Create(FTy1,
				        GlobalValue::ExternalLinkage,
				        "updateLoopIterations", M);

  		isUpdateLoopIterAdded = true;
  	}

    std::vector<Value *> ArgValue;
		ArgValue.push_back(ConstantInt::get(IntType, LoopID));
		CallInst::Create(updateLoopReachedFunc, ArgValue, "", CLoop->getLoopPreheader()->getTerminator());
		for (BasicBlock *BB : CLoop->blocks()) {
			assert(BB->getFirstNonPHI());
			CallInst::Create(updateLoopIterationsFunc, ArgValue, "", BB->getFirstNonPHI());
			break;
		}
}
