#include "llvm/Transforms/Vectorize/CustomVectorization.h"

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
			else {
				BitCastInst *BitI = new BitCastInst(V2, CharStarType, "", TermInst);
	    		assert(BitI);
	    		InstBitcast[V2] = BitI;
			}
		}
		
		AddedBases.insert(P1);
		assert(InstBitcast[V1] && InstBitcast[V2]);
		BasesBitcast.insert(make_pair(InstBitcast[V1], InstBitcast[V2]));
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

void emitRuntimeChecks(Loop *CLoop, Loop *NewLoop, BasicBlock *RuntimeChecksBlock, 
	DenseSet<pair<Value *, Value *>> Bases, bool performCloning, DenseSet<pair<Value *, Value *>> &AddedBases) {

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
  	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);

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
		Function *DynamicCheckIntrisic = Intrinsic::getDeclaration(RuntimeChecksBlock->
	  		getParent()->getParent(), Intrinsic::dynamic_noalias_check);
	  	assert(DynamicCheckIntrisic);
		Value *RTCall = IRB1.CreateCall(DynamicCheckIntrisic, Args);
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

static BasicBlock* getOuterMostLoop(BasicBlock *BB, LoopInfo &LI, PostDominatorTree &PDT)
{
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

static Value* getAlignment2k(Function &F, BasicBlock *BB, Value *Base, DenseMap<Value*, Value*> &BaseToAlignment)
{
	if (BaseToAlignment.count(Base)) {
		return BaseToAlignment[Base];
	}
	IRBuilder<> IRB(BB->getTerminator());
	auto Fn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::alignment2k);
	auto Alignment = IRB.CreateCall(Fn, {IRB.CreateBitCast(Base, IRB.getInt8PtrTy())});
	BaseToAlignment[Base] = Alignment;
	return Alignment;
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

/*static void moveInstToRuntimeBlock(Loop *CLoop, BasicBlock *RuntimeChecksBlock) {
	DenseSet <Instruction *> InstrToMove;
	for (BasicBlock::iterator I = CLoop->getLoopPreheader()->begin(); 
		 I != CLoop->getLoopPreheader()->end(); ++I) {
		Instruction *BI = &*I;
		if(isa<PHINode>(BI)) {
			InstrToMove.insert(BI);
		}
	}

	for(auto I : InstrToMove) {
      	I->getParent()->getInstList().remove(I);
      	I->insertBefore(RuntimeChecksBlock->getFirstNonPHI());
	}
}*/

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
		        	if (CalledFunc && CalledFunc->getName().equals("exit")) {
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
		        	if (CalledFunc && CalledFunc->getName().equals("exit")) {
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
  	IntegerType *CharType = Type::getInt8Ty(M->getContext());
  	PointerType *CharStarType = PointerType::getUnqual(CharType);
  	
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
			    if(ReturnInst *RI = dyn_cast<ReturnInst>(II)) {
			      	CallInst::Create(endFuncTimeFunc, ArgValue, "", II);
			    }

			    if (CallInst *CI = dyn_cast<CallInst>(II)) {
			       	Function *CalledFunc = CI->getCalledFunction();
			        if (CalledFunc && CalledFunc->getName().equals("exit")) {
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
		if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
			BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		}
		if(auto M = BI->getMetadata(LLVMContext::MD_noalias)) {
			BI->setMetadata(LLVMContext::MD_noalias, NULL);
		}
	}

	for(BasicBlock *BB : CLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
	      	Instruction *BI = &*I;
	      	if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
				BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
			}
			if(auto M = BI->getMetadata(LLVMContext::MD_noalias)) {
				BI->setMetadata(LLVMContext::MD_noalias, NULL);
			}
	    }
	}

	for (BasicBlock::iterator I = CLoop->getExitBlock()->begin(); 
		I != CLoop->getExitBlock()->end(); ++I) {
	    Instruction *BI = &*I;
		if(auto M = BI->getMetadata(LLVMContext::MD_alias_scope)) {
			BI->setMetadata(LLVMContext::MD_alias_scope, NULL);
		}
		if(auto M = BI->getMetadata(LLVMContext::MD_noalias)) {
			BI->setMetadata(LLVMContext::MD_noalias, NULL);
		}
	}
}

/*static bool canAddRuntimeChecks(Loop *CLoop) {
	DenseSet<Instruction *> LoadInsts, StoreInsts;
	for(BasicBlock *BB : CLoop->blocks()) {
	  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
	      	Instruction *BI = &*I;
	      	if(isa<LoadInst>(BI)) {
	      		LoadInsts.insert(BI);
	      	}
	      	if(isa<StoreInst>(BI)) {
	      		StoreInsts.insert(BI);
	      	}
	    }
	}
	return (LoadInsts.size() > 0 && StoreInsts.size() > 0);
}*/

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
		IntegerType *IntType = Type::getInt1Ty(NewLoop->getLoopPreheader()
				->getTerminator()->getContext());
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

unsigned getLoopID(string filename) {
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
}

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
		errs() << "LoopID:: " << LoopID << " result::" << result << "\n";
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
	assert(isInnermostLoop(L));
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
	emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, Bases, true, AddedBases);
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
    *RCB = RuntimeChecksBlock;
    
	//return NewLoop;
}
