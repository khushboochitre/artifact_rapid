#include "llvm/Pass.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Vectorize/CustomVectorization.h"

using namespace llvm;
using namespace std;

#define DEBUG_TYPE IW_NAME
#define IW_NAME "identify-wrappers"

STATISTIC(Warning1, "Number of cases with the both store operands with base as memory allocation");
STATISTIC(Warning2, "Number of cases with memory allocation stored to class member");
STATISTIC(WrapperCount1, "Number of wrapper functions with memory allocations");
STATISTIC(WrapperCount2, "Number of wrapper functions calling another wrapper functions");
STATISTIC(WrapperCount, "Number of wrapper functions");

namespace {
	struct IdentifyWrappers : public ModulePass {
		static char ID;

		IdentifyWrappers();
		virtual void getAnalysisUsage(AnalysisUsage &AU) const;
	  	virtual bool runOnModule(Module& M);
	};
}

IdentifyWrappers::IdentifyWrappers(): ModulePass(ID) {}

void IdentifyWrappers::getAnalysisUsage(AnalysisUsage &AU) const {}

static void isStandardAllocationAPI(Function *F, DenseSet<Instruction *> &AllocInstSet, 
	DenseSet<Instruction *> &StoreInstSet, DenseSet<Instruction *> &CallInstSet, 
	DenseSet<Instruction *> &RetInstSet, DenseSet<Instruction *> &InvokeInstSet) {

	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
      	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *Inst = &*I;
      		if(isAllocationSite(Inst)) {
      			AllocInstSet.insert(Inst);
	    	}
	    	else if(isa<StoreInst>(Inst)) {
	    		StoreInstSet.insert(Inst);
	    	}
	    	else if(isa<CallInst>(Inst)) {
	    		CallInst *CI = dyn_cast<CallInst>(Inst);
	    		assert(CI);
	    		if(CI->getIntrinsicID() == Intrinsic::not_intrinsic) {
	    			CallInstSet.insert(Inst);
	    		}
	    	}
	    	else if(isa<InvokeInst>(Inst)) {
	    		InvokeInstSet.insert(Inst);
	    	}
	    	else if(Inst->getNumOperands() > 0 && isa<ReturnInst>(Inst)) {
      			RetInstSet.insert(Inst);
      		}
      	}
    }
}

static void getBases(Function *F, Value *V, SmallVector<const Value *, 4> &Objects) {
	const DataLayout &DL = F->getParent()->getDataLayout();
	auto B = GetUnderlyingObject(V, DL);
    if(Instruction *I = dyn_cast<Instruction>(B)) {
    	if((isa<PHINode>(I) || isa<SelectInst>(I))) {
    		LoopInfo *LI;
    		GetUnderlyingObjects(V, Objects, DL, LI, 0);
    	}
    	else {
    		Objects.push_back(B);
    	}
    }
    else {
    	Objects.push_back(B);
    }
}

static bool isBaseAlloc(SmallVector<const Value *, 4> Objects, 
	DenseSet<Instruction *> AllocInstSet) {
	for(auto B: Objects) {
		if(ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(const_cast<Value*>(B))) {
			continue;
		}
		Instruction *BI = dyn_cast<Instruction>(const_cast<Value*>(B));
		if(BI && AllocInstSet.count(BI)) {
			return true;
		}
	}
	return false;
}

/*static bool isBaseAllocReturn(SmallVector<const Value *, 4> Objects, 
	DenseSet<Instruction *> AllocInstSet) {
	//FIXME: All the bases should be derived from memory allocations.
	if(Objects.size() == 0) {
		return false;
	}

	for(auto B: Objects) {
		Instruction *BI = dyn_cast<Instruction>(const_cast<Value*>(B));
		if(!BI) {
			return false;
		}
		if(!AllocInstSet.count(BI)) {
			return false;
		}
	}
	return true;
}*/

