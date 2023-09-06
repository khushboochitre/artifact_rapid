#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Scalar/DeadStoreElimination.h"
#include "llvm/Transforms/Vectorize/CustomVectorization.h"

namespace llvm {
	static cl::opt<string> CheckInfoFile(
  		"use-check-info");

	static cl::opt<bool> LoopTakenStats(
	  	"region-loop-taken", cl::init(false), cl::Hidden);

	static cl::opt<bool> ExecuteOptimizedPath(
	  	"region-execute-optimized-path", cl::init(false), cl::Hidden);

	static cl::opt<bool> ExecuteUnoptimizedPath(
	  	"region-execute-unoptimized-path", cl::init(false), cl::Hidden);

	static cl::opt<bool> AllowBenLoops(
  		"region-allow-ben-loops", cl::init(false), cl::Hidden);

	static cl::opt<bool> GetTimeStats(
  		"region-get-time-stats", cl::init(false), cl::Hidden);

	static cl::opt<uint32_t> BenLoopThreshold(
  		"region-ben-loop-threshold", cl::init(0), cl::Hidden);

	static cl::opt<bool> SameMACountStats(
	  	"region-same-macount", cl::init(false), cl::Hidden);

	static cl::opt<bool> HandleSameMA(
	  	"region-handle-samema", cl::init(false), cl::Hidden);

	static cl::opt<bool> DisableDynamicChecks(
	  	"region-disable-checks", cl::init(false), cl::Hidden);

	static cl::opt<bool> EnableWrapperAnnotation(
	  	"region-wrapper-annotation", cl::init(false), cl::Hidden);

	static cl::opt<bool> DynamicCheckOverhead(
	  	"region-check-overhead", cl::init(false), cl::Hidden);

	static cl::opt<uint32_t> RegionThreshold(
  		"region-threshold", cl::init(256), cl::Hidden);

	static cl::opt<bool> DisableLoops(
                "disable-loops", cl::init(false), cl::Hidden);

	static cl::opt<bool> EnableNative(
                "enable-native", cl::init(false), cl::Hidden);

	static cl::opt<bool> GetFuncTimeStats(
  		"region-get-func-time-stats", cl::init(false), cl::Hidden);

	static cl::opt<bool> AllowBenFunc(
  		"region-allow-ben-functions", cl::init(false), cl::Hidden);

	static cl::opt<uint32_t> BenFuncThreshold(
  		"region-ben-functions-threshold", cl::init(0), cl::Hidden);

	static cl::opt<bool> LoopIterationStats(
  		"region-loop-iter-stats", cl::init(false), cl::Hidden);

	static cl::opt<uint32_t> LoopIterThreshold(
  		"region-loop-iter-threshold", cl::init(0), cl::Hidden);
}

using namespace llvm;
using namespace std;

#define DEBUG_TYPE RB_NAME
#define RB_NAME "region-based-dis"
#define MAXID 30000

STATISTIC(VersionedLoop, "Number of versioned loops");
STATISTIC(RegionNewVectLoop, "Number of newly vectorized loops");
STATISTIC(RegionCount, "Number of regions required");
STATISTIC(ReplacedMA, "Number of replaced memory allocations");

namespace {

	struct RegionBasedDis : public FunctionPass {
		static char ID;

