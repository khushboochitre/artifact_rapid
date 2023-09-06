#include "VPlan.h"
#include "llvm/Pass.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Scalar/DeadStoreElimination.h"
#include "llvm/Transforms/Vectorize/CustomVectorization.h"
#include "llvm/Transforms/Vectorize/LoopVectorizationLegality.h"

#include <stdio.h>  
#include <unistd.h>
#include <stdlib.h>
#include <sstream>
#include <fstream>

namespace llvm {
  static cl::opt<bool> DisableAdditionalVectorization(
  	"disable-additional-vectorize", cl::init(false), cl::Hidden);

  static cl::opt<bool> ExecuteOptimizedPath(
  	"execute-optimized-path", cl::init(false), cl::Hidden);

  static cl::opt<bool> ExecuteUnoptimizedPath(
  	"execute-unoptimized-path", cl::init(false), cl::Hidden);

  static cl::opt<bool> NonAffineLoops(
  	"non-affine-loops", cl::init(false), cl::Hidden);

  static cl::opt<string> AllowProfiling(
  	"allow-profiling");

  static cl::opt<string> GetTimeStats(
  	"get-time-stats");

  static cl::opt<string> GetTimeStatsFunc(
  	"get-time-stats-func");

  static cl::opt<string> AllowBenLoops(
  	"allow-ben-loops");

  static cl::opt<string> InstDynChecks(
  	"instrument-dynamic-checks");

  static cl::opt<uint32_t> BenLoopThreshold(
  	"ben-loop-threshold", cl::init(0), cl::Hidden);

  static cl::opt<bool> ComputeAlignedAllocOverhead(
  	"compute-aligned-alloc-overhead", cl::init(false), cl::Hidden);

  static cl::opt<bool> EnableWrapperAnnotation(
	  	"enable-wrapper-annotation", cl::init(false), cl::Hidden);
}

using namespace llvm;
using namespace std;

#define DEBUG_TYPE AV_NAME
#define AV_NAME "additional-vectorize"

STATISTIC(InnermostLoop, "Number of innermost loops");
STATISTIC(ClonedLoop, "Number of cloned loops");
STATISTIC(BenLoop, "Number of loops benefited");
//STATISTIC(DeletedLoop, "Number of deleted loops");
STATISTIC(NewVectLoop, "Number of newly vectorized loops");
//STATISTIC(OtherOptLoop, "Number of loops benefited by other optimizations and not vectorizable");
//STATISTIC(OtherOptVectLoops, "Number of loops benefited by other optimizations and vectorizable/not vectorizable");
STATISTIC(UnsafeBases, "Number of loops deleted due to unsafe bases");
STATISTIC(NonAffine, "Number of benefited loops with missing induction variable or bounds");
STATISTIC(ConvertedStackAllocations, "Number of stack allocations converted");

namespace {

	struct AdditionalVectorization : public FunctionPass {
		static char ID;

		explicit AdditionalVectorization() : FunctionPass(ID) {
			initializeAdditionalVectorizationPass(*PassRegistry::getPassRegistry());
		}

