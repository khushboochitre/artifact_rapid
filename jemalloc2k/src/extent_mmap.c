#define JEMALLOC_EXTENT_MMAP_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/extent_mmap.h"

/******************************************************************************/
/* Data. */

bool	opt_retain =
#ifdef JEMALLOC_RETAIN
    true
#else
    false
#endif
    ;

/******************************************************************************/

void* san_largealloc(size_t Size, int idx, bool *Zero);
void san_largefree(void *Ptr);

static int size_to_idx(size_t size)
{
	int i;
	for (i = 3; i < 32; i++) {
		if ((size_t)(1ULL << i) >= size) {
			return i;
		}
	}
	assert(0);
	return 0;
}

void *
extent_alloc_mmap(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit, szind_t szind) {
	assert(alignment == ALIGNMENT_CEILING(alignment, PAGE));

	if (size == PAGE) {
		size_t idxsz = sz_index2size(szind);
		int idx = size_to_idx(idxsz);
		bool need_zero = *zero;
		*zero = false;
		void *ret = san_largealloc(size, idx, zero);
		*commit = true;
		if (need_zero && !*zero) {
			memset(ret, 0, size);
			*zero = true;
		}
		return ret;
	}
	void *ret = pages_map(new_addr, size, alignment, commit);
	if (ret == NULL) {
		return NULL;
	}
	assert(ret != NULL);
	if (*commit) {
	  *zero = true;
	}
	//malloc_printf("map size:%zd szind:%d ret:%p align:%zd commit:%d ment:%zd\n", 
		//size, szind, ret, sz_index2size(szind), *commit, alignment);
	return ret;
}

bool
extent_dalloc_mmap(void *addr, size_t size) {
	san_largefree(addr);
	return false;
//#if 0
	//malloc_printf("unmap addr:%p size:%zd retain:%d\n", addr, size, opt_retain);
	if (!opt_retain) {
		//pages_unmap(addr, size);
	}
	return opt_retain;
//#endif
}
