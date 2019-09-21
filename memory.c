#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <pthread.h>
#include "memory.h"

typedef unsigned long long ulong64;
#define MAGIC_ADDR 0x12abcdef

#define SEGMENT_SIZE (4ULL << 32)
#define PAGE_SIZE 4096
#define METADATA_SIZE ((SEGMENT_SIZE/PAGE_SIZE) * 2)
#define NUM_PAGES_IN_SEG (METADATA_SIZE/2)
#define OTHER_METADATA_SIZE ((METADATA_SIZE/PAGE_SIZE) * 2)
#define COMMIT_SIZE PAGE_SIZE
#define Align(x, y) (((x) + (y-1)) & ~(y-1))
#define ADDR_TO_PAGE(x) (char*)(((ulong64)(x)) & ~(PAGE_SIZE-1))
#define ADDR_TO_SEGMENT(x) (Segment*)(((ulong64)(x)) & ~(SEGMENT_SIZE-1))
#define FREE 1
#define MARK 2
#define GC_THRESHOLD (32ULL << 20)

long long NumGCTriggered = 0;
long long NumBytesFreed = 0;
long long NumBytesAllocated = 0;
extern char  etext, edata, end;

struct OtherMetadata
{
	char *AllocPtr;
	char *CommitPtr;
	char *ReservePtr;
	char *DataPtr;
	int BigAlloc;
};

typedef struct Segment
{
	union
	{
		unsigned short Size[NUM_PAGES_IN_SEG];
		struct OtherMetadata Other;
	};
} Segment;

typedef struct SegmentList
{
	struct Segment *Segment;
	struct SegmentList *Next;
} SegmentList;

typedef struct ObjHeader
{
	unsigned Size;
	unsigned Status;
	ulong64 Type;
} ObjHeader;

#define OBJ_HEADER_SIZE (sizeof(ObjHeader))


static SegmentList *Segments = NULL;

static void setAllocPtr(Segment *Seg, char *Ptr) { Seg->Other.AllocPtr = Ptr; }
static void setCommitPtr(Segment *Seg, char *Ptr) { Seg->Other.CommitPtr = Ptr; }
static void setReservePtr(Segment *Seg, char *Ptr) { Seg->Other.ReservePtr = Ptr; }
static void setDataPtr(Segment *Seg, char *Ptr) { Seg->Other.DataPtr = Ptr; }
static char* getAllocPtr(Segment *Seg) { return Seg->Other.AllocPtr; }
static char* getCommitPtr(Segment *Seg) { return Seg->Other.CommitPtr; }
static char* getReservePtr(Segment *Seg) { return Seg->Other.ReservePtr; }
static char* getDataPtr(Segment *Seg) { return Seg->Other.DataPtr; }
static void setBigAlloc(Segment *Seg, int BigAlloc) { Seg->Other.BigAlloc = BigAlloc; }
static int getBigAlloc(Segment *Seg) { return Seg->Other.BigAlloc; }
static void myfree(void *Ptr);
static void checkAndRunGC();

static void addToSegmentList(Segment *Seg)
{
	SegmentList *L = malloc(sizeof(SegmentList));
	if (L == NULL)
	{
		printf("Unable to allocate list node\n");
		exit(0);
	}
	L->Segment = Seg;
	L->Next = Segments;
	Segments = L;
}