static bool isReturnDerived(Function *F, DenseSet<Instruction *> AllocInstSet, 
	DenseSet<Instruction *> RetInstSet) {
	if(RetInstSet.size() == 0) {
		return false;
	}

	for(auto R: RetInstSet) {
		ReturnInst *RI = dyn_cast<ReturnInst>(R);
		assert(RI);
		assert(RI->getNumOperands() == 1);
		assert(RI->getOperand(0));

		//Polybench wrapper function.
		if(LoadInst *LI = dyn_cast<LoadInst>(RI->getOperand(0))) {
			bool flag = false;
			for(auto A: AllocInstSet) {
				if(isPosixMemalign(A)) {
					CallInst *CI = dyn_cast<CallInst>(A);
      				assert(CI);
      				assert(CI->arg_size() > 0);
      				assert(CI->getArgOperand(0));
      				if(LI->getPointerOperand() == CI->getArgOperand(0)) {
      					flag = true;
      					break;
      				}
				}
			}
			if(flag) {
				continue;
			}
		}

		SmallVector<const Value *, 4> Objects;
		getBases(F, RI->getOperand(0), Objects);
		if(!isBaseAlloc(Objects, AllocInstSet)) {
			return false;
		}
	}
	return true;
}

bool isClassType(Value *V) {
	std::string prefix = "%\"class.";
	string type_str;
	raw_string_ostream rso(type_str);
	V->getType()->print(rso);
	if(rso.str().rfind(prefix, 0) == 0) {
		return true;
	}
	return false;
}

static bool isAllocCaptured(Function *F, DenseSet<Instruction *> AllocInstSet, 
	DenseSet<Instruction *> StoreInstSet, DenseSet<Instruction *> CallInstSet, 
	bool &BaseEqual, bool &AssignedToClass, unsigned &StoreCount, DenseSet<Instruction *> InvokeInstSet) {

	for(auto C: CallInstSet) {
		CallInst *CI = dyn_cast<CallInst>(C);
		assert(CI);
		for(unsigned i = 0; i < CI->arg_size(); i++) {
			auto Arg = CI->getArgOperand(i);
			assert(Arg);

			SmallVector<const Value *, 4> Objects;
			getBases(F, Arg, Objects);
			if(isBaseAlloc(Objects, AllocInstSet)) {
				return true;
			}
		}
	}

	for(auto C: InvokeInstSet) {
		InvokeInst *CI = dyn_cast<InvokeInst>(C);
		assert(CI);
		for(unsigned i = 0; i < CI->arg_size(); i++) {
			auto Arg = CI->getArgOperand(i);
			assert(Arg);

			SmallVector<const Value *, 4> Objects;
			getBases(F, Arg, Objects);
			if(isBaseAlloc(Objects, AllocInstSet)) {
				return true;
			}
		}
	}
	
	for(auto S: StoreInstSet) {
		StoreInst *SI = dyn_cast<StoreInst>(S);
		assert(SI);
		assert(SI->getValueOperand());
		SmallVector<const Value *, 4> Objects;
		getBases(F, SI->getValueOperand(), Objects);
		if(isBaseAlloc(Objects, AllocInstSet)) {
			StoreCount++;
			assert(SI->getPointerOperand());
			SmallVector<const Value *, 4> Objects1;
			getBases(F, SI->getPointerOperand(), Objects1);
			if(Objects1.size() == 1) {
				if(isBaseAlloc(Objects1, AllocInstSet)) {
					BaseEqual = true;
				}

				for(auto O: Objects1) {
					assert(O);
					if(isClassType(const_cast<Value*>(O))) {
						AssignedToClass = true;
					}
					/*if(O->hasName() && O->getName().equals("this")) {
						AssignedToClass = true;
					}*/
				}
			}
		}
	}

	if(StoreCount > 0) {
		return true;
	}

	return false;
}

static bool isLibraryFunctionCall(Instruction *I) {
	TargetLibraryInfoImpl TLII;
  	TargetLibraryInfo TLI(TLII);
  	LibFunc Func;

	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(TLI.getLibFunc(*F, Func) || (F->hasName() && F->getName().equals("exit"))) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(TLI.getLibFunc(*F, Func) || (F->hasName() && F->getName().equals("exit"))) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(TLI.getLibFunc(*F, Func) || (F->hasName() && F->getName().equals("exit"))) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(TLI.getLibFunc(*F, Func) || (F->hasName() && F->getName().equals("exit"))) {
				return true;
			}
		}
	}
	return false;
}

