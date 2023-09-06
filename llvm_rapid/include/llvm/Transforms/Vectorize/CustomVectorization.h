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
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"

#include <stdio.h>  
#include <unistd.h>
#include <stdlib.h>
#include <sstream>
#include <fstream>
#include <sys/file.h>

using namespace llvm;
using namespace std;

static bool isFuncDefAdded = false;
static bool isDummyFuncDefAdded = false;
static bool isWriteDefAdded = false;
static bool isDebugFuncDefAdded = false;
static bool isNoAliasIntrisicDefined = false;
static bool isTimeWriteDefAdded = false;
static bool isTimeFuncDefAdded = false;
static bool isFuncTimeFuncDefAdded = false;
static bool isFuncTimeWriteDefAdded = false;
static bool issetASIDDefined = false;
static bool isgetASIDDefined = false;
static bool isWriteSameASIDDefAdded = false;
static bool ischeckASIDDefined = false;
static bool isdynamicCheckDefined = false;
static bool isstoreDynamicChecksDefined = false;
static bool iswriteLoopDynamicChecksLogDefined = false;
static bool iswriteSameMACountFuncDefined = false;
static bool isUniqueIDGlobalCreated = false;
static bool isStackInfoLoaded = false;
static bool isWriteLoopIterStatsDefined = false;
static bool isUpdateLoopIterAdded = false;
static GlobalVariable *UniqueASIDGlobal;
//static bool isDynamicCheckIntrinsicDefAdded = false;
static Function *isNoAliasRTCheckFunc, *isDummyTrueFunc, *isDummyFalseFunc, *NoAliasIntrinsic;
static Function *updateLoopCounterFunc;
static Function *exitFunc;
static Function *writeLoopCounterFunc;
static Function *writeTimeStatsFunc;
static Function *writeFuncTimeStatsFunc;
static Function *startTimeFunc;
static Function *endTimeFunc;
static Function *startFuncTimeFunc;
static Function *endFuncTimeFunc;
static Function *setASIDFunc;
static Function *getASIDFunc;
static Function *writeSameASIDFunc;
static Function *checkASIDFunc;
static Function *dynamicCheckFunc;
static Function *storeDynamicChecksFunc;
static Function *writeLoopDynamicChecksLogFunc;
static Function *writeSameMACountFunc;
static Function *writeLoopIterStatsFunc;
static Function *updateLoopReachedFunc;
static Function *updateLoopIterationsFunc;
//static Function *DynamicCheckIntrisic;
//static DenseMap<Function *, GlobalVariable *> FuncGVMap;
static unsigned FuncID = 0;

static unsigned StackRegionReq = 0;

void fixIRForVectorization(Function *F, PHINode *PHI, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA);

void createCloneToVectorize(Loop *L, LoopInfo *LI, 
		DominatorTree *DT, AAResults *AA, Loop **NL, 
		BasicBlock **RCB, DenseSet<Instruction *> LoadSet, 
		DenseSet<Instruction *> StoreSet, bool calledFromAV);

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
		DenseSet<pair<Instruction *, Instruction *>> &InstSet);

void convertCallToCmp(BasicBlock *RuntimeChecksBlock);

void instrumentDebugFunction(Loop *CLoop, unsigned index, 
	unsigned LoopID);

void instrumentWriteFunction(Function *F, string FileName);

void restoreLoop(BasicBlock *RuntimeChecksBlock);

void mapInstructions(Loop *CLoop, Loop *NewLoop, 
	DenseMap<Instruction *, unsigned> &OldInstMap, 
	DenseMap<unsigned, Instruction *> &NewInstMap);

void updatePredecessors(BasicBlock *LoopPreheader, 
	BasicBlock *RuntimeChecks);

bool areMemDependenciesFoundSelectedInst(Loop *CLoop, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	DenseSet<pair<Instruction *, Instruction *>> &Insts, 
	DenseSet<Instruction *> &ConflictingLoads,
	DenseSet<Instruction *> &LoadInsts,
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, 
	DenseSet<Instruction *> LoadSet, 
	DenseSet<Instruction *> StoreSet);