		bool runOnFunction(Function &F) override {
			auto *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
			auto *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
			auto *AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
			auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
			auto *MD = &getAnalysis<MemoryDependenceWrapperPass>().getMemDep();
			auto *TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
			auto *TLIP = getAnalysisIfAvailable<TargetLibraryInfoWrapperPass>();
    	auto *TLI = TLIP ? &TLIP->getTLI(F) : nullptr;
			auto *AC = &getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
			auto *ORE = &getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();
			auto &DB = getAnalysis<DemandedBitsWrapperPass>().getDemandedBits();
			auto *LAA = &getAnalysis<LoopAccessLegacyAnalysis>();

			if (LI->empty() && !doesExternalCall(F)) {
				return false;
			}

			if(ComputeAlignedAllocOverhead) {
				unsigned ConvertedStackAllocationsCount = 0;
				convertAllocaToMalloc(F, TLI, ConvertedStackAllocationsCount);
				ConvertedStackAllocations += ConvertedStackAllocationsCount;
				return false;
			}
			
			if(llvm::DisableAdditionalVectorization) {
				return false;
			}

			//Instrument write function before return/exit in main.
			if(F.getName().equals("main")) {
				//setGlobalAddressVariable(&*(F.getEntryBlock().getFirstInsertionPt()));
				if(!AllowProfiling.empty()) {
					instrumentWriteFunction(&F, AllowProfiling);
				}
				if(!GetTimeStats.empty()) {
					instrumentTimeWriteFunction(&F, GetTimeStats);
				}
				if(!GetTimeStatsFunc.empty()) {
					instrumentFuncTimeWriteFunction(&F, GetTimeStatsFunc);
				}
				if(!InstDynChecks.empty()) {
					//instrumentWriteSameASIDFunction(&F, InstDynChecks);
					instrumentWriteLoopDynamicChecksLogFunction(&F, InstDynChecks);
					//instrumentTimeWriteFunction(&F, InstDynChecks);
				}
			}

			//Create file to assign unique IDs to functions.
			if(!AllowProfiling.empty() || !GetTimeStats.empty() || !GetTimeStatsFunc.empty() || !InstDynChecks.empty()) {
				string filename = "";
				if(!AllowProfiling.empty()) {
					filename = AllowProfiling;
				}
				else if(!GetTimeStats.empty()) {
					filename = GetTimeStats;
				}
				else if(!InstDynChecks.empty()) {
					filename = InstDynChecks;
				}
				else {
					assert(!GetTimeStatsFunc.empty());
					filename = GetTimeStatsFunc;
				}
				assert(filename != "");
				assignFunctionID(F.getParent(), filename);
			}
			
			vector<Loop *> Loops;
			collectInnermostLoops(LI, Loops);

			//Create file to assign unique IDs to innermost loops.
			if(!AllowProfiling.empty() || !GetTimeStats.empty() || !InstDynChecks.empty()) {
				string filename = "";
				if(!AllowProfiling.empty()) {
					filename = AllowProfiling;
				}
				else if(!InstDynChecks.empty()) {
					filename = InstDynChecks;
				}
				else {
					assert(!GetTimeStats.empty());
					filename = GetTimeStats;
				}
				assert(filename != "");
				assignLoopID(&F, FuncID, Loops.size(), filename);
			}

			FuncID++;

			//Read profile information.
			DenseMap<unsigned, unsigned long long> UnoptimizedTimeStatsMap;
			DenseMap<unsigned, unsigned long long> OptimizedTimeStatsMap;
			if(!AllowBenLoops.empty() && Loops.size() > 0) {
	    		createTimeStatsMap(AllowBenLoops, UnoptimizedTimeStatsMap, OptimizedTimeStatsMap);
	    	}
			
			DenseMap<Loop *, unsigned> LoopIDMap;
			DenseMap<Loop *, pair<Loop *, BasicBlock *>> LoopBlockMap;
			DenseMap<Loop *, DenseSet<Instruction *>> LoopAllLoadsMap;
			DenseMap<Loop *, DenseSet<Instruction *>> LoopAllStoresMap;
			DenseMap<Loop *, DenseSet<pair<Value *, Value *>>> LoopLoadBasesMap;
			DenseMap<Loop *, DenseSet<pair<Value *, Value *>>> LoopStoreBasesMap;
			DenseMap<Loop *, DenseMap<Instruction *, DenseSet<MDNode *>>> LoopLoadDomainMap;
			DenseMap<Loop *, DenseMap<Instruction *, DenseSet<MDNode *>>> LoopConflictingLoadDomainMap;
			DenseMap<Loop *, DenseMap<Instruction *, DenseSet<MDNode *>>> LoopStoreDomainMap;
			DenseMap<Loop *, DenseSet<Value *>> LoopSafeBases;
			DenseMap<Loop *, DenseSet<pair<Instruction *, Instruction *>>> LoopAllInstSetMap;
			unsigned ID = 0;
			for(auto L : Loops) {
				assert(L->empty());
				InnermostLoop++;
				LoopIDMap[L] = ID;
				ID++;

				simplifyLoop(L, DT, LI, SE, AC, nullptr, false);
				formLCSSARecursively(*L, *DT, LI, SE);
				Loop *NewLoop = nullptr;
    			BasicBlock *RuntimeChecksBlock = nullptr;

    			DenseSet<Instruction *> AllLoads;
				DenseSet<Instruction *> AllStores;
				DenseSet<pair<Value *, Value *>> LoadBases;
				DenseSet<pair<Value *, Value *>> StoreBases;
				DenseMap<Instruction *, DenseSet<MDNode *>> LoadDomainMap;
				DenseMap<Instruction *, DenseSet<MDNode *>> ConflictingLoadDomainMap;
				DenseMap<Instruction *, DenseSet<MDNode *>> StoreDomainMap;
				DenseSet<Value *> SafeBases;
				DenseSet<pair<Instruction *, Instruction *>> InstSetMap;
    			createCloneToGetChecks(L, LI, DT, AA, &NewLoop, 
    				&RuntimeChecksBlock, AllLoads, AllStores, 
    				LoadBases, StoreBases, LoadDomainMap, 
    				ConflictingLoadDomainMap, StoreDomainMap, 
    				ExecuteOptimizedPath, ExecuteUnoptimizedPath, SafeBases, InstSetMap);
    			
				if(NewLoop) {
					assert(RuntimeChecksBlock);
					assert(AllLoads.size() > 0 && AllStores.size() > 0);
					LoopBlockMap[L] = make_pair(NewLoop, RuntimeChecksBlock);
					DT->recalculate(F);

					LoopAllLoadsMap[L] = AllLoads;
					LoopAllStoresMap[L] = AllStores;
					LoopLoadBasesMap[L] = LoadBases;
					LoopStoreBasesMap[L] = StoreBases;
					LoopLoadDomainMap[L] = LoadDomainMap;
					LoopConflictingLoadDomainMap[L] = ConflictingLoadDomainMap;
					LoopStoreDomainMap[L] = StoreDomainMap;
					LoopSafeBases[L] = SafeBases;
					LoopAllInstSetMap[L] = InstSetMap;
					ClonedLoop++;
				}
			}
			
			bool Changed = false;
			for(auto Pair : LoopBlockMap) {
				auto L = Pair.first;
				auto NewLoop = Pair.second.first;

				simplifyLoop(NewLoop, DT, LI, 
					SE, AC, nullptr, false);
				
				Loop *Parent = NewLoop;
				Loop *Prev = Parent;
				while(Parent) {
				    Prev = Parent;
				    Parent = Parent->getParentLoop();
				}
				assert(Prev);
				Changed |= formLCSSARecursively(*Prev, *DT, LI, SE);
				
				LICMPass licm;
				MemorySSA *MSSA;
				Changed |= licm.callLICM(NewLoop, AA, LI, DT, 
						TLI, TTI, SE, MSSA, ORE, false);
			}
			
			if(LoopBlockMap.size() > 0) {
				GVN gvn;
		    	Changed |= gvn.runImpl(F, *AC, *DT, *TLI, 
		    				*AA, MD, LI, ORE);
		    	
		    	DSEPass dse;
		    	Changed |= dse.callDSE(F, AA, MD, DT, TLI);
			}

			removeCustomNoAliasIntrinsic(&F);
			AA->setDisableCustomIntrinsicChecks(true);
			
			for(auto Pair : LoopBlockMap) {
				auto L = Pair.first;
				auto NewLoop = Pair.second.first;
				auto RuntimeChecksBlock = Pair.second.second;

				//Is original loop vectorizable?
				LoopVectorizePass LVPassO;
				LoopVectorizationRequirements RO(*ORE);
				LoopVectorizeHints HO(L, false, *ORE);
				PredicatedScalarEvolution PSEO(*SE, *L);
				std::function<const LoopAccessInfo &(Loop &)> GetLAAO =
        			[&](Loop &L) -> const LoopAccessInfo & { return LAA->getInfo(&L); };
				bool isVectorizableBefore = LVPassO.isVectorizationPossible(L, PSEO, DT, TTI, TLI, AA, &F, &GetLAAO,
      					LI, ORE, &RO, &HO, &DB, AC);

				//Is cloned loop vectorizable?
	    		LoopVectorizePass LVPassN;
				LoopVectorizationRequirements RN(*ORE);
				LoopVectorizeHints HN(NewLoop, false, *ORE);
				PredicatedScalarEvolution PSEN(*SE, *NewLoop);
				std::function<const LoopAccessInfo &(Loop &)> GetLAAN =
        			[&](Loop &L) -> const LoopAccessInfo & { return LAA->getInfo(NewLoop); };
				bool isVectorizableAfter = LVPassN.isVectorizationPossible(NewLoop, PSEN, DT, TTI, TLI, AA, &F, &GetLAAN,
      					LI, ORE, &RN, &HN, &DB, AC);

				DenseMap<Instruction *, unsigned> OldInstMap;
				DenseMap<unsigned, Instruction *> NewInstMap;
				mapInstructions(L, NewLoop, OldInstMap, NewInstMap);
	    		
	    		DenseSet<Instruction *> LoadSet, StoreSet;
	    		getLoadStoreSet(L, NewLoop, LoadSet, StoreSet);

	    		//Is benefited by other optimizations?
	    		bool otherOpt = false;
	    		if(LoadSet.size() > 0 || StoreSet.size() > 0) {
	    			otherOpt = true;
	    		}

	    		//Is benefited on the basis of profiler.
	    		bool isBenefited = true;
				if(!AllowBenLoops.empty()) {
	    			unsigned LoopID = UINT_MAX;
	    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], AllowBenLoops);
	    			assert(LoopID != UINT_MAX);
	    			if(!isLoopBenefited(LoopID, UnoptimizedTimeStatsMap, OptimizedTimeStatsMap, BenLoopThreshold)) {
	    				isBenefited = false;
	    			}
	    		}