		explicit RegionBasedDis() : FunctionPass(ID) {
			initializeRegionBasedDisPass(*PassRegistry::getPassRegistry());
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

			if (CheckInfoFile.empty()) {
        //addDummyStore(F);
        return false;
			}

			//convertAllocaToMalloc(F, TLI);
			DenseMap<Instruction *, Instruction *> AlignedAllocToAllocMap;
			convertAllocaToMallocForUse(F, TLI, AlignedAllocToAllocMap);

			assert(ifFileExists(CheckInfoFile));

			if (!isCheckInfoFound(CheckInfoFile)) {
        //addDummyStore(F);
        return false;
			}

			if(EnableNative) {
				DisableDynamicChecks = true;
			}

			vector<Loop *> Loops;
			collectInnermostLoops(LI, Loops);

			DenseMap<unsigned, unsigned long long> UnoptimizedTimeStatsMap;
			DenseMap<unsigned, unsigned long long> OptimizedTimeStatsMap;
			if(AllowBenLoops && Loops.size() > 0) {
	    		createTimeStatsMap(CheckInfoFile, UnoptimizedTimeStatsMap, OptimizedTimeStatsMap);
	    	}

			DenseMap<unsigned, unsigned long long> UnoptimizedFuncTimeStatsMap;
			DenseMap<unsigned, unsigned long long> OptimizedFuncTimeStatsMap;
	    	if(AllowBenFunc && Loops.size() > 0) {
	    		createFuncTimeStatsMap(CheckInfoFile, UnoptimizedFuncTimeStatsMap, OptimizedFuncTimeStatsMap);
	    	}

	    	DenseMap<unsigned, unsigned long long> LoopIterStatsMap;
	    	if(LoopIterThreshold > 0 && Loops.size() > 0) {
	    		createLoopIterStatsMap(CheckInfoFile, LoopIterStatsMap);
	    	}

			if(!DisableDynamicChecks && F.getName().equals("main")) {
				if(GetTimeStats) {
					instrumentTimeWriteFunction(&F, CheckInfoFile);
				}
				if(LoopTakenStats) {
					instrumentWriteFunction(&F, CheckInfoFile);
				}
				if(SameMACountStats) {
					assert(HandleSameMA);
					instrumentSameMACountWriteFunc(&F, CheckInfoFile);
				}
				if(GetFuncTimeStats) {
					instrumentFuncTimeWriteFunction(&F, CheckInfoFile);
				}
				if(LoopIterationStats) {
					instrumentLoopIterStatsFunction(&F, CheckInfoFile);
				}
			}

			unsigned FID = UINT_MAX;
			FID = getFuncID(&F, FuncID, CheckInfoFile);
			assert(FID != UINT_MAX);
			DenseMap<Instruction *, unsigned> AllocationIDMap;
			DenseMap<Instruction *, unsigned> AllocationIDMap1;
			mapAllocationSitesToID(&F, FID, AllocationIDMap1, CheckInfoFile);
			mapAlignedAllocIDToAlloc(AllocationIDMap, AllocationIDMap1, AlignedAllocToAllocMap);
			
			//instrumentSetASIDFunction(&F, AllocationIDMap);
			FuncID++;

			static DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> DynamicCheckMap;
			static DenseMap<unsigned, unsigned> LoopIDIndiMap;
			static DenseMap<unsigned, unsigned> LoopIDTotalMap;
			static DenseMap<unsigned long long, DenseSet<unsigned long long>> VertexNeighborMap, VertexNeighborMap1;
			static DenseMap<unsigned, bool> CanAddCheckLoop;
			static DenseSet<unsigned> MASameID;
			static DenseSet<unsigned> StackIDSet;
			static DenseSet<unsigned> ReallocIDSet;
			static DenseMap<unsigned, unsigned> StartIDMap;
			static DenseMap<unsigned, unsigned> EndIDMap;
			static unsigned MAXALLOCAID = 0;
			static unsigned AllocaRegionMap[MAXID] = {0};
			static unsigned AllocaRegionMap1[MAXID] = {0};
			static unsigned MAXREGION = 0, MAXREGION1 = 0;
			static unsigned MAXStackRegion = 0;
			static bool isLoaded = false;
			if(!isLoaded) {
				LoadCheckInfo(CheckInfoFile, DynamicCheckMap, LoopIDIndiMap, LoopIDTotalMap);
				getStackID(CheckInfoFile, StackIDSet);
				//getReallocID(CheckInfoFile, ReallocIDSet);
				errs() << "Number of stack allocations " << StackIDSet.size() << "\n";

				DiscardLoopsWithSameID(DynamicCheckMap, LoopIDIndiMap, LoopIDTotalMap, CanAddCheckLoop);

				CreateGraph(DynamicCheckMap, VertexNeighborMap, MAXALLOCAID);
				assert(MAXALLOCAID > 0);
				assert(MAXALLOCAID < MAXID);

				//if(HandleSameMA) {
				if(!DisableLoops) {
					getSameMAID(DynamicCheckMap, MASameID);
				}

				AssignRegionToAlloca(AllocaRegionMap, VertexNeighborMap, MAXALLOCAID + 1, MAXREGION, MASameID, StackIDSet, MAXStackRegion);
				//AssignRegionToRealloc(AllocaRegionMap, VertexNeighborMap, MAXALLOCAID + 1, MAXREGION, MASameID, StackIDSet, ReallocIDSet);
				AssignRegion(AllocaRegionMap, VertexNeighborMap, MAXALLOCAID + 1, MAXREGION, MASameID, StackIDSet, ReallocIDSet);

				errs() << "Minimum number of regions required " << MAXREGION << "\n";
				errs() << "Maximum region for stack allocations " << MAXStackRegion << "\n";
				for(unsigned i = 0; i < MAXALLOCAID + 1; i++) {
					if(AllocaRegionMap[i] > 0) {
						errs() << "Vertex " << i << " Region " << AllocaRegionMap[i] << "\n"; 
					}
				}
				RegionCount = MAXREGION;

				//if(HandleSameMA) {
				if(MASameID.size() > 0) {
					CreateGraphWithSameID(DynamicCheckMap, VertexNeighborMap1, MASameID);
					AssignRegionWithSameID(AllocaRegionMap1, VertexNeighborMap1, MAXALLOCAID + 1, MAXREGION1, MASameID);
					errs() << "Minimum number of regions required for same ID " << MAXREGION1 << "\n";
					for(unsigned i = 0; i < MAXALLOCAID + 1; i++) {
						if(AllocaRegionMap1[i] > 0) {
							errs() << "Vertex " << i << " Region " << AllocaRegionMap1[i] << "\n"; 
						}
					}
					getRegionIDForSameID(MAXREGION, AllocaRegionMap1, RegionThreshold, MASameID, StartIDMap, EndIDMap, MAXREGION1);
				}
				if(VertexNeighborMap.size() > 0) {
					assert(MAXREGION > 0);
				}
				assert(MAXREGION < MAXALLOCAID);
				isLoaded = true;
			}

			if(SameMACountStats) {
				assert(HandleSameMA);
				instrumentSameCountFunc(MASameID, AllocationIDMap);
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
			
			if(!DisableDynamicChecks) {
				unsigned ID = 0;
				//Performs loop versioning.
				for(auto L : Loops) {
					assert(L->empty());
					LoopIDMap[L] = ID;
					ID++;

					unsigned LoopID = UINT_MAX;
				    LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
				    assert(LoopID != UINT_MAX);

				    if(!DynamicCheckMap.count(LoopID)) {
				    	continue;
				    }

				    if(EnableWrapperAnnotation) {
 				    	for(auto Check: DynamicCheckMap[LoopID]) {
							if(Check.first == Check.second) {
								if(Check.first != 0 || Check.second != 0) {
									errs() << "Loop with same ASID " << LoopID << "\n";
								}
								//assert(Check.first == 0 && Check.second == 0);
							}
						}
				    }

				    if(MASameID.size() == 0 && !CanAddCheckLoop[LoopID]) {
				    	continue;
				    }
				    /*if(!HandleSameMA && !CanAddCheckLoop[LoopID]) {
						continue;
					}

				    if(!DynamicCheckMap.count(LoopID) || (!HandleSameMA && !CanAddCheckLoop[LoopID])) {
				    	continue;
				    }*/
				    if(AllowBenFunc) {
						unsigned FID = UINT_MAX;
					    FID = getFuncID(&F, FuncID - 1, CheckInfoFile);
					    assert(FID != UINT_MAX);
					    if(!isFuncBenefited(FID, UnoptimizedFuncTimeStatsMap, OptimizedFuncTimeStatsMap, BenFuncThreshold)) {
				    		continue;
				    	}
				    }

				    if(LoopIterThreshold > 0 && LoopIterStatsMap.count(LoopID) && LoopIterStatsMap[LoopID] < LoopIterThreshold) {
				    	continue;
				    }
				    errs() << "Benefited loop ID " << LoopID << "\n";

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
					}
				}
			}

			if(AllowBenFunc) {
				unsigned FID = UINT_MAX;
				FID = getFuncID(&F, FuncID - 1, CheckInfoFile);
				assert(FID != UINT_MAX);
				if(!isFuncBenefited(FID, UnoptimizedFuncTimeStatsMap, OptimizedFuncTimeStatsMap, BenFuncThreshold)) {
				    assert(LoopBlockMap.size() == 0);
				}
			}

			if(!DisableDynamicChecks) {
				//Performs LICM, GVN and DSE.
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
			}
			AA->setDisableCustomIntrinsicChecks(true);

			if(!DisableDynamicChecks) {
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
					if(AllowBenLoops) {
		    			unsigned LoopID = UINT_MAX;
		    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
		    			assert(LoopID != UINT_MAX);
		    			if(!isLoopBenefited(LoopID, UnoptimizedTimeStatsMap, OptimizedTimeStatsMap, BenLoopThreshold)) {
		    				isBenefited = false;
		    			}
		    		}

		    		simplifyLoop(L, DT, LI, SE, AC, nullptr, false);

		    		//The loop should be benefited.
		    		assert((!isVectorizableBefore && isVectorizableAfter) || otherOpt);

					DenseSet<pair<Value *, Value *>> FinalBases;
					if(!isVectorizableBefore && isVectorizableAfter) {
						DenseSet<pair<Instruction *, Instruction *>> InstSet = LoopAllInstSetMap[L];
						getBasesLVBenefited(L, InstSet, FinalBases, LI, DT);
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
					}
					else {
						assert(0);
					}

					assert(FinalBases.size() > 0);
					if(!AllowBenLoops) {
						assert(isBenefited);
					}

		    		bool found = false;

		    		if(!isBenefited) {
		    			simplifyLoop(NewLoop, DT, LI, SE, AC, nullptr, false);
		    			formLCSSARecursively(*NewLoop, *DT, LI, SE);
		    			removeDeadLoop(NewLoop);
		    			replaceCondWithTrue(L, RuntimeChecksBlock);
		    			deleteDeadLoop(NewLoop, DT, SE, LI);
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

							if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
								DenseSet<pair<Value *, Value *>> AddedBases1;
								emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, 
									FinalBases, false, AddedBases1, "", TLI);
							}

							DenseSet<pair<Value *, Value *>> AddedBases2;
							addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
							RegionNewVectLoop++;
						}
						else if(otherOpt) {
							if(StoreSet.size() > 0) {
								found = true;
								for(auto Pair : StoreDomainMap) {
									if(!StoreSet.count(Pair.first)) {
										removeDomain(NewLoop, Pair.second);
									}
								}

								if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
									DenseSet<pair<Value *, Value *>> AddedBases1;
									emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, 
										FinalBases, false, AddedBases1, "", TLI);
								}