void emitRuntimeChecks(Function *F, unsigned FID, unsigned LID, Loop *CLoop, Loop *NewLoop, BasicBlock *RuntimeChecksBlock, 
	DenseSet<pair<Value *, Value *>> Bases, bool performCloning, DenseSet<pair<Value *, Value *>> &AddedBases, string FileName, 
	TargetLibraryInfo *TLI);

void removeMetadataFromAll(Loop *CLoop);

void addScopedNoaliasMetadata(Loop *CLoop, Loop *NewLoop, 
		DenseSet<pair<Instruction *, Instruction *>> Insts, 
		DenseSet<Instruction *> ConflictingLoads, 
		DenseSet<Instruction *> LoadInsts,
		DenseMap<Instruction *, unsigned> OldInstMap, 
		DenseMap<unsigned, Instruction *> NewInstMap);

void getAllLoadStore(Loop *L, DenseSet<Instruction *> 
	&AllLoads, DenseSet<Instruction *> &AllStores);

bool LoadDep(Loop *L, Instruction *Load, DenseSet<Instruction *> AllLoads, 
	DenseSet<Instruction *> AllStores, 
	DenseSet<Instruction *> &ConflictingLoads, 
	DenseSet<pair<Instruction *, Instruction *>> &InstSet, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, DenseSet<Value *> &SafeBases, 
	DenseSet<Instruction *> &ConflictingStores);

void StoreDep(Loop *L, Instruction *Store, DenseSet<Instruction *> AllStores,
	DenseSet<pair<Instruction *, Instruction *>> &InstSet, 
	DenseSet<pair<Value *, Value *>> &Bases, 
	LoopInfo *LI, DominatorTree *DT, AAResults *AA, DenseSet<Value *> &SafeBases);

bool getBasesPair(Loop *L, MemoryLocation Loc1, MemoryLocation Loc2, 
	DenseSet<pair<Value *, Value *>> &Bases, LoopInfo *LI, 
	DominatorTree *DT, DenseSet<Value *> &SafeBases, bool first, bool second);

void addMetadata(Loop *L, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<Instruction *> ConflictingLoads, DenseSet<Instruction *> AllLoads, 
	DenseSet<Instruction *> AllStores, 
	DenseMap<Instruction *, unsigned> OldInstMap, 
	DenseMap<unsigned, Instruction *> NewInstMap, 
	DenseMap<Instruction, DenseSet<MDNode *>> &LoadDomainMap, 
	DenseMap<Instruction, DenseSet<MDNode *>> &ConflictingLoadDomainMap, 
	DenseMap<Instruction, DenseSet<MDNode *>> &StoreDomainMap, AAResults *AA);

void getLoadStoreSet(Loop *L, Loop *NewLoop, 
	DenseSet<Instruction *> &LoadSet, 
	DenseSet<Instruction *> &StoreSet);

void removeDomain(Loop *L, DenseSet<MDNode *> Domains);

void addDummyStore(Function &F);

void removeConflictingLoadDomain(Instruction *I, DenseSet<MDNode *> Domains);

void addCustomNoAliasIntrinsic(Function *F, Loop *NewLoop, DenseSet<pair<Value *, Value *>> Bases, 
	DenseSet<pair<Value *, Value *>> &AddedBases);

void removeCustomNoAliasIntrinsic(Function *F);

void removeInstrumentedFunctions(Function *F, bool ExecuteOptimizedPath, 
	bool ExecuteUnoptimizedPath);
bool splitIntrinsic(Function &F, LoopInfo &LI, DominatorTree &DT, PostDominatorTree &PDT);
void mergeIntrinsics(Function &F);

void removeDummyFunctionCall(Function *F);

unsigned getLoopID(string filename);

void collectInnermostLoops(LoopInfo *LI, vector<Loop *> &Loops);

void assignFunctionID(Module *M, string filename);

void assignLoopID(Function *F, unsigned FuncID, unsigned LoopCount, string filename);

unsigned getLoopID(Function *F, unsigned FuncID, unsigned LoopID, string filename);

void instrumentTimeWriteFunction(Function *F, string FileName);

void instrumentTimeFunction(Function *F, BasicBlock *StartTimeBlock, 
	BasicBlock *EndTimeBlock, unsigned LoopID);