static bool isWrapperFuncCall(Instruction *I, DenseSet<Function *> WrapperFn) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(WrapperFn.count(F)) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(WrapperFn.count(F)) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(WrapperFn.count(F)) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(WrapperFn.count(F)) {
				return true;
			}
		}
	}
	return false;
}

static bool ArgAsGlobalString(Instruction *C) {
	bool flag = false;
	if(CallInst *CI = dyn_cast<CallInst>(C)) {
		assert(CI);
		for(unsigned i = 0; i < CI->arg_size(); i++) {
			auto Arg = CI->getArgOperand(i);
			assert(Arg);
			if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Arg)) {
				assert(CE->getOperand(0));
				if(GlobalVariable *GV = dyn_cast<GlobalVariable>(CE->getOperand(0))) {
					assert(GV->getInitializer());
					if(auto GI = dyn_cast<ConstantDataArray>(GV->getInitializer())) {
						if(GI->isCString()) {
							flag = true;
							break;
						}
					}
				}
			}
		}
	}
	else if(InvokeInst *CI = dyn_cast<InvokeInst>(C)) {
		assert(CI);
		for(unsigned i = 0; i < CI->arg_size(); i++) {
			auto Arg = CI->getArgOperand(i);
			assert(Arg);
			if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Arg)) {
				assert(CE->getOperand(0));
				if(GlobalVariable *GV = dyn_cast<GlobalVariable>(CE->getOperand(0))) {
					assert(GV->getInitializer());
					if(auto GI = dyn_cast<ConstantDataArray>(GV->getInitializer())) {
						if(GI->isCString()) {
							flag = true;
							break;
						}
					}
				}
			}
		}
	}
	else {
		assert(0);
	}

	if(flag) {
		return true;
	}
	return false;
}

static bool callsFunctionsToSkip(Instruction *I) {
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->hasName() && (F->getName().equals("Perl_my_exit") || F->getName().equals("Perl_PerlIO_stderr") 
				|| F->getName().equals("xexit") || F->getName().equals("__clang_call_terminate") 
				|| F->getName().equals("__cxa_begin_catch") || F->getName().equals("Perl_PerlIO_fileno") 
				|| F->getName().equals("__errno_location") || F->getName().equals("__cxa_allocate_exception") 
				|| F->getName().equals("__cxa_end_catch") || F->getName().equals("__cxa_throw"))) {
				return true;
			}
		}
	}
	if(CallInst *CI = dyn_cast<CallInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->hasName() && (F->getName().equals("Perl_my_exit") || F->getName().equals("Perl_PerlIO_stderr") 
				|| F->getName().equals("xexit") || F->getName().equals("__clang_call_terminate") 
				|| F->getName().equals("__cxa_begin_catch") || F->getName().equals("Perl_PerlIO_fileno") 
				|| F->getName().equals("__errno_location") || F->getName().equals("__cxa_allocate_exception") 
				|| F->getName().equals("__cxa_end_catch") || F->getName().equals("__cxa_throw"))) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = CI->getCalledFunction();
		if(F) {
			if(F->hasName() && (F->getName().equals("Perl_my_exit") || F->getName().equals("Perl_PerlIO_stderr") 
				|| F->getName().equals("xexit") || F->getName().equals("__clang_call_terminate") 
				|| F->getName().equals("__cxa_begin_catch") || F->getName().equals("Perl_PerlIO_fileno") 
				|| F->getName().equals("__errno_location") || F->getName().equals("__cxa_allocate_exception") 
				|| F->getName().equals("__cxa_end_catch") || F->getName().equals("__cxa_throw"))) {
				return true;
			}
		}
	}
	if(InvokeInst *CI = dyn_cast<InvokeInst>(I)) {
		Function *F = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
		if(F) {
			if(F->hasName() && (F->getName().equals("Perl_my_exit") || F->getName().equals("Perl_PerlIO_stderr") 
				|| F->getName().equals("xexit") || F->getName().equals("__clang_call_terminate") 
				|| F->getName().equals("__cxa_begin_catch") || F->getName().equals("Perl_PerlIO_fileno") 
				|| F->getName().equals("__errno_location") || F->getName().equals("__cxa_allocate_exception") 
				|| F->getName().equals("__cxa_end_catch") || F->getName().equals("__cxa_throw"))) {
				return true;
			}
		}
	}
	return false;
}

