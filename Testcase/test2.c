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

unsigned long *__attribute__((noinline,memoryallocwrapperfunc)) imalloc(unsigned long size) {
	unsigned long *a = (unsigned long *)malloc(size * sizeof(unsigned long));
	return a;
}

void __attribute__((noinline)) dummy(unsigned long *a) {}

int main() {
	unsigned long *a = imalloc(ntimes);
	unsigned long *b = imalloc(ntimes);
	unsigned long *c = imalloc(ntimes);
	unsigned long *size = imalloc(1);
	*size = ntimes;

	for(unsigned long i = 0; i < *size; i++) {
		a[i] = i;
		b[i] = i;
		c[i] = i;
	}

	for(unsigned long i = 0; i < 2 * ntimes; i++) {
		foo(a, b, c, size);
	}

  dummy(a);
	/*for(unsigned long i = 0; i < *size; i++) {
		printf("%lu %lu\n", i, a[i]);
	}*/

	free(a);
	free(b);
	free(c);
	free(size);
	return 0;
}