	    		simplifyLoop(L, DT, LI, 
					SE, AC, nullptr, false);
	    		bool IndStepFound = isIndStepFound(L, SE);
	    		if(NonAffineLoops) {
    				if(IndStepFound) {
    					isBenefited = false;
    				}
    			}

	    		if((!isVectorizableBefore && !isVectorizableAfter && !otherOpt) /*||
	    			(isVectorizableBefore && isVectorizableAfter && !otherOpt)*/) {
	    			//Case1: Loop is not vectorizable before, after and not benefited by any of the loop.
	    			isBenefited = false;
	    		}

				DenseSet<pair<Value *, Value *>> FinalBases;
				if(!isVectorizableBefore && isVectorizableAfter) {
					DenseSet<pair<Instruction *, Instruction *>> InstSet = LoopAllInstSetMap[L];
					getBasesLVBenefited(L, InstSet, FinalBases, LI, DT);

					if(FinalBases.size() == 0) {
						UnsafeBases++;
					}
				}
				else if(otherOpt) {
					DenseSet<pair<Instruction *, Instruction *>> InstSet = LoopAllInstSetMap[L];
					DenseSet<Instruction *> AllLoads = LoopAllLoadsMap[L];
					DenseSet<Instruction *> AllStores = LoopAllStoresMap[L];

					if(StoreSet.size() > 0) {
						for(auto St: StoreSet) {
							getBasesStoreBenefited(L, St, InstSet, FinalBases, LI, DT);
						}
					}
					else if(LoadSet.size() == AllLoads.size()) {
						for(auto Ld: LoadSet) {
							getBasesLoadBenefited(L, Ld, InstSet, FinalBases, LI, DT);
						}
					}
					else if(LoadSet.size() < AllLoads.size()) {
						DenseSet<Instruction *> ConflictingLoads;
						DenseSet<pair<Instruction *, Instruction *>> InstSet1;
						DenseSet<pair<Value *, Value *>> LoadBases1;
						bool isConflictingLoadBen = false;
						DenseSet<Value *> SafeBases;
						DenseSet<Instruction *> ConflictingStores;
						for(auto Load : LoadSet) {
							LoadDep(L, Load, AllLoads, AllStores, ConflictingLoads, 
									InstSet1, LoadBases1, LI, DT, AA, SafeBases, ConflictingStores);
							if(ConflictingLoads.count(Load)) {
								isConflictingLoadBen = true;
							}
						}

						if(isConflictingLoadBen) {
							for(auto Ld: LoadSet) {
								getBasesLoadBenefited(L, Ld, InstSet, FinalBases, LI, DT);
							}
							for(auto St: ConflictingStores) {
								getBasesStoreBenefited(L, St, InstSet, FinalBases, LI, DT);
							}
						}
						else {
							for(auto Ld: LoadSet) {
								getBasesLoadBenefited(L, Ld, InstSet, FinalBases, LI, DT);
							}
						}
					}

					if(FinalBases.size() == 0) {
						UnsafeBases++;
					}
				}

