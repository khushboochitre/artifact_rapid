#include <pwd.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <limits.h>

#define MAX_CACHE_ENTRIES 32
#define LARGE_MAGIC 0xdeadfacedeaddeed
#define SEGMENT_SIZE (1ULL << 32)
#define MAX_LEN 30000
#define STACK_LEN 1024
#define STR_MAX_LEN 1000
#define CHECK_LEN 1000
#define CUR_CHECK_LEN 50

#define ADDR_TO_SEGMENT(x) (unsigned long long*)(((unsigned long long)(x)) & ~(SEGMENT_SIZE-1))

//size_t max_globaladdr = 0xFFFFFFFFFFFFFFFF;
size_t max_globaladdr = 0xFFFFFFFF;

//static unsigned MAXALLOCAID = UINT_MAX;

struct LoopInfo {
	unsigned long long Count1 = 0;
	unsigned long long Count2 = 0;
} Summary[MAX_LEN];

unsigned long long TimeInfo[MAX_LEN] = {0};
unsigned long long TimeInfoFunc[MAX_LEN] = {0};

struct StackInfo {
	unsigned ID;
	unsigned long long startTime;
};

struct StackInfo Stack[STACK_LEN] = {0};
struct StackInfo FuncStack[STACK_LEN] = {0};

int top_of_stack = -1;
int func_top_of_stack = -1;

static size_t san_getsize(void *Ptr) {
	assert(Ptr);
	unsigned long long *S = ADDR_TO_SEGMENT(Ptr);
	assert(S);
	assert(S[2] == LARGE_MAGIC);
	return S[0];
}

extern "C" void setGlobalVar() {
	max_globaladdr = 0xFFFFFFFF;
}

extern "C" bool returnsTrue() {
	return true;
}

extern "C" bool returnsFalse() {
	return false;
}

extern "C" bool callExit() {
	exit(0);
}

extern "C" size_t return_size(void *P1) {
	if((size_t)P1 < max_globaladdr) {
		return max_globaladdr;
	}
	else {
		size_t Sz = san_getsize(P1);
		return Sz;
	}
}

extern "C" bool isNoAliasRTCheck(void *P1, void *P2, size_t Sz) {
	return ((size_t)P1 ^ (size_t)P2) >= Sz;
	/*size_t S1 = san_getsize(P1);
	size_t S2 = san_getsize(P2);
	assert(S1 > 0 || S2 > 0);
	if(S1 == 0 && S2 > 0) {
		if(((uintptr_t)P1 ^ (uintptr_t)P2) < S2) {
			return false;
		}
		else {
			return true;
		}
	}
	else if(S1 > 0 && S2 == 0) {
		if(((uintptr_t)P1 ^ (uintptr_t)P2) < S1) {
			return false;
		}
		else {
			return true;
		}
	}
	else if(S1 == S2) {
		if(((uintptr_t)P1 ^ (uintptr_t)P2) < S1) {
			return false;
		}
		else {
			return true;
		}
	}
	else {
		return true;
	}*/
	/*if(P1 == P2) {
		return false;
	}
	return true;*/
}

extern "C" void updateLoopCounter(unsigned ID, unsigned index) {
   	if(index == 0) {
   		Summary[ID].Count1++;
   	}
   	else {
   		Summary[ID].Count2++;
   	}
}

void ReadLoopCounter(char *file) {
	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		int ID;
		unsigned long long Count1, Count2;
		if(sscanf(line, "%d %llu %llu", &ID, &Count1, &Count2) == 3) {
			Summary[ID].Count1 += Count1;
			Summary[ID].Count2 += Count2;
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeLoopCounter(char *file) {
	ReadLoopCounter(file);

	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(Summary[i].Count1 > 0 || Summary[i].Count2 > 0) {
   			fprintf(fptr_d, "%d %llu %llu\n", i, Summary[i].Count1, Summary[i].Count2);
   		}
   	}
   	fclose(fptr_d);
}

extern "C" void rdtsc_s(unsigned LoopID)
{
	unsigned a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d)); 
	unsigned long long start = ((unsigned long)a) | (((unsigned long long)d) << 32);
	top_of_stack++;
	assert(top_of_stack < STACK_LEN);
	Stack[top_of_stack].ID = LoopID;
	Stack[top_of_stack].startTime = start;
}

