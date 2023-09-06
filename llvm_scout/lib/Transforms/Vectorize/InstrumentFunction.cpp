#include <pwd.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_CACHE_ENTRIES 32
#define LARGE_MAGIC 0xdeadfacedeaddeed
#define SEGMENT_SIZE (1ULL << 32)
#define MAX_LEN 30000
#define STACK_LEN 1024

#define ADDR_TO_SEGMENT(x) (unsigned long long*)(((unsigned long long)(x)) & ~(SEGMENT_SIZE-1))

//size_t max_globaladdr = 0xFFFFFFFFFFFFFFFF;
size_t max_globaladdr = 0xFFFFFFFF;

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

extern "C" void ReadLoopCounter(char *file) {
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

extern "C" void ReadTimeStats(char *file) {
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

extern "C" void ReadFuncStats(char *file) {
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