void synchronize(const char *path, bool lock);

void replaceCondWithTrue(Loop *L, BasicBlock *RuntimeChecksBlock);

bool isLoopBenefited(unsigned LoopID, DenseMap<unsigned, unsigned long long> UnoptimizedTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> OptimizedTimeStatsMap, unsigned threshold);

bool isFuncBenefited(unsigned FID, DenseMap<unsigned, unsigned long long> UnoptimizedFuncTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> OptimizedFuncTimeStatsMap, unsigned threshold);

void createTimeStatsMap(string filename, DenseMap<unsigned, unsigned long long> &UnoptimizedTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> &OptimizedTimeStatsMap);

void createFuncTimeStatsMap(string filename, DenseMap<unsigned, unsigned long long> &UnoptimizedFuncTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> &OptimizedFuncTimeStatsMap);

void removeDeadLoop(Loop *L);

void instrumentFuncTimeWriteFunction(Function *F, string FileName);

unsigned getFuncID(Function *F, unsigned FuncID, string filename);

void instrumentFuncTimeFunction(Function *F, unsigned FuncID);

//void convertDynamicCheckIntrToCall(Function *F);

bool areBasePairsSafe(DenseSet<Value *> SafeBases,
	DenseSet<pair<Value *, Value *>> Bases);

void unionLoadStoreBases(DenseSet<pair<Value *, Value *>> LoadBases, DenseSet<pair<Value *, Value *>> StoreBases, 
	DenseSet<pair<Value *, Value *>> &FinalBases);

void setGlobalAddressVariable(Instruction *I);

bool isIndStepFound(Loop *L, ScalarEvolution *SE);

void getBasesLVBenefited(Loop *L, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT);

void getBasesStoreBenefited(Loop *L, Instruction *Store, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT);

void getBasesLoadBenefited(Loop *L, Instruction *Load, DenseSet<pair<Instruction *, Instruction *>> InstSet, 
	DenseSet<pair<Value *, Value *>> &FinalBases, LoopInfo *LI, DominatorTree *DT);

void mapAllocationSitesToID(Function *F, unsigned FID, DenseMap<Instruction *, unsigned> &AllocationIDMap, string FileName);

void instrumentSetASIDFunction(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, TargetLibraryInfo *TLI, bool EnableWrapperAnnotation);

void instrumentWriteSameASIDFunction(Function *F, string FileName);

void instrumentWriteLoopDynamicChecksLogFunction(Function *F, string FileName);

void instrumentTimeFunction2(Function *F, Loop *L, Loop *NewLoop, unsigned LoopID);

//Region Based Pointer Disambiguation.
void LoadCheckInfo(string filename, DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> 
	&DynamicCheckMap, DenseMap<unsigned, unsigned> &LoopIDIndiMap, DenseMap<unsigned, unsigned> &LoopIDTotalMap);

void CreateGraph(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> DynamicCheckMap, 
	DenseMap<unsigned long long, DenseSet<unsigned long long>> &VertexNeighborMap, unsigned &MAXALLOCAID);

void AssignRegionToAlloca(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, unsigned &MAXStackRegion);

void AssignRegionToRealloc(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, DenseSet<unsigned> ReallocIDSet);

void AssignRegion(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, DenseSet<unsigned> ReallocIDSet);

void createRegionBasedChecks(Function *F, BasicBlock *RuntimeChecksBlock, unsigned LoopID, 
	DenseMap<unsigned, unsigned> LoopIDIndiMap);

//bool isSafeAlloca(const AllocaInst *AllocaPtr, const TargetLibraryInfo &TLI);

//void allocaToMalloc(Function *F, AllocaInst *AI);

bool doesExternalCall(Function &F);

//void setGlobalLimit(Function &F);

void convertAllocaToMalloc(Function &F, TargetLibraryInfo *TLI, unsigned &ConvertedStackAllocations);

void assignASID(Function *F, unsigned FID, string filename);

bool ifFileExists(string filename);

bool isCheckInfoFound(string filename);

void createRegionBasedChecksWithID(Function *F, BasicBlock *RuntimeChecksBlock);

