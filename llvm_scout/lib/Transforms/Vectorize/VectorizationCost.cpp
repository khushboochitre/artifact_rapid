#include "llvm/Pass.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;
using namespace std;

#define DEBUG_TYPE "vectCost"

STATISTIC(MetadataLoops,  "Number of loops for which metadata was added");
STATISTIC(ParentLoopCount,  "Number of parent loops that can be additionally vectorized");
STATISTIC(VectLoop,  "Number of loops vectorized");
STATISTIC(LoopNotBen,  "Number of loops not benefitted");

static cl::opt<bool> AllowLVOnly("allow-lv-only", cl::init(false));
static cl::opt<string> FunctionName("cost-func-name");
static cl::opt<string> DebugModeFile("enable-debug-mode");

namespace {
	static bool isFuncDefAdded = false;
	static bool isWriteDefAdded = false;
	static unsigned LoopID = 0;
	static Function *updateLoopCounterFunc;
	static Function *writeLoopCounterFunc;
	static GlobalVariable* TGV;
	static PointerType *CharStarType;
	static IntegerType *IntType;
	static DenseMap<Function *, GlobalVariable *> FuncGVMap;

	struct VectorizationCost : public FunctionPass {
		static char ID;
		VectorizationCost() : FunctionPass(ID) {}

		bool runOnFunction(Function &F) {
			if(!FunctionName.empty() && !F.getName().equals(FunctionName)) {
				return true;
			}

			if (verifyFunction(F, &errs())) {
        		errs() << "Not able to verify!\n";
        		errs() << F << "\n";
        		assert(0);
      		}

			return true;
		}

		void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.addRequired<LoopInfoWrapperPass>();
			AU.addRequired<DominatorTreeWrapperPass>();
		}
	};
}

char VectorizationCost::ID = 0;
static RegisterPass <VectorizationCost> 
	X("vectorization-cost", "Computes vectorization cost",
    false, false);