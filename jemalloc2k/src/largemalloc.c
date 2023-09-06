#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <elf.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>

typedef unsigned long long ulong64;

#define CONTAINS_SIZE(x) (((size_t)(x) & 8) == 8)
#define GET_SIZE(x) (*(((size_t*)(x)) - 1))
#define SET_SIZE(x, y) (*((size_t*)(x)) = (y))

#define SEGMENT_SIZE (1ULL << 32)
#define PAGE_SIZE 4096
#define BUMP_SIZE PAGE_SIZE
#define Align(x, y) (((x) + (y-1)) & ~(y-1))
#define ADDR_TO_PAGE(x) (char*)(((ulong64)(x)) & ~(PAGE_SIZE-1))
#define ADDR_TO_SEGMENT(x) (Segment*)(((ulong64)(x)) & ~(SEGMENT_SIZE-1) & 0xFFFFFFFFFFFF)
#define MAX_ULONG 0xFFFFFFFFFFFFFFFFULL
#define LARGE_MAGIC 0xdeadfacedeaddeed
#define MAX_CACHE_SIZE (1ULL << 20)
#define MAX_CACHE_ENTRIES 64
#define MIN_LARGE_OBJECT_SIZE PAGE
#define SYSCALL_THRESHOLD 100

void *MinLargeAddr = (void*)-1ULL;

size_t max_globaladdr = 0xFFFFFFFFFFFFFFFF;

static void *_san_largerealloc(void *Ptr, size_t OldSize, size_t NewSize);

typedef struct Segment
{
	size_t IdxSize;
	size_t AlignmentMask;
	size_t MagicString;
	size_t AlignedSize;
	size_t NumFreePages;
	size_t MaxBitMapIndex;
	int NumMetadataPages;
	int MaxCacheEntries;
	int NumSyscall;
	int CacheOverflow;
	struct Segment *Next;
	size_t *Cache[MAX_CACHE_ENTRIES];
	int NumCacheEntries;
	pthread_mutex_t lock;
	unsigned long long BitMap[];
} Segment;

size_t GetLargeIndex(size_t size)
{
  size_t i;
  for (i = 0; i < 48; i++) {
    if ((1ULL << i) >= size) {
      assert(i <= 31 && i >= 12);
      return i - 12;
    }
  }
	assert(0);
  return -1;
}

struct Segment *Segments[32] = {NULL};
pthread_mutex_t SegmentLock = PTHREAD_MUTEX_INITIALIZER;
size_t TotalCommitMem = 0;
size_t TotalDeCommitMem = 0;

static void printRlimit()
{
	struct rlimit old;
	if (getrlimit(RLIMIT_AS, &old) == -1)
		perror("prlimit-1");

	printf("Previous limits as: soft=%jd; hard=%jd\n", 
		(intmax_t) old.rlim_cur, (intmax_t) old.rlim_max);
}

static void setRlimit()
{
#if 0
	struct rlimit old = {(1ULL << 40), (1ULL<<40)};
	if (setrlimit(RLIMIT_AS, &old) == -1)
		perror("prlimit-1");
#endif
}

static bool allowAccess(void *Ptr, size_t Size)
{
	TotalCommitMem += Size;
	Size = Align(Size, PAGE_SIZE);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);

	int Ret = mprotect(Ptr, Size, PROT_READ|PROT_WRITE);
	//malloc_printf("Allowing: Ptr:%p Size:%zd\n", Ptr, Size);
	if (Ret == -1)
	{
		perror("mprotect :: ");
		printf("unable to mprotect %s():%d TotalComm:%zd TotalDecomm:%zd\n",
			__func__, __LINE__, TotalCommitMem, TotalDeCommitMem);
		printRlimit();
		exit(0);
		return false;
	}
	return true;
}