void DiscardLoopsWithSameID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> 
	DynamicCheckMap, DenseMap<unsigned, unsigned> LoopIDIndiMap, 
	DenseMap<unsigned, unsigned> LoopIDTotalMap, DenseMap<unsigned, bool> &CanAddCheckLoop);

void instrumentRegionIDFunction(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[]);

void replaceMemoryAllocations(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, bool EnableWrapperAnnotation, 
	unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap);

void getRegionIDForSameID(unsigned MAXREGION, unsigned AllocaRegionMap[], unsigned threshold, 
	DenseSet<unsigned> MASameID, DenseMap<unsigned, unsigned> &StartIDMap,
	DenseMap<unsigned, unsigned> &EndIDMap, unsigned MAXREGION1);

void replaceMemoryAllocationsWithSameID(Function *F, unsigned MAXREGION, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap,
	DenseMap<unsigned, unsigned> EndIDMap);

void getSameMAID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> 
	DynamicCheckMap, DenseSet<unsigned> &MASameID);

//void mapAllocationSitesToIDForUse(Function *F, unsigned FID, DenseMap<Instruction *, 
//	unsigned> &AllocationIDMap, string FileName, TargetLibraryInfo *TLI);

void CreateGraphWithSameID(DenseMap<unsigned, vector<pair<unsigned long long, unsigned long long>>> DynamicCheckMap, 
	DenseMap<unsigned long long, DenseSet<unsigned long long>> &VertexNeighborMap, DenseSet<unsigned> MASameID);

void AssignRegionWithSameID(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID);

void instrumentSameMACountWriteFunc(Function *F, string FileName);

void instrumentSameCountFunc(DenseSet<unsigned> MASameID, DenseMap<Instruction *, unsigned> AllocationIDMap);

void convertAllocaToMallocForUse(Function &F, TargetLibraryInfo *TLI, DenseMap<Instruction *, Instruction *> &AlignedAllocToAllocMap);

void mapAlignedAllocIDToAlloc(DenseMap<Instruction *, unsigned> &AllocationIDMap, DenseMap<Instruction *, unsigned> 
	AllocationIDMap1, DenseMap<Instruction *, Instruction *> AlignedAllocToAllocMap);

void createStoreToGlobal(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI);

void storeStackID(string filename, DenseMap<Instruction *, unsigned> AllocationIDMap);

void getStackID(string filename, DenseSet<unsigned> &StackIDSet);

void setStackRegionReq(unsigned MAXStackRegion);

unsigned getStackRegionReq();

void createRegionBasedChecksForOverhead(Function *F, BasicBlock *RuntimeChecksBlock, unsigned LoopID, 
	DenseMap<unsigned, unsigned> LoopIDIndiMap);

void AssignRegionToAlloca(unsigned AllocaRegionMap[], DenseMap<unsigned long long, DenseSet<unsigned long long>> 
	VertexNeighborMap, unsigned MAXALLOCAID, unsigned &MAXREGION, DenseSet<unsigned> MASameID, 
	DenseSet<unsigned> StackIDSet, unsigned &MAXStackRegion);

bool isRealloc(Instruction *I);

void storeReallocID(string filename, DenseMap<Instruction *, unsigned> AllocationIDMap);

void getReallocID(string filename, DenseSet<unsigned> &ReallocIDSet);

void replaceMemoryAllocationsForNative(Function *F, DenseMap<Instruction *, unsigned> AllocationIDMap, 
	unsigned AllocaRegionMap[], TargetLibraryInfo *TLI, unsigned &ReplacedMA, bool EnableWrapperAnnotation, 
	unsigned threshold, DenseMap<unsigned, unsigned> StartIDMap, DenseMap<unsigned, unsigned> EndIDMap);

void instrumentLoopIterStatsFunction(Function *F, string FileName);

void instrumentUpdateLoopIterFunction(Loop *CLoop, unsigned LoopID);

void createLoopIterStatsMap(string filename, DenseMap<unsigned, unsigned long long> &LoopIterStatsMap);

bool isAllocationSite(Instruction *I);

bool isPosixMemalign(Instruction *I);