static bool doesCallFunction(DenseSet<Function *> WrapperFn, 
	DenseSet<Instruction *> CallInstSet, DenseSet<Instruction *> InvokeInstSet) {

	for(auto C: CallInstSet) {
		if(isAllocationSite(C) || isLibraryFunctionCall(C) || isWrapperFuncCall(C, WrapperFn) 
			|| ArgAsGlobalString(C) || callsFunctionsToSkip(C)) {
			continue;
		}
		else {
			return true;
		}
	}

	for(auto C: InvokeInstSet) {
		if(isAllocationSite(C) || isLibraryFunctionCall(C) || isWrapperFuncCall(C, WrapperFn) 
			|| ArgAsGlobalString(C) || callsFunctionsToSkip(C)) {
			continue;
		}
		else {
			return true;
		}
	}
	return false;
}

static bool isWrapperFunc(Function *F, DenseSet<Function *> WrapperFn) {
	DenseSet<Instruction *> AllocInstSet;
	DenseSet<Instruction *> StoreInstSet; 
	DenseSet<Instruction *> CallInstSet; 
	DenseSet<Instruction *> RetInstSet;
	DenseSet<Instruction *> InvokeInstSet;

	isStandardAllocationAPI(F, AllocInstSet, StoreInstSet, CallInstSet, RetInstSet, InvokeInstSet);

    if(AllocInstSet.size() == 0) {
    	return false;
    }

    if(AllocInstSet.size() > 1) {
    	return false;
    }

    assert(AllocInstSet.size() == 1);

    if(doesCallFunction(WrapperFn, CallInstSet, InvokeInstSet)) {
    	return false;
    }

    bool AssignedToClass = false;
    bool BaseEqual = false;
    unsigned StoreCount = 0;
    if(isAllocCaptured(F, AllocInstSet, StoreInstSet, CallInstSet, BaseEqual, AssignedToClass, StoreCount, InvokeInstSet)) {
    	if(StoreCount == 1) {
    		if(BaseEqual) {
	    		errs() << "Warning1: Function " << F->getName() << " contains both store operands with base as memory allocation.\n";
	    		Warning1++;
	    	}
	    	if(AssignedToClass) {
	    		errs() << "Warning2: Function " << F->getName() << " contains memory allocation stored to class member.\n";
	    		Warning2++;
	    	}
    	}
    	return false;
    }

    if(!isReturnDerived(F, AllocInstSet, RetInstSet)) {
    	return false;
    }

    errs() << "Wrapper function: " << F->getName() << "\n";
    return true;
}

static void isWrapperAllocation(Function *F, DenseSet<Function *> WrapperFn, 
	DenseSet<Instruction *> &AllocInstSet, DenseSet<Instruction *> &StoreInstSet, 
	DenseSet<Instruction *> &CallInstSet, DenseSet<Instruction *> &RetInstSet, 
	DenseSet<Instruction *> &InvokeInstSet) {

	for (Function::iterator BB = F->begin(); BB != F->end(); ++BB) {
      	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
      		Instruction *Inst = &*I;
      		if(isa<StoreInst>(Inst)) {
	    		StoreInstSet.insert(Inst);
	    	}
	    	else if(CallInst *CI = dyn_cast<CallInst>(Inst)) {
	    		Function *CF = CI->getCalledFunction();
	    		Function *CF2 = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
	    		if(CF && WrapperFn.count(CF)) {
	    			AllocInstSet.insert(Inst);
	    		}
	    		else if(CF2 && WrapperFn.count(CF2)) {
			    	AllocInstSet.insert(Inst);
			    }
	    		else {
	    			CallInst *CI = dyn_cast<CallInst>(Inst);
		    		assert(CI);
		    		if(CI->getIntrinsicID() == Intrinsic::not_intrinsic) {
		    			CallInstSet.insert(Inst);
		    		}
	    		}
	    	}
	    	else if(InvokeInst *CI = dyn_cast<InvokeInst>(Inst)) {
	    		Function *CF = CI->getCalledFunction();
	    		Function *CF2 = dyn_cast<Function>(CI->getCalledValue()->stripPointerCasts());
	    		if(CF && WrapperFn.count(CF)) {
	    			AllocInstSet.insert(Inst);
	    		}
	    		else if(CF2 && WrapperFn.count(CF2)) {
			    	AllocInstSet.insert(Inst);
			    }
	    		else {
	    			InvokeInstSet.insert(Inst);
	    		}
	    	}
	    	else if(Inst->getNumOperands() > 0 && isa<ReturnInst>(Inst)) {
      			RetInstSet.insert(Inst);
      		}
      	}
    }
}