				if(FinalBases.size() == 0) {
					isBenefited = false;
				}

	    		/*if(!isVectorizableBefore && isVectorizableAfter) {
	    			if(LoopLoadBasesMap[L].size() > 0) {
	    				if(isBenefited && !areBasePairsSafe(LoopSafeBases[L], LoopLoadBasesMap[L])) {
	    					isBenefited = false;
	    					UnsafeBases++;
	    				}
	    				if(isBenefited && LoopStoreBasesMap[L].size() > 0 
	    					&& !areBasePairsSafe(LoopSafeBases[L], LoopStoreBasesMap[L])) {
	    					isBenefited = false;
	    					UnsafeBases++;
	    				}
	    			}
	    			else {
	    				assert(LoopStoreBasesMap[L].size() == 0);
	    			}
	    		}*/

	    		bool basesNotFound = false;
	    		bool found = false;
	    		if(!isBenefited) {
	    			//Loop not benefited based on profiler or,
	    			//Loop not benefited by any on the optimization or,
	    			//Loop is vectorizable before and after without the optimization getting benefited.
	    			simplifyLoop(NewLoop, DT, LI, SE, AC, nullptr, false);
	    			formLCSSARecursively(*NewLoop, *DT, LI, SE);
	    			removeDeadLoop(NewLoop);
	    			replaceCondWithTrue(L, RuntimeChecksBlock);
	    			deleteDeadLoop(NewLoop, DT, SE, LI);
	    			//DeletedLoop++;
	    		}
	    		else {
	    			DenseSet<Instruction *> AllLoads;
					DenseSet<Instruction *> AllStores;
					DenseSet<pair<Value *, Value *>> LoadBases;
					DenseSet<pair<Value *, Value *>> StoreBases;
					DenseMap<Instruction *, DenseSet<MDNode *>> LoadDomainMap;
					DenseMap<Instruction *, DenseSet<MDNode *>> ConflictingLoadDomainMap;
					DenseMap<Instruction *, DenseSet<MDNode *>> StoreDomainMap;

					AllLoads = LoopAllLoadsMap[L];
					AllStores = LoopAllStoresMap[L];
					LoadBases = LoopLoadBasesMap[L];
					StoreBases = LoopStoreBasesMap[L];
					LoadDomainMap = LoopLoadDomainMap[L];
					ConflictingLoadDomainMap = LoopConflictingLoadDomainMap[L];
					StoreDomainMap = LoopStoreDomainMap[L];

	    			if(!isVectorizableBefore && isVectorizableAfter) {
	    				found = true;
	    				//Case2: The loop is vectorizable after attaching metadata.
	    				/*DenseSet<pair<Value *, Value *>> FinalBases;
						unionLoadStoreBases(LoadBases, StoreBases, FinalBases);*/

						if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
							DenseSet<pair<Value *, Value *>> AddedBases1;
							emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1, InstDynChecks, TLI);
							/*DenseSet<pair<Value *, Value *>> AddedBases1;
							emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases, false, AddedBases1);
							emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, StoreBases, false, AddedBases1);*/
						}