extern "C" void rdtsc_e(unsigned LoopID)
{
	unsigned a, d; 
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	unsigned long long end = ((unsigned long)a) | (((unsigned long long)d) << 32);
	assert(top_of_stack >= 0);
	assert(Stack[top_of_stack].ID == LoopID);
	assert(Stack[top_of_stack].startTime > 0);
	TimeInfo[LoopID] += (end - Stack[top_of_stack].startTime);
	top_of_stack--;
}

extern "C" void rdtsc_s_f(unsigned FuncID)
{
	unsigned a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d)); 
	unsigned long long start = ((unsigned long)a) | (((unsigned long long)d) << 32);
	func_top_of_stack++;
	assert(func_top_of_stack < STACK_LEN);
	FuncStack[func_top_of_stack].ID = FuncID;
	FuncStack[func_top_of_stack].startTime = start;
}

extern "C" void rdtsc_e_f(unsigned FuncID)
{
	unsigned a, d; 
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	unsigned long long end = ((unsigned long)a) | (((unsigned long long)d) << 32);
	assert(func_top_of_stack >= 0);
	assert(FuncStack[func_top_of_stack].ID == FuncID);
	assert(FuncStack[func_top_of_stack].startTime > 0);
	TimeInfoFunc[FuncID] += (end - FuncStack[func_top_of_stack].startTime);
	func_top_of_stack--;
}

