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
//static Function *DynamicCheckIntrisic;
//static DenseMap<Function *, GlobalVariable *> FuncGVMap;
static unsigned FuncID = 0;

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

void emitRuntimeChecks(Loop *CLoop, Loop *NewLoop, BasicBlock *RuntimeChecksBlock, 
	DenseSet<pair<Value *, Value *>> Bases, bool performCloning, DenseSet<pair<Value *, Value *>> &AddedBases);

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

void createTimeStatsMap(string filename, DenseMap<unsigned, unsigned long long> &UnoptimizedTimeStatsMap, 
	DenseMap<unsigned, unsigned long long> &OptimizedTimeStatsMap);

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