						DenseSet<pair<Value *, Value *>> AddedBases2;
						//addCustomNoAliasIntrinsic(&F, NewLoop, LoadBases, AddedBases2);
						addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
						/*if(otherOpt) {
							OtherVectLoop++;
						}*/
						NewVectLoop++;
	    			}
					else if(otherOpt) {
	    				if(!isVectorizableBefore && !isVectorizableAfter && otherOpt) {
	    					//OtherOptLoop++;
	    				}
	    				/*if(isVectorizableBefore && isVectorizableAfter && otherOpt) {
	    					EnhancedVectLoops++;
	    				}*/
	    				//Case 3: Loop is not vectorizable but benefited by other optimizations.
	    				//Case 4: Loop is vectorizable before.

		    			if(StoreSet.size() > 0) {
		    				found = true;
		    				//Case 1: Some stores got benefitted.
		    				for(auto Pair : StoreDomainMap) {
		    					if(!StoreSet.count(Pair.first)) {
		    						removeDomain(NewLoop, Pair.second);
		    					}
		    				}
		    				/*DenseSet<Value *> SafeBases;
		    				if(StoreSet.size() != AllStores.size()) {
		    					DenseSet<pair<Instruction *, Instruction *>> InstSet;
		    					StoreBases.clear();
		    					for(auto Store : StoreSet) {
									StoreDep(L, Store, AllStores, InstSet, StoreBases, LI, DT, AA, SafeBases);
								}
		    				}
		    				
							assert(LoadBases.size() > 0);
							DenseSet<pair<Value *, Value *>> FinalBases;
							unionLoadStoreBases(LoadBases, StoreBases, FinalBases);*/
		    				assert(FinalBases.size() > 0);
							if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
								//emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases, false, AddedBases1);
								DenseSet<pair<Value *, Value *>> AddedBases1;
								emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1, InstDynChecks, TLI);
							}