void ReadTimeStats(char *file) {
	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		int ID;
		unsigned long long time;
		if(sscanf(line, "%d %llu", &ID, &time) == 2) {
			TimeInfo[ID] += time;
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeTimeStats(char *file) {
	ReadTimeStats(file);

	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(TimeInfo[i] > 0) {
   			fprintf(fptr_d, "%d %llu\n", i, TimeInfo[i]);
   		}
   	}
   	fclose(fptr_d);
}

void ReadFuncStats(char *file) {
	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		int ID;
		unsigned long long time;
		if(sscanf(line, "%d %llu", &ID, &time) == 2) {
			TimeInfoFunc[ID] += time;
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeFuncStats(char *file) {
	ReadFuncStats(file);

	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(TimeInfoFunc[i] > 0) {
   			fprintf(fptr_d, "%d %llu\n", i, TimeInfoFunc[i]);
   		}
   	}
   	fclose(fptr_d);
}

#if 0
extern "C" void setASID(void *Ptr, unsigned long long ASID) {
	size_t size = return_size(Ptr);
	if(size != max_globaladdr) {
		double shift = log2(size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		*(unsigned long long *)Ptr = ASID;
	}
}
#endif

/*static void getMaxAllocationID() {
	if(MAXALLOCAID != UINT_MAX) {
		return;
	}
	char LogDir[STR_MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, STR_MAX_LEN, "%s/stats/as_last_id.txt", LogDirEnv);
	assert(len < STR_MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;

   	while ((read = getline(&line, &flen, fptr_d)) != -1) {
   		unsigned VAL;
		if(sscanf(line, "%u", &VAL) == 1) {
			assert(VAL > 1);
			MAXALLOCAID = VAL;
		}
	}
	assert(MAXALLOCAID != UINT_MAX);
	fclose(fptr_d);
}*/
#if 0
extern "C" unsigned long long getASID(void *Ptr) {
	//getMaxAllocationID();
	//assert(MAXALLOCAID != UINT_MAX);
	size_t size = return_size(Ptr);
	if(size != max_globaladdr) {
		double shift = log2(size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		unsigned long long VAL = *(unsigned long long *)Ptr;
		/*if(VAL >= MAXALLOCAID) {
			assert(0);
		}*/
		//assert(VAL > 0);
		return VAL;
	}
	else {
		return 0;
	}
}
#endif

unsigned long long TotalChecks[MAX_LEN] = {0};
unsigned long long SameASID[MAX_LEN] = {0};

extern "C" void checkASID(unsigned LoopID, unsigned long long ID1, unsigned long long ID2) {
	if(ID1 == max_globaladdr || ID2 == max_globaladdr) {
		return;
	}
	TotalChecks[LoopID]++;
	if(ID1 == ID2) {
		SameASID[LoopID]++;
	}
}

void ReadSameASIDStats(char *file) {
	char LogDir[STR_MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, STR_MAX_LEN, "%s/stats/%s_same_asid_stats.txt", LogDirEnv, file);
	assert(len < STR_MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		unsigned ID;
		unsigned long long Same;
		unsigned long long Total;
		if(sscanf(line, "%u %llu %llu", &ID, &Same, &Total) == 3) {
			TotalChecks[ID] += Total;
			SameASID[ID] += Same;
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeSameASIDStats(char *file) {
	ReadSameASIDStats(file);

	char LogDir[STR_MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, STR_MAX_LEN, "%s/stats/%s_same_asid_stats.txt", LogDirEnv, file);
	assert(len < STR_MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(TotalChecks[i] > 0) {
   			fprintf(fptr_d, "%u %llu %llu\n", i, SameASID[i], TotalChecks[i]);
   		}
   	}
   	fclose(fptr_d);
}

struct Check {
	unsigned long long ID1;
	unsigned long long ID2;
};

/*struct LoopDetails {
	unsigned CheckCount = 0;
	Check checks[CHECK_LEN];
} LoopLog[MAX_LEN];

unsigned NumChecks[MAX_LEN] = {0};

unsigned CurChecksCount = 0;
Check CurChecks[CUR_CHECK_LEN];

extern "C" void dynamicCheck(unsigned LoopID, unsigned checkCount, unsigned long long ID1, unsigned long long ID2) {
	assert(LoopID < MAX_LEN);
	assert(checkCount > 0);
	assert(CurChecksCount < CUR_CHECK_LEN);
	assert(CurChecksCount < checkCount);
	if(NumChecks[LoopID] == 0) {
		NumChecks[LoopID] = checkCount;
	}
	assert(NumChecks[LoopID] > 0);
	//assert(ID1 < MAXALLOCAID && ID2 < MAXALLOCAID);
	CurChecks[CurChecksCount].ID1 = ID1;
	CurChecks[CurChecksCount].ID2 = ID2;
	CurChecksCount++;
}

extern "C" void storeDynamicChecks(unsigned LoopID) {
	assert(LoopID < MAX_LEN);
	assert(NumChecks[LoopID] > 0);
	assert(NumChecks[LoopID] == CurChecksCount);
	assert((LoopLog[LoopID].CheckCount % NumChecks[LoopID]) == 0);

	unsigned i = 0;
	bool found = false;
	while(i < LoopLog[LoopID].CheckCount) {
		bool TempChecks[NumChecks[LoopID]] = {false};
		unsigned TempChecksCount = 0;

		for(unsigned j = 0; j < NumChecks[LoopID]; j++) {
			unsigned k = i;
			
			while(k < i + NumChecks[LoopID]) {
				if(CurChecks[j].ID1 == LoopLog[LoopID].checks[k].ID1 && 
				   CurChecks[j].ID2 == LoopLog[LoopID].checks[k].ID2) {
					if(!TempChecks[j]) {
						TempChecks[j] = true;
						TempChecksCount++;
						break;
					}
				}
				else if(CurChecks[j].ID1 == LoopLog[LoopID].checks[k].ID2 && 
						CurChecks[j].ID2 == LoopLog[LoopID].checks[k].ID1) {
					if(!TempChecks[j]) {
						TempChecks[j] = true;
						TempChecksCount++;
						break;
					}
				}
				k++;
			}
		}
		
		if(TempChecksCount == NumChecks[LoopID]) {
			found = true;
			break;
		}

		i += NumChecks[LoopID];
	}
	if(!found) {
		for(unsigned i = 0; i < CurChecksCount; i++) {
			LoopLog[LoopID].checks[LoopLog[LoopID].CheckCount].ID1 = CurChecks[i].ID1;
			LoopLog[LoopID].checks[LoopLog[LoopID].CheckCount].ID2 = CurChecks[i].ID2;
			LoopLog[LoopID].CheckCount++;
		}
		assert(LoopLog[LoopID].CheckCount < CHECK_LEN);
		assert(LoopLog[LoopID].CheckCount % NumChecks[LoopID] == 0);
	}
	CurChecksCount = 0;
}

void ReadLoopDynamicChecksLog(char *file) {
	char LogDir[STR_MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, STR_MAX_LEN, "%s/stats/%s_dynamic_checks.txt", LogDirEnv, file);
	assert(len < STR_MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		int rec = 0;
		unsigned ID;
		unsigned Individual;
		unsigned Total;
		Check ReadChecks[CHECK_LEN];
		unsigned ReadChecksCount = 0;

		for (char *p = strtok(line, " "); p != NULL; p = strtok(NULL, " ")) {
			switch (rec) {
				case 0:	ID = strtol(p, NULL, 10);
						break;
				case 1:	Individual = strtol(p, NULL, 10);
						break;
				case 2:	Total = strtol(p, NULL, 10);
						break;
				case 3: ReadChecks[ReadChecksCount].ID1 = strtoll(p, NULL, 10);
						break;
				case 4: ReadChecks[ReadChecksCount].ID2 = strtoll(p, NULL, 10);
						ReadChecksCount++;
						rec = 2;
						break;
			}
			rec++;
		}
		
		assert(ReadChecksCount == Total);
		if(NumChecks[ID] > 0) {
			assert(NumChecks[ID] == Individual);
		}
		assert(Total % Individual == 0);

		if(NumChecks[ID] > 0) {
			Check MissingChecks[Total];
			unsigned MissingChecksCount = 0;
			unsigned i = 0;
			while(i < Total) {
				unsigned j = 0;
				bool found = false;
				while(j < LoopLog[ID].CheckCount) {
					bool TempChecks[NumChecks[ID]] = {false};
					unsigned TempChecksCount = 0;

					for(unsigned l = 0; l < NumChecks[ID]; l++) {
						unsigned k = j;
						
						while(k < j + NumChecks[ID]) {
							if(ReadChecks[i + l].ID1 == LoopLog[ID].checks[k].ID1 && 
							   ReadChecks[i + l].ID2 == LoopLog[ID].checks[k].ID2) {
								if(!TempChecks[l]) {
									TempChecks[l] = true;
									TempChecksCount++;
									break;
								}
							}
							else if(ReadChecks[i + l].ID1 == LoopLog[ID].checks[k].ID2 && 
									ReadChecks[i + l].ID2 == LoopLog[ID].checks[k].ID1) {
								if(!TempChecks[l]) {
									TempChecks[l] = true;
									TempChecksCount++;
									break;
								}
							}
							k++;
						}
					}
					
					if(TempChecksCount == NumChecks[ID]) {
						found = true;
						break;
					}
					j += NumChecks[ID];
				}
				if(!found) {
					for(unsigned k = 0; k < NumChecks[ID]; k++) {
						MissingChecks[MissingChecksCount].ID1 = ReadChecks[i + k].ID1;
						MissingChecks[MissingChecksCount].ID2 = ReadChecks[i + k].ID2;
						MissingChecksCount++;
					}
				}
				i += NumChecks[ID];
			}

			for(unsigned i = 0; i < MissingChecksCount; i++) {
				LoopLog[ID].checks[LoopLog[ID].CheckCount].ID1 = MissingChecks[i].ID1;
				LoopLog[ID].checks[LoopLog[ID].CheckCount].ID2 = MissingChecks[i].ID2;
				LoopLog[ID].CheckCount++;
			}
			assert(LoopLog[ID].CheckCount < CHECK_LEN);
		}
		else {
			assert(Individual > 0 && Total > 0);
			assert(LoopLog[ID].CheckCount == 0);
			NumChecks[ID] = Individual;
			for(unsigned i = 0; i < ReadChecksCount; i++) {
				LoopLog[ID].checks[LoopLog[ID].CheckCount].ID1 = ReadChecks[i].ID1;
				LoopLog[ID].checks[LoopLog[ID].CheckCount].ID2 = ReadChecks[i].ID2;
				LoopLog[ID].CheckCount++;
			}
			assert(LoopLog[ID].CheckCount < CHECK_LEN);
			assert(LoopLog[ID].CheckCount == Total);
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeLoopDynamicChecksLog(char *file) {
	ReadLoopDynamicChecksLog(file);

	char LogDir[STR_MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, STR_MAX_LEN, "%s/stats/%s_dynamic_checks.txt", LogDirEnv, file);
	assert(len < STR_MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(NumChecks[i] > 0) {
   			fprintf(fptr_d, "%u %u %u", i, NumChecks[i], LoopLog[i].CheckCount);
   			for(unsigned j = 0; j < LoopLog[i].CheckCount; j++) {
   				fprintf(fptr_d, " %llu %llu", LoopLog[i].checks[j].ID1, LoopLog[i].checks[j].ID2);
   			}
   			fprintf(fptr_d, "\n");
   		}
   	}
   	fclose(fptr_d);
}*/

extern "C" bool isNoAliasASIDCheck(unsigned long long ID1, unsigned long long ID2) {
	if(ID1 == 0) {
		return false;
	}
	if(ID2 == 0) {
		return false;
	}
	return ID1 != ID2;
}

extern "C" void *rmalloc(size_t size, unsigned long long RID) {
	void *Ptr = malloc(size);
	void *Prev_Ptr = Ptr;
	size_t slot_size = return_size(Ptr);
	if(slot_size != max_globaladdr) {
		double shift = log2(slot_size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		*(unsigned long long *)Ptr = RID;
	}
	return Prev_Ptr;
}

extern "C" void *rcalloc(size_t num, size_t size, unsigned long long RID) {
	void *Ptr = calloc(num, size);
	void *Prev_Ptr = Ptr;
	size_t slot_size = return_size(Ptr);
	if(slot_size!= max_globaladdr) {
		double shift = log2(slot_size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		*(unsigned long long *)Ptr = RID;
	}
	return Prev_Ptr;
}

extern "C" void *rrealloc(void *p, size_t size, unsigned long long RID) {
	void *Ptr = realloc(p, size);
	void *Prev_Ptr = Ptr;
	size_t slot_size = return_size(Ptr);
	if(slot_size != max_globaladdr) {
		double shift = log2(slot_size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		*(unsigned long long *)Ptr = RID;
	}
	return Prev_Ptr;
}

extern "C" void *raligned_alloc(size_t alignment, size_t size, unsigned long long RID) {
	void *Ptr = aligned_alloc(alignment, size);
	void *Prev_Ptr = Ptr;
	size_t slot_size = return_size(Ptr);
	if(slot_size != max_globaladdr) {
		double shift = log2(slot_size);	
		uintptr_t P1 = (uintptr_t)Ptr & ~((1ULL << (unsigned long long)shift) - 1);
		Ptr = (uintptr_t*)P1;
		*(unsigned long long *)Ptr = RID;
	}
	return Prev_Ptr;
}

unsigned long long MACount[MAX_LEN] = {0};

extern "C" void computeSameMACount(unsigned long long ID) {
   	MACount[ID]++;
}

void ReadSameMACount(char *file) {
	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
 
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		int ID;
		unsigned long long count;
		if(sscanf(line, "%d %llu", &ID, &count) == 2) {
			MACount[ID] += count;
		}
	}

   	fclose(fptr_d);
}

extern "C" void writeSameMACount(char *file) {
	ReadSameMACount(file);

	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(MACount[i] > 0) {
   			fprintf(fptr_d, "%d %llu\n", i, MACount[i]);
   		}
   	}
   	fclose(fptr_d);
}

struct LoopCount {
	unsigned ID;
	unsigned Count = 0;
	unsigned long long IndCount = 0;
} LoopCountStruct[MAX_LEN];

extern "C" void updateLoopReached(unsigned ID) {
	LoopCountStruct[ID].Count++;
}

extern "C" void updateLoopIterations(unsigned ID) {
	LoopCountStruct[ID].IndCount++;
}

void ReadLoopIter(char *file) {
	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	if(access(LogDir, F_OK) == -1) {
		return;
	}

	FILE *fptr_d = fopen(LogDir, "r");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}

   	char *line = NULL;
	size_t flen = 0;
	ssize_t read;
	while ((read = getline(&line, &flen, fptr_d)) != -1) {
		unsigned ID;
		unsigned count;
		unsigned long long indcount;
		unsigned long long aver;
		if(sscanf(line, "%u %u %llu %llu", &ID, &count, &indcount, &aver) == 4) {
			LoopCountStruct[ID].Count += count;
			LoopCountStruct[ID].IndCount += indcount;
		}
	}
   	fclose(fptr_d);
}

extern "C" void writeLoopIterStats(char *file) {
	ReadLoopIter(file);

	char LogDir[MAX_LEN] = {'\0'};
	struct passwd *pw = getpwuid(getuid());
	const char *LogDirEnv = pw->pw_dir;
	assert(LogDirEnv);
	int len = snprintf(LogDir, MAX_LEN, "%s/stats/%s.txt", LogDirEnv, file);
	assert(len < MAX_LEN);
	LogDir[len] = '\0';

	FILE *fptr_d = fopen(LogDir, "w");
	if (fptr_d == NULL) {
    	printf("Error in opening stats file!");
    	exit(1);
   	}
   	
   	for(unsigned i = 0; i < MAX_LEN; i++) {
   		if(LoopCountStruct[i].Count > 0) {
   			long long aver = (long long)(((long long)LoopCountStruct[i].IndCount) / (long long)LoopCountStruct[i].Count);
   			fprintf(fptr_d, "%u %u %llu %llu\n", i, LoopCountStruct[i].Count, LoopCountStruct[i].IndCount, aver);
   		}
   	}
   	fclose(fptr_d);
}