								DenseSet<pair<Value *, Value *>> AddedBases2;
								addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);

				    		}
				    		else if(LoadSet.size() == AllLoads.size()) {
				    			found = true;
				    			assert(StoreSet.size() == 0);

				    			for(auto Pair : StoreDomainMap) {
				    				removeDomain(NewLoop, Pair.second);
				    			}

				    			DenseSet<pair<Value *, Value *>> AddedBases1;
				    			if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
				    				emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, 
				    					FinalBases, false, AddedBases1, "", TLI);
				    			}

				    			DenseSet<pair<Value *, Value *>> AddedBases2;
				    			addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
				    		}
				    		else if(LoadSet.size() < AllLoads.size()) {
				    			found = true;
				    			assert(StoreSet.size() == 0);
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

				    				DenseSet<pair<Value *, Value *>> AddedBases1;
				    				if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
				    					emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, 
				    						FinalBases, false, AddedBases1, "", TLI);
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
				    						removeConflictingLoadDomain(NewInstMap[OldInstMap[Pair.first]], Pair.second);
				    					}
				    				}
									
									DenseSet<pair<Value *, Value *>> AddedBases1;
									if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
										emitRuntimeChecks(&F, FuncID - 1, LoopIDMap[L], L, NewLoop, RuntimeChecksBlock, 
											FinalBases, false, AddedBases1, "", TLI);
									}

									DenseSet<pair<Value *, Value *>> AddedBases2;
									addCustomNoAliasIntrinsic(&F, NewLoop, FinalBases, AddedBases2);
								}
				    		}
			    		}
			    		else {
			    			assert(0);
			    		}
			    		assert(found);

			    		if(ExecuteOptimizedPath) {
			    			for(auto Pair : LoadDomainMap) {
			    				removeDomain(NewLoop, Pair.second);
			    			}

			    			for(auto Pair : StoreDomainMap) {
			    				removeDomain(NewLoop, Pair.second);
			    			}

			    			for(auto Pair : ConflictingLoadDomainMap) {
			    				if(NewInstMap.count(OldInstMap[Pair.first])) {
			    					removeConflictingLoadDomain(NewInstMap[OldInstMap[Pair.first]], Pair.second);
			    				}
			    			}
			    		}
			    		F.addFnAttr("function_benefitted");

			    		if(isVectorizableBefore && !isVectorizableAfter) {
			    			DenseSet<PHINode *> PHINodeSet;
			    			for (BasicBlock *BB : NewLoop->blocks()) {
			    				for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
			    					Instruction *II = &*I;
			    					if(PHINode *PN = dyn_cast<PHINode>(II)) {
			    						PHINodeSet.insert(PN);
			    					}
			    				}
			    			}

			    			for(auto PHI: PHINodeSet) {
			    				DT->recalculate(F);
			    				fixIRForVectorization(&F, PHI, LI, DT, AA);
			    			}
			    		}

			    		if(GetTimeStats) {
			    			unsigned LoopID = UINT_MAX;
			    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
			    			assert(LoopID != UINT_MAX);
			    			instrumentTimeFunction(&F, RuntimeChecksBlock, L->getExitBlock(), LoopID);
			    		}

			    		if(GetFuncTimeStats) {
			    			unsigned FID = UINT_MAX;
			    			FID = getFuncID(&F, FuncID - 1, CheckInfoFile);
			    			assert(FID != UINT_MAX);
			    			instrumentFuncTimeFunction(&F, FID);
			    		}

			    		if(LoopTakenStats) {
				    		unsigned LoopID = UINT_MAX;
				    		LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
				    		assert(LoopID != UINT_MAX);
				    		instrumentDebugFunction(L, 0, LoopID);
				    		instrumentDebugFunction(NewLoop, 1, LoopID);
				    	}

				    	if(LoopIterationStats) {
				    		unsigned LoopID = UINT_MAX;
				    		LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
				    		assert(LoopID != UINT_MAX);
				    		instrumentUpdateLoopIterFunction(NewLoop, LoopID);
				    	}
			    		
			    		if(!ExecuteOptimizedPath && !ExecuteUnoptimizedPath) {
			    			unsigned LoopID = UINT_MAX;
			    			LoopID = getLoopID(&F, FuncID - 1, LoopIDMap[L], CheckInfoFile);
			    			assert(LoopID != UINT_MAX);
			    			if(DynamicCheckOverhead) {
			    				createRegionBasedChecksForOverhead(&F, RuntimeChecksBlock, LoopID, LoopIDIndiMap);
			    			}
			    			else {
			    				createRegionBasedChecks(&F, RuntimeChecksBlock, LoopID, LoopIDIndiMap);
			    			}
			    			//createRegionBasedChecksWithID(&F, RuntimeChecksBlock);
			    		}
			    		
			    		VersionedLoop++;
		    		}

		    		if(isVectorizableBefore && !isVectorizableAfter /*&& NewLoop->getLoopLatch()*/) {
				    	DenseSet<PHINode *> PHINodeSet;
					    for (BasicBlock *BB : NewLoop->blocks()) {
							for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I) {
								Instruction *II = &*I;
							    if(PHINode *PN = dyn_cast<PHINode>(II)) {
							      	PHINodeSet.insert(PN);
							    }
							}
						}

						for(auto PHI: PHINodeSet) {
							DT->recalculate(F);
							fixIRForVectorization(&F, PHI, LI, DT, AA);
						}
					}
				}

				removeInstrumentedFunctions(&F, ExecuteOptimizedPath, ExecuteUnoptimizedPath);
			}

			AA->setDisableCustomIntrinsicChecks(false);

			unsigned ReplacedMACount = 0;
			//instrumentRegionIDFunction(&F, AllocationIDMap, AllocaRegionMap);
			//createStoreToGlobal(&F, AllocationIDMap, AllocaRegionMap, TLI);
			setStackRegionReq(MAXStackRegion);
			
			if(EnableNative) {
				replaceMemoryAllocationsForNative(&F, AllocationIDMap, AllocaRegionMap, TLI, ReplacedMACount, EnableWrapperAnnotation, 
					RegionThreshold, StartIDMap, EndIDMap);
			}
			else {
				replaceMemoryAllocations(&F, AllocationIDMap, AllocaRegionMap, TLI, ReplacedMACount, EnableWrapperAnnotation, 
					RegionThreshold, StartIDMap, EndIDMap);
			}
			
			ReplacedMA += ReplacedMACount;
			if (verifyFunction(F, &errs())) {
			    errs() << "Not able to verify!\n";
			    errs() << F << "\n";
			    assert(0);
			}
      //addDummyStore(F);
			return true;
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
	};
} // end of anonymous namespace.

using namespace llvm;

char RegionBasedDis::ID = 0;

static const char rb_name[] = "Region Based Pointer Disambiguation";

INITIALIZE_PASS_BEGIN(RegionBasedDis, RB_NAME, 
	rb_name, false /* Only looks at CFG */, false /* Analysis Pass */)

//INITIALIZE_PASS_DEPENDENCY(PromoteLegacyPass)

INITIALIZE_PASS_END(RegionBasedDis, RB_NAME, 
	rb_name, false /* Only looks at CFG */, false /* Analysis Pass */)

namespace llvm {
	Pass *createRegionBasedDisPass() {
		return new RegionBasedDis(); 
	}
}
