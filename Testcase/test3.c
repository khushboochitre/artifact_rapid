#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define ntimes 80000

void __attribute__((noinline)) foo(unsigned long *a, unsigned long *b, unsigned long *c, unsigned long *size) {
	for(unsigned long i = 0; i < *size; i++) {
		a[i] = b[i] + c[i];
	}
}

void __attribute__((noinline)) dummy(unsigned long *a) {}

int main() {
	unsigned long *size = (unsigned long *)malloc(sizeof(unsigned long));
  *size = ntimes; 

  unsigned long *a[3];
  #pragma clang loop unroll(disable)
	for(unsigned long i = 0; i < 3; i++) {
    a[i] = (unsigned long *)malloc(ntimes * sizeof(unsigned long));
	}

  for(unsigned long i = 0; i < 3; i++) {
		for(unsigned long j = 0; j < *size; j++) {
			a[i][j] = j;
		}
	}

	for(unsigned long i = 0; i < 2 * ntimes; i++) {
		foo(a[0], a[1], a[2], size);
	}

  dummy(a[0]);
	/*for(unsigned long i = 0; i < *size; i++) {
		printf("%lu %lu\n", i, a[i]);
	}*/

	for(unsigned long i = 0; i < 3; i++) {
		free(a[i]);
	}
  free(size);
	return 0;
}