static Segment* allocateSegment(size_t Idx)
{
	setRlimit();
	void* Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE, MAP_ANON|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
	if (Base == MAP_FAILED)
	{
		printf("unable to allocate a segment\n");
		exit(0);
	}

	/* segments are aligned to segment size */
	Segment *S = (struct Segment*)Align((ulong64)Base, SEGMENT_SIZE);

	if ((void*)S < MinLargeAddr) {
		MinLargeAddr = (void*)S;
	}

	//malloc_printf("SEGMENT: %p-%p\n", S, (char*)S + SEGMENT_SIZE);

	size_t AlignShift = Idx;
	assert(AlignShift < 32);
	size_t Alignment = (1ULL << AlignShift);
	if (Alignment < MIN_LARGE_OBJECT_SIZE) {
		Alignment = PAGE;
	}
	size_t AlignmentMask = ~(Alignment-1);
	AlignmentMask = (AlignmentMask << 15) >> 15;
	size_t BitmapSize = (SEGMENT_SIZE / Alignment) - 1;
	size_t BitmapSizeBytes = (BitmapSize + 7) / 8;

	assert(BitmapSize > 0);

	size_t MetadataSize = BitmapSizeBytes + sizeof(struct Segment);
	bool Ret = allowAccess(S, MetadataSize);
	assert(Ret);
	S->IdxSize = (1ULL << AlignShift);
	S->NumMetadataPages = (BitmapSizeBytes + Alignment - 1) / Alignment;
	S->MagicString = LARGE_MAGIC;
	S->MaxBitMapIndex = BitmapSize;
	S->NumFreePages = BitmapSize;
	S->AlignmentMask = AlignmentMask;
	S->AlignedSize = Alignment;
	S->NumCacheEntries = 0;
	S->MaxCacheEntries = 1;
	S->CacheOverflow = 0;
	S->NumSyscall = 0;
	S->Next = NULL;
	pthread_mutex_init(&S->lock, NULL);
	//malloc_printf("Alloc Seg: %p Idx:%zd\n", S, Idx);

	return S;
}

