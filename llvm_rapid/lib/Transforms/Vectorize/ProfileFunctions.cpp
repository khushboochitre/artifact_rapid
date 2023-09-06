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

#define MAX_LEN 30000
#define CHECK_LEN 1000
#define STR_MAX_LEN 1000
#define CUR_CHECK_LEN 50

struct Check {
	unsigned long long ID1;
	unsigned long long ID2;
};

struct LoopDetails {
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
}
