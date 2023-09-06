#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MIN_SIZE (1ULL << 14)
int *arr[100000];


int main()
{
	int i;
	size_t sz;
	for (i = 0; i < 100000; i++) {
		sz = (rand() % MIN_SIZE);
		if (rand() % 2 == 0) {
			sz = MIN_SIZE - sz;
		}
		else {
			sz = MIN_SIZE + sz;
		}
		arr[i] = malloc(sz);
		memset(arr[i], 1, sz);
	}
	for (i = 0; i < 100000; i++) {
		assert(arr[i][0] > 0);
		free(arr[i]);
	}
	return 0;
}