static void allowAccess(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);

	int Ret = mprotect(Ptr, Size, PROT_READ|PROT_WRITE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

static Segment* allocateSegment(int BigAlloc)
{
	void* Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (Base == MAP_FAILED)
	{
		printf("unable to allocate a segment\n");
		exit(0);
	}

	/* segments are aligned to segment size */
	Segment *Segment = (struct Segment*)Align((ulong64)Base, SEGMENT_SIZE);
	allowAccess(Segment, METADATA_SIZE);

	char *AllocPtr = (char*)Segment + METADATA_SIZE;
	char *ReservePtr = (char*)Segment + SEGMENT_SIZE;
	setAllocPtr(Segment, AllocPtr);
	setReservePtr(Segment, ReservePtr);
	setCommitPtr(Segment, AllocPtr);
	setDataPtr(Segment, AllocPtr);
	setBigAlloc(Segment, BigAlloc);
	addToSegmentList(Segment);
	return Segment;
}

static void extendCommitSpace(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	char *ReservePtr = getReservePtr(Seg);
	char *NewCommitPtr = CommitPtr + COMMIT_SIZE;

	assert(AllocPtr == CommitPtr);
	if (NewCommitPtr <= ReservePtr)
	{
		allowAccess(CommitPtr, COMMIT_SIZE);
		setCommitPtr(Seg, NewCommitPtr);
	}
	else
	{
		assert(CommitPtr == ReservePtr);
	}
}

static unsigned short* getSizeMetadata(char *Ptr)
{
	char *Page = ADDR_TO_PAGE(Ptr);
	Segment *Seg = ADDR_TO_SEGMENT(Ptr);
	ulong64 PageNo = (Page - (char*)Seg)/ PAGE_SIZE;
	return &Seg->Size[PageNo];
}

static void createHole(Segment *Seg)
{
	char *AllocPtr = getAllocPtr(Seg);
	char *CommitPtr = getCommitPtr(Seg);
	size_t HoleSz = CommitPtr - AllocPtr;
	if (HoleSz > 0)
	{
		assert(HoleSz >= 8);
		ObjHeader *Header = (ObjHeader*)AllocPtr;
		Header->Size = HoleSz;
		Header->Status = 0;
		setAllocPtr(Seg, CommitPtr);
		myfree(AllocPtr + OBJ_HEADER_SIZE);
		NumBytesFreed -= HoleSz;
	}
}

static void reclaimMemory(void *Ptr, size_t Size)
{
	assert((Size % PAGE_SIZE) == 0);
	assert(((ulong64)Ptr & (PAGE_SIZE-1)) == 0);
	
	int Ret = mprotect(Ptr, Size, PROT_NONE);
	if (Ret == -1)
	{
		printf("unable to mprotect %s():%d\n", __func__, __LINE__);
		exit(0);
	}
	Ret = madvise(Ptr, Size, MADV_DONTNEED);
	if (Ret == -1)
	{
		printf("unable to reclaim physical page %s():%d\n", __func__, __LINE__);
		exit(0);
	}
}

/* used by the GC to free objects. */
static void myfree(void *Ptr)
{
	ObjHeader *Header = (ObjHeader*)((char*)Ptr - OBJ_HEADER_SIZE);
	assert((Header->Status & FREE) == 0);
	NumBytesFreed += Header->Size;

	if (Header->Size > COMMIT_SIZE)
	{
		assert((Header->Size % PAGE_SIZE) == 0);
		assert(((ulong64)Header & (PAGE_SIZE-1)) == 0);
		size_t Size = Header->Size;
		char *Start = (char*)Header;
		size_t Iter;
		for (Iter = 0; Iter < Size; Iter += PAGE_SIZE)
		{
			unsigned short *SzMeta = getSizeMetadata((char*)Start + Iter);
			SzMeta[0] = PAGE_SIZE;
		}
		Header->Status = FREE;
		reclaimMemory(Header, Header->Size);
		return;
	}

	unsigned short *SzMeta = getSizeMetadata((char*)Header);
	SzMeta[0] += Header->Size;
	assert(SzMeta[0] <= PAGE_SIZE);
	Header->Status = FREE;
	if (SzMeta[0] == PAGE_SIZE)
	{
		char *Page = ADDR_TO_PAGE(Ptr);
		reclaimMemory(Page, PAGE_SIZE);
	}
}

static void* BigAlloc(size_t Size)
{
	size_t AlignedSize = Align(Size + OBJ_HEADER_SIZE, PAGE_SIZE);
	assert(AlignedSize <= SEGMENT_SIZE - METADATA_SIZE);
	static Segment *CurSeg = NULL;
	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(1);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	char *ReservePtr = getReservePtr(CurSeg);
	if (NewAllocPtr > ReservePtr)
	{
		CurSeg = allocateSegment(1);
		return BigAlloc(Size);
	}
	assert(AllocPtr == CommitPtr);
	allowAccess(CommitPtr, AlignedSize);
	setAllocPtr(CurSeg, NewAllocPtr);
	setCommitPtr(CurSeg, NewAllocPtr);

	unsigned short *SzMeta = getSizeMetadata(AllocPtr);
	SzMeta[0] = 1;

	ObjHeader *Header = (ObjHeader*)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}


void *_mymalloc(size_t Size)
{
	size_t AlignedSize = Align(Size, 8) + OBJ_HEADER_SIZE;

	NumBytesAllocated += AlignedSize;
	checkAndRunGC(AlignedSize);
	if (AlignedSize > COMMIT_SIZE)
	{
		return BigAlloc(Size);
	}
	assert(Size != 0);
	assert(sizeof(struct OtherMetadata) <= OTHER_METADATA_SIZE);
	assert(sizeof(struct Segment) == METADATA_SIZE);

	static Segment *CurSeg = NULL;

	if (CurSeg == NULL)
	{
		CurSeg = allocateSegment(0);
	}
	char *AllocPtr = getAllocPtr(CurSeg);
	char *CommitPtr = getCommitPtr(CurSeg);
	char *NewAllocPtr = AllocPtr + AlignedSize;
	if (NewAllocPtr > CommitPtr)
	{
		if (AllocPtr != CommitPtr)
		{
			/* Free remaining space on this page */
			createHole(CurSeg);
		}
		extendCommitSpace(CurSeg);
		AllocPtr = getAllocPtr(CurSeg);
		NewAllocPtr = AllocPtr + AlignedSize;
		CommitPtr = getCommitPtr(CurSeg);
		if (NewAllocPtr > CommitPtr)
		{
			CurSeg = allocateSegment(0);
			return _mymalloc(Size);
		}
	}

	setAllocPtr(CurSeg, NewAllocPtr);
	ObjHeader *Header = (ObjHeader*)AllocPtr;
	Header->Size = AlignedSize;
	Header->Status = 0;
	Header->Type = 0;
	return AllocPtr + OBJ_HEADER_SIZE;
}

/* scan and mark objects in the scanner list.
 * add newly encountered objects to the scanner list.
 */
void scanner()
{
}

/* Free all unmarked objects. */
void sweep()
{
}

/* walk all 4-byte aligned addresses.
 * add valid objects to the scanner list for 
 * mark and scan.
 */
static void scanRoots(unsigned *Top, unsigned *Bottom)
{
}

void _runGC()
{
	NumGCTriggered++;
	unsigned *DataStart = (unsigned*)Align(((ulong64)&etext), 4);
	unsigned *DataEnd = (unsigned*)((ulong64)(&edata) - 7);

	/* scan global variables */
	scanRoots(DataStart, DataEnd);

	unsigned *UnDataStart = (unsigned*)Align(((ulong64)&edata), 4);
	unsigned *UnDataEnd = (unsigned*)((ulong64)(&end) - 7);

	/* scan uninitialized global variables */
	scanRoots(UnDataStart, UnDataEnd);

	
	int Lvar;
	void *Base;
	size_t Size;
	pthread_attr_t Attr;
	
	int Ret = pthread_getattr_np(pthread_self(), &Attr);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	Ret = pthread_attr_getstack(&Attr , &Base, &Size);
	if (Ret != 0)
	{
		printf("Error getting stackinfo\n");
		return;
	}
	unsigned *Bottom = (unsigned*)(Base + Size - 7);
	unsigned *Top = (unsigned*)&Lvar;
	Top = (unsigned*)Align((ulong64)Top, 4);
	/* skip GC stack frame */
	while (Top[0] != MAGIC_ADDR)
	{
		assert(Top < Bottom);
		Top++;
	}
	/* scan application stack */
	scanRoots(Top, Bottom);

	scanner();
	sweep();
}

static void checkAndRunGC(size_t Sz)
{
	static size_t TotalAlloc = 0;

	TotalAlloc += Sz;
	if (TotalAlloc < GC_THRESHOLD)
	{
		return;
	}
	TotalAlloc = 0;
	_runGC();
}

void printMemoryStats()
{
	printf("Num Bytes Allocated: %lld\n", NumBytesAllocated);
	printf("Num Bytes Freed: %lld\n", NumBytesFreed);
	printf("Num GC Triggered: %lld\n", NumGCTriggered);
}