							DenseSet<pair<Value *, Value *>> AddedBases2;
							addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);

							/*if(StoreBases.size() > 0) {
								if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
									emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, StoreBases, false, AddedBases1);
								}
								addCustomNoAliasIntrinsic(&F, NewLoop, StoreBases, AddedBases2);
							}*/
							//OtherOptVectLoops++;
		    			}
		    			else if(LoadSet.size() == AllLoads.size()) {
		    				found = true;
		    				assert(StoreSet.size() == 0);
		    				//Case 2: All loads got benefitted.
		    				for(auto Pair : StoreDomainMap) {
		    					removeDomain(NewLoop, Pair.second);
		    				}

		    				/*assert(LoadBases.size() > 0);
		    				DenseSet<pair<Value *, Value *>> FinalBases;
		    				StoreBases.clear();
							unionLoadStoreBases(LoadBases, StoreBases, FinalBases);*/

		    				DenseSet<pair<Value *, Value *>> AddedBases1;
		    				if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
		    					emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1, InstDynChecks, TLI);
		    					//emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases, false, AddedBases1);
		    				}

		    				DenseSet<pair<Value *, Value *>> AddedBases2;
		    				addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
		    				//addCustomNoAliasIntrinsic(&F, NewLoop, LoadBases, AddedBases2);
		    				//OtherOptVectLoops++;
		    			}
		    			else if(LoadSet.size() < AllLoads.size()) {
		    				found = true;
		    				assert(StoreSet.size() == 0);
		    				//Case 3: Some loads got benefitted.
		    				for(auto Pair : StoreDomainMap) {
		    					removeDomain(NewLoop, Pair.second);
		    				}

		    				DenseSet<Instruction *> ConflictingLoads;
							DenseSet<pair<Instruction *, Instruction *>> InstSet;
							DenseSet<pair<Value *, Value *>> LoadBases1;
							bool isConflictingLoadBen = false;
							DenseSet<Value *> SafeBases;
							DenseSet<Instruction *> ConflictingStores;
							for(auto Load : LoadSet) {
								LoadDep(L, Load, AllLoads, AllStores, ConflictingLoads, 
									InstSet, LoadBases1, LI, DT, AA, SafeBases, ConflictingStores);
								if(ConflictingLoads.count(Load)) {
									isConflictingLoadBen = true;
								}
							}

							if(!isConflictingLoadBen) {
								for(auto Pair : ConflictingLoadDomainMap) {
			    					if(NewInstMap.count(OldInstMap[Pair.first])) {
			    						removeConflictingLoadDomain(NewInstMap[OldInstMap[Pair.first]],
			    							Pair.second);
			    					}
			    				}

			    				for(auto Pair : LoadDomainMap) {
			    					if(!LoadSet.count(Pair.first)) {
			    						removeDomain(NewLoop, Pair.second);
			    					}
			    				}

								/*DenseSet<pair<Value *, Value *>> AddedBases1;
								if(LoadBases1.size() == 0) {
									basesNotFound = true;
									simplifyLoop(NewLoop, DT, LI, SE, AC, nullptr, false);
					    			formLCSSARecursively(*NewLoop, *DT, LI, SE);
					    			removeDeadLoop(NewLoop);
					    			replaceCondWithTrue(L, RuntimeChecksBlock);
					    			deleteDeadLoop(NewLoop, DT, SE, LI);
					    			//DeletedLoop++;
								}
								else {
									DenseSet<pair<Value *, Value *>> FinalBases;
		    						StoreBases.clear();
									unionLoadStoreBases(LoadBases1, StoreBases, FinalBases);

									assert(LoadBases1.size() > 0);
									if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
										emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1);
										//emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases1, false, AddedBases1);
									}

									DenseSet<pair<Value *, Value *>> AddedBases2;
									addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
									//addCustomNoAliasIntrinsic(&F, NewLoop, LoadBases1, AddedBases2);
								}*/
								DenseSet<pair<Value *, Value *>> AddedBases1;
			    				if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
			    					emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1, InstDynChecks, TLI);
			    					//emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases, false, AddedBases1);
			    				}

			    				DenseSet<pair<Value *, Value *>> AddedBases2;
			    				addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
							}
							else {
								for(auto Pair : LoadDomainMap) {
			    					if(!LoadSet.count(Pair.first)) {
			    						removeDomain(NewLoop, Pair.second);
			    					}
			    				}

			    				for(auto Pair : StoreDomainMap) {
			    					if(!ConflictingStores.count(Pair.first)) {
			    						removeDomain(NewLoop, Pair.second);
			    					}
			    				}

			    				for(auto Pair : ConflictingLoadDomainMap) {
			    					if(!LoadSet.count(Pair.first) && NewInstMap.count(OldInstMap[Pair.first])) {
			    						removeConflictingLoadDomain(NewInstMap[OldInstMap[Pair.first]],
			    							Pair.second);
			    					}
			    				}
								//Conflicting load got benefited.
								/*assert(LoadBases.size() > 0);
								DenseSet<pair<Value *, Value *>> FinalBases;
		    					StoreBases.clear();
								unionLoadStoreBases(LoadBases, StoreBases, FinalBases);*/

								DenseSet<pair<Value *, Value *>> AddedBases1;
			    				if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
			    					emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, FinalBases, false, AddedBases1, InstDynChecks, TLI);
			    					//emitRuntimeChecks(L, NewLoop, RuntimeChecksBlock, LoadBases, false, AddedBases1);
			    				}

			    				DenseSet<pair<Value *, Value *>> AddedBases2;
			    				addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
			    				//addCustomNoAliasIntrinsic(&F, NewLoop, LoadBases, AddedBases2);
							}
							if(!basesNotFound) {
								//OtherOptVectLoops++;
							}
		    			}
	    			}

	    			/*if(!InstDynChecks.empty()) {
	    				unsigned LoopID = UINT_MAX;
			    		LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], InstDynChecks);
			    		assert(LoopID != UINT_MAX);
	    				instrumentTimeFunction2(&F, L, NewLoop, LoopID);
	    			}*/

	    			if(!found) {
	    				simplifyLoop(NewLoop, DT, LI, SE, AC, nullptr, false);
		    			formLCSSARecursively(*NewLoop, *DT, LI, SE);
		    			removeDeadLoop(NewLoop);
		    			replaceCondWithTrue(L, RuntimeChecksBlock);
		    			deleteDeadLoop(NewLoop, DT, SE, LI);
		    			//DeletedLoop++;
	    			}
	    			
	    			if(found && !basesNotFound) {
	    				if(!IndStepFound) {
	    					NonAffine++;
	    				}

	    				F.addFnAttr("function_benefitted");
	    				BenLoop++;

	    				if(ExecuteOptimizedPath) {
	    					for(auto Pair : LoadDomainMap) {
		    					removeDomain(NewLoop, Pair.second);
		    				}

		    				for(auto Pair : StoreDomainMap) {
		    					removeDomain(NewLoop, Pair.second);
		    				}

		    				for(auto Pair : ConflictingLoadDomainMap) {
			    				if(NewInstMap.count(OldInstMap[Pair.first])) {
			    					removeConflictingLoadDomain(NewInstMap[OldInstMap[Pair.first]],
			    							Pair.second);
			    				}
			    			}
	    				}

	    				//Profiling logic.
	    				if(!AllowProfiling.empty()) {
			    			unsigned LoopID = UINT_MAX;
			    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], AllowProfiling);
			    			assert(LoopID != UINT_MAX);
			    			instrumentDebugFunction(L, 0, LoopID);
			    			instrumentDebugFunction(NewLoop, 1, LoopID);
			    		}

			    		if(!GetTimeStats.empty()) {
			    			unsigned LoopID = UINT_MAX;
			    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], GetTimeStats);
			    			assert(LoopID != UINT_MAX);
			    			instrumentTimeFunction(&F, RuntimeChecksBlock, L->getExitBlock(), LoopID);
			    		}

			    		if(!GetTimeStatsFunc.empty()) {
			    			unsigned FID = UINT_MAX;
			    			FID = getFuncID(&F, FuncID - 1, GetTimeStatsFunc);
			    			assert(FID != UINT_MAX);
			    			instrumentFuncTimeFunction(&F, FID);
			    		}

			    		if(isVectorizableBefore && !isVectorizableAfter /*&& NewLoop->getLoopLatch()*/) {
			    			DenseSet<PHINode *> PHINodeSet;
				    		for (BasicBlock *BB : NewLoop->blocks()) {
							  	for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
						      		Instruction *II = &*I;
						      		if(PHINode *PN = dyn_cast<PHINode>(II)) {
						      			PHINodeSet.insert(PN);
						      			/*assert(NewLoop->getLoopLatch());
						      			if(PN->getBasicBlockIndex(NewLoop->getLoopLatch()) > -1) {
						      				auto *Previous = dyn_cast<Instruction>(PN->getIncomingValueForBlock(NewLoop->getLoopLatch()));
							      			if(PHINode *PPN = dyn_cast<PHINode>(Previous)) {
							      				PHINodeSet.insert(PN);
							      			}
						      			}*/
						      		}
						      	}
							}

							for(auto PHI: PHINodeSet) {
								DT->recalculate(F);
								fixIRForVectorization(&F, PHI, LI, DT, AA);
							}
			    		}
	    			}
	    		}
	    		//assert(BenLoop == NewVectLoop + OtherOptVectLoops);
			}

			removeInstrumentedFunctions(&F, ExecuteOptimizedPath, ExecuteUnoptimizedPath);

			DominatorTree DT1(F);
			PostDominatorTree PDT(F);
			LoopInfo LI1(DT1);

			splitIntrinsic(F, LI1, DT1, PDT);

			mergeIntrinsics(F);
			AA->setDisableCustomIntrinsicChecks(false);

			if(!InstDynChecks.empty()) {
				unsigned ConvertedStackAllocationsCount = 0;
				convertAllocaToMalloc(F, TLI, ConvertedStackAllocationsCount);
				ConvertedStackAllocations += ConvertedStackAllocationsCount;

				unsigned FID = UINT_MAX;
			  FID = getFuncID(&F, FuncID - 1, InstDynChecks);
			  assert(FID != UINT_MAX);
			  assignASID(&F, FID, InstDynChecks);
			  DenseMap<Instruction *, unsigned> AllocationIDMap;
				mapAllocationSitesToID(&F, FID, AllocationIDMap, InstDynChecks);
				instrumentSetASIDFunction(&F, AllocationIDMap, TLI, EnableWrapperAnnotation);
				storeStackID(InstDynChecks, AllocationIDMap);
				storeReallocID(InstDynChecks, AllocationIDMap);
			}
			if (verifyFunction(F, &errs())) {
			    errs() << "Not able to verify!\n";
			    errs() << F << "\n";
			    assert(0);
			}
			return false;
		}

		void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.addRequired<LoopInfoWrapperPass>();
			AU.addRequired<AAResultsWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<DemandedBitsWrapperPass>();
			AU.addRequired<LoopAccessLegacyAnalysis>();
			AU.addRequired<DominatorTreeWrapperPass>();
			AU.addRequired<ScalarEvolutionWrapperPass>();
			AU.addRequired<MemoryDependenceWrapperPass>();
			AU.addRequired<LoopAccessLegacyAnalysis>();
			AU.addRequired<TargetTransformInfoWrapperPass>();
			AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
		}
	}; // end of struct AdditionalVectorization.
} // end of anonymous namespace.

using namespace llvm;

char AdditionalVectorization::ID = 0;

static const char av_name[] = "Additional Vectorization";

INITIALIZE_PASS_BEGIN(AdditionalVectorization, AV_NAME, 
	av_name, false /* Only looks at CFG */, false /* Analysis Pass */)

//INITIALIZE_PASS_DEPENDENCY(PromoteLegacyPass)

INITIALIZE_PASS_END(AdditionalVectorization, AV_NAME, 
	av_name, false /* Only looks at CFG */, false /* Analysis Pass */)

namespace llvm {
	Pass *createAdditionalVectorizationPass() {
		return new AdditionalVectorization(); 
	}
}