static bool isWrapperCallingWrapper(Function *F, DenseSet<Function *> WrapperFn) {
	DenseSet<Instruction *> AllocInstSet;
	DenseSet<Instruction *> StoreInstSet; 
	DenseSet<Instruction *> CallInstSet; 
	DenseSet<Instruction *> RetInstSet;
	DenseSet<Instruction *> InvokeInstSet;

	isWrapperAllocation(F, WrapperFn, AllocInstSet, StoreInstSet, CallInstSet, RetInstSet, InvokeInstSet);

	if(AllocInstSet.size() == 0) {
    	return false;
    }

    if(AllocInstSet.size() > 1) {
    	return false;
    }

    assert(AllocInstSet.size() == 1);

    if(doesCallFunction(WrapperFn, CallInstSet, InvokeInstSet)) {
    	return false;
    }

    bool AssignedToClass = false;
    bool BaseEqual = false;
    unsigned StoreCount = 0;
    if(isAllocCaptured(F, AllocInstSet, StoreInstSet, CallInstSet, BaseEqual, AssignedToClass, StoreCount, InvokeInstSet)) {
    	if(StoreCount == 1) {
    		if(BaseEqual) {
	    		errs() << "Warning1: Function " << F->getName() << " contains both store operands with base as memory allocation.\n";
	    		Warning1++;
	    	}
	    	if(AssignedToClass) {
	    		errs() << "Warning2: Function " << F->getName() << " contains memory allocation stored to class member.\n";
	    		Warning2++;
	    	}
    	}
    	return false;
    }

    if(!isReturnDerived(F, AllocInstSet, RetInstSet)) {
    	return false;
    }

	errs() << "Wrapper function calling another wrapper function: " << F->getName() << "\n";
    return true;
}

bool IdentifyWrappers::runOnModule(Module &M) {
	DenseSet<Function *> WrapperFn;

	for (Module::iterator F = M.begin(); F != M.end(); ++F) {
    	Function *Fn = dyn_cast<Function>(F);

    	if (Fn->isDeclaration()) {
      		continue;
    	}

    	if(isWrapperFunc(Fn, WrapperFn)) {
    		WrapperFn.insert(Fn);
    		WrapperCount1++;
    	}
    }

    if(WrapperFn.size() > 0) {
    	bool Changed = true;
    	while(Changed) {
    		DenseSet<Function *> TempWrapperFn;

    		for (Module::iterator F = M.begin(); F != M.end(); ++F) {
		    	Function *Fn = dyn_cast<Function>(F);

		    	if (Fn->isDeclaration()) {
		      		continue;
		    	}

		    	if(WrapperFn.count(Fn)) {
		    		continue;
		    	}

		    	if(isWrapperCallingWrapper(Fn, WrapperFn)) {
	    			TempWrapperFn.insert(Fn);
	    		}
		    }

    		if(TempWrapperFn.size() == 0) {
    			Changed = false;
    		}
    		else {
    			WrapperFn.insert(TempWrapperFn.begin(), TempWrapperFn.end());
    		} 
    	}
    }

    WrapperCount = WrapperFn.size();
    WrapperCount2 = WrapperCount - WrapperCount1;
    return true;
}

char IdentifyWrappers::ID = 0;

static RegisterPass<IdentifyWrappers> X(IW_NAME,
                             "Identify wrapper functions",
                             false, // Is CFG Only?
                             false); // Is Analysis?