static void reclaimMemory(void *Ptr, size_t Size)
{
#if 0
	static int advise = 0;
	if (advise == 0) {
		char buf[128];
		readlink("/proc/self/exe", buf, 128);
		buf[127] = '\0';
		if (0 /*strstr(buf, "gcc") || strstr(buf, "perlbench")*/) {
			advise = MADV_DONTNEED;
		}
		else {
			advise = MADV_DONTNEED | MADV_FREE;
		}
	}
#endif
	TotalDeCommitMem += Size;
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);
#if 0
	//malloc_printf("Revoking: Ptr:%p Size:%zd\n", Ptr, Size);
	int Ret = mprotect(Ptr, Size, PROT_NONE);
	//void* Ret = mmap(Ptr, Size, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON, -1, 0);
	if (Ret == -1)
	//if (Ret != Ptr)
	{
		malloc_printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
#endif
	//int Ret1 = msync(Ptr, Size, MS_INVALIDATE);
	int Ret = madvise(Ptr, Size, MADV_DONTNEED);
	//if (Ret1 == -1)
	if (Ret == -1)
	{
		perror("madvise :: ");
		malloc_printf("unable to reclaim physical page %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

static void _san_largefree(void *Ptr)
{
	Segment *Cur = ADDR_TO_SEGMENT(Ptr);
	assert(Cur->MagicString == LARGE_MAGIC);
	size_t AlignedSize = Cur->AlignedSize;

	pthread_mutex_lock(&Cur->lock);
	if (Cur->NumCacheEntries < Cur->MaxCacheEntries && Cur->AlignedSize <= MAX_CACHE_SIZE) {
		Cur->Cache[Cur->NumCacheEntries] = (size_t*)Ptr;
		//((size_t*)Ptr)[0] = Size;
		//malloc_printf("free cache:%zd alined:%zd num:%d\n", Size, AlignedSize, Cur->NumCacheEntries);
		Cur->NumCacheEntries++;
		pthread_mutex_unlock(&Cur->lock);
		return;
	}
	Cur->CacheOverflow = 1;

	pthread_mutex_unlock(&Cur->lock);
	if (CONTAINS_SIZE(Ptr)) {
		size_t Sz = GET_SIZE(Ptr);
		reclaimMemory(Ptr - 8, Sz + 8);
	}
	else {
		reclaimMemory(Ptr, AlignedSize);
	}
	pthread_mutex_lock(&Cur->lock);
	size_t FreeIdx = ((char*)Ptr - (char*)Cur) / Cur->AlignedSize;
	assert(FreeIdx > 0);
	FreeIdx -= Cur->NumMetadataPages;
	//malloc_printf("Free Ptr: %p Idx:%zd Size:%zd\n", Ptr, FreeIdx, Size);
	size_t BitMapIdx = FreeIdx / 64;
	size_t BitIdx = FreeIdx % 64;
	Cur->BitMap[BitMapIdx] &= ~(1ULL << BitIdx);
	Cur->NumFreePages++;
	pthread_mutex_unlock(&Cur->lock);
	//malloc_printf("free:%zd\n", Size);
}

//JEMALLOC_EXPORT
void san_largefree(void *Ptr) {
	_san_largefree(Ptr);
}

static int findFirstFreeIdx(unsigned long long BitVal)
{
	int i = 0;
	for (i = 0; i <= 63; i++) {
		if (((BitVal >> i) & 1) == 0) {
			return i;
		}
	}
	assert(0);
	return -1;
}

static void* UpdateSizeNew(Segment *S, void *Ptr, size_t Size, size_t AlignedSize)
{
	void *Ret = NULL;
	assert(!CONTAINS_SIZE(Ptr));
	size_t NewSize = Align((Size + 8), PAGE_SIZE);
	bool SetSize = (NewSize < AlignedSize) && S->NumSyscall < SYSCALL_THRESHOLD;

	if (SetSize) {
		bool r = allowAccess(Ptr, NewSize);
		if (r) {
			SET_SIZE(Ptr, NewSize - 8);
			Ret = Ptr + 8;
		}
	}
	else {
		bool r = allowAccess(Ptr, AlignedSize);
		if (r) {
			Ret = Ptr;
		}
	}
	return Ret;
}

static void* UpdateSizeOld(Segment *S, void *Ptr, size_t Size, size_t AlignedSize)
{
	void *Ret = Ptr;

	if (!CONTAINS_SIZE(Ptr)) {
		return Ret;
	}
	size_t OldSize = GET_SIZE(Ptr);

	if (OldSize >= Size) {
		return Ret;
	}

	S->NumSyscall++;
	Ptr -= 8;
	return UpdateSizeNew(S, Ptr, Size, AlignedSize);
}

static void* _san_largealloc(size_t Size, int Idx, bool *Zero)
{
	//size_t AlignedSize = Align(Size, PAGE_SIZE);

	Segment *Cur = Segments[Idx];

	while (Cur != NULL && Cur->NumFreePages == 0) {
		Cur = Cur->Next;
	}
	if (Cur == NULL) {
		pthread_mutex_lock(&SegmentLock);
		Cur = Segments[Idx];
		while (Cur != NULL && Cur->NumFreePages == 0) {
			Cur = Cur->Next;
		}
		if (Cur == NULL) {
			Cur = allocateSegment(Idx);
			if (Segments[Idx] != NULL) {
				Cur->Next = Segments[Idx];
			}
			Segments[Idx] = Cur;
		}
		pthread_mutex_unlock(&SegmentLock);
	}

	pthread_mutex_lock(&Cur->lock);
	if (Cur->NumFreePages == 0) {
		pthread_mutex_unlock(&Cur->lock);
		return _san_largealloc(Size, Idx, Zero);
	}

	//assert(Cur->AlignedSize == AlignedSize);
	size_t AlignedSize = Cur->AlignedSize;


	if (Cur->NumCacheEntries) {
		//malloc_printf("cache: Size:%zd AlignedSz:%zd Num:%d\n", Size, AlignedSize, Cur->NumCacheEntries);
		assert(Cur->NumCacheEntries > 0 && Cur->NumCacheEntries <= Cur->MaxCacheEntries);
		assert(Cur->AlignedSize <= MAX_CACHE_SIZE);
		size_t *Ptr = Cur->Cache[Cur->NumCacheEntries -1 ];
		Cur->NumCacheEntries--;
		if (Cur->NumCacheEntries == 0 && Cur->MaxCacheEntries != MAX_CACHE_ENTRIES && Cur->CacheOverflow) {
			if (Cur->MaxCacheEntries * 2 <= MAX_CACHE_ENTRIES) {
				Cur->MaxCacheEntries = Cur->MaxCacheEntries * 2;
			} else {
				Cur->MaxCacheEntries++;
			}
			Cur->CacheOverflow = 0;
			//malloc_printf("MAX_CACHE_ENTRIES:: %d SIZE:%zd\n", Cur->MaxCacheEntries, AlignedSize);
		}

		pthread_mutex_unlock(&Cur->lock);
		if (Size > MIN_LARGE_OBJECT_SIZE) {
			Ptr = UpdateSizeOld(Cur, Ptr, Size, AlignedSize);
			bool Ret = Ptr != NULL;
			if (Ret == false) {
				return _san_largealloc(Size, Idx, Zero);
			}
		}

		return Ptr; //_san_largerealloc(Ptr, Ptr[0], Size);
	}

	size_t FreeIdx = -1;
	size_t NumPages = 0;
	size_t i;

#if 0
	if (Cur->MaxCacheEntries == MAX_CACHE_ENTRIES) {
		if (Cur->MaxCacheEntries >= 64) {
			unsigned long long *BitMap = Cur->BitMap;
			for (i = 0; i < Cur->MaxBitMapIndex; i++) {
				if (BitMap[i] == 0) {
					BitMap[i] = 0xFFFFFFFFFFFFFFFF;
					FreeIdx = i * 64;
					NumPages = 64;
					break;
				}
			}
		}
		else if (Cur->MaxCacheEntries >= 32) {
			unsigned *BitMap = (unsigned*)Cur->BitMap;
			for (i = 0; i < Cur->MaxBitMapIndex; i++) {
				if (BitMap[i] == 0) {
					BitMap[i] = 0xFFFFFFFF;
					FreeIdx = i * 32;
					NumPages = 32;
					break;
				}
			}
		}
		else if (Cur->MaxCacheEntries >= 16) {
			unsigned short *BitMap = (unsigned short*)Cur->BitMap;
			for (i = 0; i < Cur->MaxBitMapIndex; i++) {
				if (BitMap[i] == 0) {
					BitMap[i] = 0xFFFF;
					FreeIdx = i * 16;
					NumPages = 16;
					break;
				}
			}
		}
		else if (Cur->MaxCacheEntries >= 8) {
			//malloc_printf("MaxBitMapIndex:%zd NumFreePages:%zd\n", Cur->MaxBitMapIndex, Cur->NumFreePages);
			unsigned char *BitMap = (unsigned char*)Cur->BitMap;
			for (i = 0; i < Cur->MaxBitMapIndex; i++) {
				if (BitMap[i] == 0) {
					BitMap[i] = 0xFF;
					FreeIdx = i * 8;
					NumPages = 8;
					break;
				}
			}
		}
	}
#endif

	if (FreeIdx == (size_t)-1) {
		//malloc_printf("MaxBitMapIndex:%zd NumFreePages:%zd\n", Cur->MaxBitMapIndex, Cur->NumFreePages);
		for (i = 0; i < Cur->MaxBitMapIndex; i+=64) {
			size_t MapIdx = i / 64;
			if (Cur->BitMap[MapIdx] != MAX_ULONG) {
				FreeIdx = findFirstFreeIdx(Cur->BitMap[MapIdx]);
				Cur->BitMap[MapIdx] |= (1ULL << FreeIdx);
				FreeIdx += i;
				//malloc_printf("FreeIdx: %zd\n", FreeIdx);
				assert(FreeIdx < Cur->MaxBitMapIndex);
				break;
			}
		}
		assert(FreeIdx != (size_t)-1);
		NumPages = 1;
	}

	char *Addr = (char*)(Cur) + ((FreeIdx + Cur->NumMetadataPages) * Cur->AlignedSize);
	Cur->NumFreePages -= NumPages;
	pthread_mutex_unlock(&Cur->lock);

	bool Ret;
	if (Size <= MIN_LARGE_OBJECT_SIZE) {
		Ret = allowAccess(Addr, NumPages * AlignedSize);
	}
	else {
		Addr = UpdateSizeNew(Cur, Addr, Size, AlignedSize);
		Ret = (Addr != NULL);
	}
	if (Ret == false) {
		return _san_largealloc(Size, Idx, Zero);
	}
	if (Zero) {
		*Zero = true;
	}

	if (NumPages > 1) {
		pthread_mutex_lock(&Cur->lock);
		for (i = 0; i < NumPages - 1; i++) {
			Cur->Cache[Cur->NumCacheEntries] = (size_t*)Addr;
			Addr += AlignedSize;
			Cur->NumCacheEntries++;
		}
		pthread_mutex_unlock(&Cur->lock);
	}
	//memset(Addr, 0, AlignedSize);
	//malloc_printf("Large alloc: %p FreeIdx:%zd Idx:%zd Size:%zd AlignedSz:%zd\n", Addr, FreeIdx, Idx, Size, AlignedSize);
	//malloc_printf("%llx Size:%zd AlignedSz:%zd IdxSize:%zd Max:%d\n", Addr, Size, AlignedSize, Cur->IdxSize, Cur->MaxCacheEntries);
	return Addr;
}

void* san_largealloc(size_t Size, int idx, bool *Zero) {
	void *ret = _san_largealloc(Size, idx, Zero);
	return ret;
}

static void* stackalloc(int idx, size_t size)
{
	static Segment *StackSegs[12] = {NULL};
	static int CurIdxs[12] = {0};
	size_t stack_sz = (1ULL << 20);

	if (!StackSegs[idx]) {
		StackSegs[idx] = allocateSegment(idx);
	}

	pthread_mutex_lock(&StackSegs[idx]->lock);
	char *Addr = (char*)(StackSegs[idx]) + PAGE_SIZE + (stack_sz * CurIdxs[idx]);
	CurIdxs[idx]++;
	pthread_mutex_unlock(&StackSegs[idx]->lock);

	bool Ret = allowAccess(Addr, size);
	assert(Ret);
	//reclaimMemory(Addr, size);
	return Addr;
}

__attribute__((visibility(
    "default"))) __thread void* allocptr8 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr8_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr16 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr16_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr32 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr32_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr64 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr64_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr128 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr128_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr256 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr256_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr512 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr512_e = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr1024 = NULL;
__attribute__((visibility(
    "default"))) __thread void* allocptr1024_e = NULL;

static void stackcommit(size_t size, size_t sz) {
	sz = Align(sz, BUMP_SIZE);
	switch (size) {
		case 8: {
			if (allocptr8_e) {
				allowAccess(allocptr8_e, sz);
				allocptr8_e += sz;
			}
			else {
				allocptr8 = stackalloc(3, sz);
				allocptr8_e = allocptr8 + sz;
			}
			break;
		}
		case 16: {
			if (allocptr16_e) {
				allowAccess(allocptr16_e, sz);
				allocptr16_e += sz;
			}
			else {
				allocptr16 = stackalloc(4, sz);
				allocptr16_e = allocptr16 + sz;
			}
			break;
		}
		case 32: {
			if (allocptr32_e) {
				allowAccess(allocptr32_e, sz);
				allocptr32_e += sz;
			}
			else {
				allocptr32 = stackalloc(5, sz);
				allocptr32_e = allocptr32 + sz;
			}
			break;
		}
		case 64: {
			if (allocptr64_e) {
				allowAccess(allocptr64_e, sz);
				allocptr64_e += sz;
			}
			else {
				allocptr64 = stackalloc(6, sz);
				allocptr64_e = allocptr64 + sz;
			}
			break;
		}
		case 128: {
			if (allocptr128_e) {
				allowAccess(allocptr128_e, sz);
				allocptr128_e += sz;
			}
			else {
				allocptr128 = stackalloc(7, sz);
				allocptr128_e = allocptr128 + sz;
			}
			break;
		}
		case 256: {
			if (allocptr256_e) {
				allowAccess(allocptr256_e, sz);
				allocptr256_e += sz;
			}
			else {
				allocptr256 = stackalloc(8, sz);
				allocptr256_e = allocptr256 + sz;
			}
			break;
		}
		case 512: {
			if (allocptr512_e) {
				allowAccess(allocptr512_e, sz);
				allocptr512_e += sz;
			}
			else {
				allocptr512 = stackalloc(9, sz);
				allocptr512_e = allocptr512 + sz;
			}
			break;
		}
		case 1024: {
			if (allocptr1024_e) {
				allowAccess(allocptr1024_e, sz);
				allocptr1024_e += sz;
			}
			else {
				allocptr1024 = stackalloc(10, sz);
				allocptr1024_e = allocptr1024 + sz;
			}
			break;
		}
		default:
			assert(0);
	}
}


JEMALLOC_EXPORT
void je_init_bump_allocator()
{
	static int initialized = 0;
	if (initialized != 1) {
		initialized = 1;
		stackcommit(128, (1ULL << 20));
		stackcommit(256, (1ULL << 20));
		stackcommit(512, (1ULL << 20));
		stackcommit(1024, (1ULL << 20));
	}
}

JEMALLOC_EXPORT
void je_init_bump_allocator1()
{
	stackcommit(8, (1ULL << 20));
	stackcommit(16, (1ULL << 20));
	stackcommit(32, (2ULL << 20));
	stackcommit(64, (1ULL << 20));
	stackcommit(128, (1ULL << 20));
	stackcommit(256, (1ULL << 20));
	stackcommit(512, (1ULL << 20));
	stackcommit(1024, (1ULL << 20));
}

JEMALLOC_EXPORT
void* je_san_stackalloc()
{
	static Segment *StackSeg = NULL;
	static int CurIdx = 0;
	size_t size = (1ULL << 20);
	max_globaladdr = 0xFFFFFFFF;
	if (!StackSeg) {
		StackSeg = allocateSegment(6);
	}

	pthread_mutex_lock(&StackSeg->lock);
	char *Addr = (char*)(StackSeg) + PAGE_SIZE + (size * CurIdx);
	CurIdx++;
	pthread_mutex_unlock(&StackSeg->lock);

	bool Ret = allowAccess(Addr, size);
	assert(Ret);
	je_init_bump_allocator();
	return Addr;
}

JEMALLOC_EXPORT
void* je_stackalloc8(int num_slots) {
	void *ret = allocptr8;
	if (allocptr8 + (8 * num_slots) > allocptr8_e) {
		assert(0);
		stackcommit(8, num_slots * 8);
		ret = allocptr8;
	}
	allocptr8 += (8 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc16(int num_slots) {
	void *ret = allocptr16;
	if (allocptr16 + (16 * num_slots) > allocptr16_e) {
		assert(0);
		stackcommit(16, num_slots * 16);
		ret = allocptr16;
	}
	allocptr16 += (16 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc32(int num_slots) {
	void *ret = allocptr32;
	if (allocptr32 + (32 * num_slots) > allocptr32_e) {
		assert(0);
		stackcommit(32, num_slots * 32);
		ret = allocptr32;
	}
	allocptr32 += (32 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc64(int num_slots) {
	void *ret = allocptr64;
	if (allocptr64 + (64 * num_slots) > allocptr64_e) {
		assert(0);
		stackcommit(64, num_slots * 64);
		ret = allocptr64;
	}
	allocptr64 += (64 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc128(int num_slots) {
	void *ret = allocptr128;
	if (allocptr128 + (128 * num_slots) > allocptr128_e) {
		assert(0);
		stackcommit(128, num_slots * 128);
		ret = allocptr128;
	}
	allocptr128 += (128 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc256(int num_slots) {
	void *ret = allocptr256;
	if (allocptr256 + (256 * num_slots) > allocptr256_e) {
		assert(0);
		stackcommit(256, num_slots * 256);
		ret = allocptr256;
	}
	allocptr256 += (256 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc512(int num_slots) {
	void *ret = allocptr512;
	if (allocptr512 + (512 * num_slots) > allocptr512_e) {
		assert(0);
		stackcommit(512, num_slots * 512);
		ret = allocptr512;
	}
	allocptr512 += (512 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void* je_stackalloc1024(int num_slots) {
	void *ret = allocptr1024;
	if (allocptr1024 + (1024 * num_slots) > allocptr1024_e) {
		assert(0);
		stackcommit(1024, num_slots * 1024);
		ret = allocptr1024;
	}
	allocptr1024 += (1024 * num_slots);
	return ret;
}

JEMALLOC_EXPORT
void je_stackfree16(void *ptr) {
	allocptr16 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree32(void *ptr) {
	allocptr32 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree64(void *ptr) {
	allocptr64 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree128(void *ptr) {
	allocptr128 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree256(void *ptr) {
	allocptr256 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree512(void *ptr) {
	allocptr512 = ptr;
}

JEMALLOC_EXPORT
void je_stackfree1024(void *ptr) {
	allocptr1024 = ptr;
}

static void *_san_largerealloc(void *Ptr, size_t OldSize, size_t NewSize)
{
	size_t OldAlignedSize = Align(OldSize, PAGE_SIZE);
	size_t NewAlignedSize = Align(NewSize, PAGE_SIZE);
	if (NewAlignedSize < OldAlignedSize) {
		reclaimMemory(Ptr + NewAlignedSize, OldAlignedSize - NewAlignedSize);
		return Ptr;
	}
	if (NewAlignedSize == OldAlignedSize) {
		return Ptr;
	}
	Segment *S = ADDR_TO_SEGMENT(Ptr);
	if (S->AlignedSize >= NewAlignedSize) {
		bool Ret = allowAccess(Ptr + OldAlignedSize, NewAlignedSize - OldAlignedSize);
		assert(Ret);
		return Ptr;
	}
	void *NewPtr = _san_largealloc(NewSize, 0, NULL);
	memcpy(NewPtr, Ptr, OldSize);
	//_san_largefree(Ptr, OldSize);
	return NewPtr;
}

static void *san_largerealloc(void *Ptr, size_t OldSize, size_t NewSize)
{
	void *ret = _san_largerealloc(Ptr, OldSize, NewSize);
	return ret;
}

static void *san_largeheader(void *Ptr)
{
	Segment *S = ADDR_TO_SEGMENT(Ptr);
	if ((void*)S < MinLargeAddr) {
		return NULL;
	}
	assert(S->MagicString == LARGE_MAGIC);
	return (void*)((size_t)Ptr & S->AlignmentMask);
}

static size_t san_getsize(void *Ptr)
{
	Segment *S = ADDR_TO_SEGMENT(Ptr);
	assert(S->MagicString == LARGE_MAGIC);
	size_t AlignedSize = S->IdxSize;
	if (AlignedSize > MIN_LARGE_OBJECT_SIZE && CONTAINS_SIZE(Ptr)) {
		assert(((size_t)(Ptr) & (PAGE_SIZE-1)) == 8);
		return GET_SIZE(Ptr);
	}
	return AlignedSize;
}
