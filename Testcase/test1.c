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
	unsigned long *a = (unsigned long *)malloc(ntimes * sizeof(unsigned long));
	unsigned long *b = (unsigned long *)malloc(ntimes * sizeof(unsigned long));
	unsigned long *c = (unsigned long *)malloc(ntimes * sizeof(unsigned long));
	unsigned long *size = (unsigned long *)malloc(sizeof(unsigned long));
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
