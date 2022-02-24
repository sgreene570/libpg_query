/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - AllocSetDeleteFreeList
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * aset.c
 *	  Allocation set definitions.
 *
 * AllocSet is our standard implementation of the abstract MemoryContext
 * type.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/aset.c
 *
 * NOTE:
 *	This is a new (Feb. 05, 1999) implementation of the allocation set
 *	routines. AllocSet...() does not use OrderedSet...() any more.
 *	Instead it manages allocations in a block pool by itself, combining
 *	many small allocations in a few bigger blocks. AllocSetFree() normally
 *	doesn't free() memory really. It just add's the free'd area to some
 *	list for later reuse by AllocSetAlloc(). All memory blocks are free()'d
 *	at once on AllocSetReset(), which happens when the memory context gets
 *	destroyed.
 *				Jan Wieck
 *
 *	Performance improvement from Tom Lane, 8/99: for extremely large request
 *	sizes, we do want to be able to give the memory back to free() as soon
 *	as it is pfree()'d.  Otherwise we risk tying up a lot of memory in
 *	freelist entries that might never be usable.  This is specially needed
 *	when the caller is repeatedly repalloc()'ing a block bigger and bigger;
 *	the previous instances of the block were guaranteed to be wasted until
 *	AllocSetReset() under the old way.
 *
 *	Further improvement 12/00: as the code stood, request sizes in the
 *	midrange between "small" and "large" were handled very inefficiently,
 *	because any sufficiently large free chunk would be used to satisfy a
 *	request, even if it was much larger than necessary.  This led to more
 *	and more wasted space in allocated chunks over time.  To fix, get rid
 *	of the midrange behavior: we now handle only "small" power-of-2-size
 *	chunks as chunks.  Anything "large" is passed off to malloc().  Change
 *	the number of freelists to change the small/large boundary.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"

/*--------------------
 * Chunk freelist k holds chunks of size 1 << (k + ALLOC_MINBITS),
 * for k = 0 .. ALLOCSET_NUM_FREELISTS-1.
 *
 * Note that all chunks in the freelists have power-of-2 sizes.  This
 * improves recyclability: we may waste some space, but the wasted space
 * should stay pretty constant as requests are made and released.
 *
 * A request too large for the last freelist is handled by allocating a
 * dedicated block from malloc().  The block still has a block header and
 * chunk header, but when the chunk is freed we'll return the whole block
 * to malloc(), not put it on our freelists.
 *
 * CAUTION: ALLOC_MINBITS must be large enough so that
 * 1<<ALLOC_MINBITS is at least MAXALIGN,
 * or we may fail to align the smallest chunks adequately.
 * 8-byte alignment is enough on all currently known machines.
 *
 * With the current parameters, request sizes up to 8K are treated as chunks,
 * larger requests go into dedicated blocks.  Change ALLOCSET_NUM_FREELISTS
 * to adjust the boundary point; and adjust ALLOCSET_SEPARATE_THRESHOLD in
 * memutils.h to agree.  (Note: in contexts with small maxBlockSize, we may
 * set the allocChunkLimit to less than 8K, so as to avoid space wastage.)
 *--------------------
 */

#define ALLOC_MINBITS		3	/* smallest chunk size is 8 bytes */
#define ALLOCSET_NUM_FREELISTS	11
#define ALLOC_CHUNK_LIMIT	(1 << (ALLOCSET_NUM_FREELISTS-1+ALLOC_MINBITS))
/* Size of largest chunk that we use a fixed size for */
#define ALLOC_CHUNK_FRACTION	4
/* We allow chunks to be at most 1/4 of maxBlockSize (less overhead) */

/*--------------------
 * The first block allocated for an allocset has size initBlockSize.
 * Each time we have to allocate another block, we double the block size
 * (if possible, and without exceeding maxBlockSize), so as to reduce
 * the bookkeeping load on malloc().
 *
 * Blocks allocated to hold oversize chunks do not follow this rule, however;
 * they are just however big they need to be to hold that single chunk.
 *
 * Also, if a minContextSize is specified, the first block has that size,
 * and then initBlockSize is used for the next one.
 *--------------------
 */

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	sizeof(struct AllocChunkData)

typedef struct AllocBlockData *AllocBlock;	/* forward reference */
typedef struct AllocChunkData *AllocChunk;

/*
 * AllocPointer
 *		Aligned pointer which may be a member of an allocation set.
 */
typedef void *AllocPointer;

/*
 * AllocSetContext is our standard implementation of MemoryContext.
 *
 * Note: header.isReset means there is nothing for AllocSetReset to do.
 * This is different from the aset being physically empty (empty blocks list)
 * because we will still have a keeper block.  It's also different from the set
 * being logically empty, because we don't attempt to detect pfree'ing the
 * last active chunk.
 */
typedef struct AllocSetContext
{
	MemoryContextData header;	/* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	AllocBlock	blocks;			/* head of list of blocks in this set */
	AllocChunk	freelist[ALLOCSET_NUM_FREELISTS];	/* free chunk lists */
	/* Allocation parameters for this context: */
	Size		initBlockSize;	/* initial block size */
	Size		maxBlockSize;	/* maximum block size */
	Size		nextBlockSize;	/* next block size to allocate */
	Size		allocChunkLimit;	/* effective chunk size limit */
	AllocBlock	keeper;			/* keep this block over resets */
	/* freelist this context could be put in, or -1 if not a candidate: */
	int			freeListIndex;	/* index in context_freelists[], or -1 */
} AllocSetContext;

typedef AllocSetContext *AllocSet;

/*
 * AllocBlock
 *		An AllocBlock is the unit of memory that is obtained by aset.c
 *		from malloc().  It contains one or more AllocChunks, which are
 *		the units requested by palloc() and freed by pfree().  AllocChunks
 *		cannot be returned to malloc() individually, instead they are put
 *		on freelists by pfree() and re-used by the next palloc() that has
 *		a matching request size.
 *
 *		AllocBlockData is the header data for a block --- the usable space
 *		within the block begins at the next alignment boundary.
 */
typedef struct AllocBlockData
{
	AllocSet	aset;			/* aset that owns this block */
	AllocBlock	prev;			/* prev block in aset's blocks list, if any */
	AllocBlock	next;			/* next block in aset's blocks list, if any */
	char	   *freeptr;		/* start of free space in this block */
	char	   *endptr;			/* end of space in this block */
}			AllocBlockData;

/*
 * AllocChunk
 *		The prefix of each piece of memory in an AllocBlock
 *
 * Note: to meet the memory context APIs, the payload area of the chunk must
 * be maxaligned, and the "aset" link must be immediately adjacent to the
 * payload area (cf. GetMemoryChunkContext).  We simplify matters for this
 * module by requiring sizeof(AllocChunkData) to be maxaligned, and then
 * we can ensure things work by adding any required alignment padding before
 * the "aset" field.  There is a static assertion below that the alignment
 * is done correctly.
 */
typedef struct AllocChunkData
{
	/* size is always the size of the usable space in the chunk */
	Size		size;
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	/* this is zero in a free chunk */
	Size		requested_size;

#define ALLOCCHUNK_RAWSIZE  (SIZEOF_SIZE_T * 2 + SIZEOF_VOID_P)
#else
#define ALLOCCHUNK_RAWSIZE  (SIZEOF_SIZE_T + SIZEOF_VOID_P)
#endif							/* MEMORY_CONTEXT_CHECKING */

	/* ensure proper alignment by adding padding if needed */
#if (ALLOCCHUNK_RAWSIZE % MAXIMUM_ALIGNOF) != 0
	char		padding[MAXIMUM_ALIGNOF - ALLOCCHUNK_RAWSIZE % MAXIMUM_ALIGNOF];
#endif

	/* aset is the owning aset if allocated, or the freelist link if free */
	void	   *aset;
	/* there must not be any padding to reach a MAXALIGN boundary here! */
}			AllocChunkData;

/*
 * Only the "aset" field should be accessed outside this module.
 * We keep the rest of an allocated chunk's header marked NOACCESS when using
 * valgrind.  But note that chunk headers that are in a freelist are kept
 * accessible, for simplicity.
 */
#define ALLOCCHUNK_PRIVATE_LEN	offsetof(AllocChunkData, aset)

/*
 * AllocPointerIsValid
 *		True iff pointer is valid allocation pointer.
 */
#define AllocPointerIsValid(pointer) PointerIsValid(pointer)

/*
 * AllocSetIsValid
 *		True iff set is valid allocation set.
 */
#define AllocSetIsValid(set) PointerIsValid(set)

#define AllocPointerGetChunk(ptr)	\
					((AllocChunk)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))

/*
 * Rather than repeatedly creating and deleting memory contexts, we keep some
 * freed contexts in freelists so that we can hand them out again with little
 * work.  Before putting a context in a freelist, we reset it so that it has
 * only its initial malloc chunk and no others.  To be a candidate for a
 * freelist, a context must have the same minContextSize/initBlockSize as
 * other contexts in the list; but its maxBlockSize is irrelevant since that
 * doesn't affect the size of the initial chunk.
 *
 * We currently provide one freelist for ALLOCSET_DEFAULT_SIZES contexts
 * and one for ALLOCSET_SMALL_SIZES contexts; the latter works for
 * ALLOCSET_START_SMALL_SIZES too, since only the maxBlockSize differs.
 *
 * Ordinarily, we re-use freelist contexts in last-in-first-out order, in
 * hopes of improving locality of reference.  But if there get to be too
 * many contexts in the list, we'd prefer to drop the most-recently-created
 * contexts in hopes of keeping the process memory map compact.
 * We approximate that by simply deleting all existing entries when the list
 * overflows, on the assumption that queries that allocate a lot of contexts
 * will probably free them in more or less reverse order of allocation.
 *
 * Contexts in a freelist are chained via their nextchild pointers.
 */
#define MAX_FREE_CONTEXTS 100	/* arbitrary limit on freelist length */

typedef struct AllocSetFreeList
{
	int			num_free;		/* current list length */
	AllocSetContext *first_free;	/* list header */
} AllocSetFreeList;

/* context_freelists[0] is for default params, [1] for small params */


/*
 * These functions implement the MemoryContext API for AllocSet contexts.
 */
static void *AllocSetAlloc(MemoryContext context, Size size);
static void AllocSetFree(MemoryContext context, void *pointer);
static void *AllocSetRealloc(MemoryContext context, void *pointer, Size size);
static void AllocSetReset(MemoryContext context);
static void AllocSetDelete(MemoryContext context);
static Size AllocSetGetChunkSpace(MemoryContext context, void *pointer);
static bool AllocSetIsEmpty(MemoryContext context);
static void AllocSetStats(MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);

#ifdef MEMORY_CONTEXT_CHECKING
static void AllocSetCheck(MemoryContext context);
#endif

/*
 * This is the virtual function table for AllocSet contexts.
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif


/* ----------
 * AllocSetFreeIndex -
 *
 *		Depending on the size of an allocation compute which freechunk
 *		list of the alloc set it belongs to.  Caller must have verified
 *		that size <= ALLOC_CHUNK_LIMIT.
 * ----------
 */
#ifdef HAVE__BUILTIN_CLZ
#else
#endif


/*
 * Public routines
 */


/*
 * AllocSetContextCreateInternal
 *		Create a new AllocSet context.
 *
 * parent: parent context, or NULL if top-level context
 * name: name of context (must be statically allocated)
 * minContextSize: minimum context size
 * initBlockSize: initial allocation block size
 * maxBlockSize: maximum allocation block size
 *
 * Most callers should abstract the context size parameters using a macro
 * such as ALLOCSET_DEFAULT_SIZES.
 *
 * Note: don't call this directly; go through the wrapper macro
 * AllocSetContextCreate.
 */


/*
 * AllocSetReset
 *		Frees all memory which is allocated in the given set.
 *
 * Actually, this routine has some discretion about what to do.
 * It should mark all allocated chunks freed, but it need not necessarily
 * give back all the resources the set owns.  Our actual implementation is
 * that we give back all but the "keeper" block (which we must keep, since
 * it shares a malloc chunk with the context header).  In this way, we don't
 * thrash malloc() when a context is repeatedly reset after small allocations,
 * which is typical behavior for per-tuple contexts.
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef CLOBBER_FREED_MEMORY
#else
#endif
#ifdef CLOBBER_FREED_MEMORY
#endif

/*
 * AllocSetDelete
 *		Frees all memory which is allocated in the given set,
 *		in preparation for deletion of the set.
 *
 * Unlike AllocSetReset, this *must* free all resources of the set.
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef CLOBBER_FREED_MEMORY
#endif

/*
 * AllocSetAlloc
 *		Returns pointer to allocated memory of given size or NULL if
 *		request could not be completed; memory is added to the set.
 *
 * No request may exceed:
 *		MAXALIGN_DOWN(SIZE_MAX) - ALLOC_BLOCKHDRSZ - ALLOC_CHUNKHDRSZ
 * All callers use a much-lower limit.
 *
 * Note: when using valgrind, it doesn't matter how the returned allocation
 * is marked, as mcxt.c will set it to UNDEFINED.  In some paths we will
 * return space that is marked NOACCESS - AllocSetRealloc has to beware!
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef RANDOMIZE_ALLOCATED_MEMORY
#endif

/*
 * AllocSetFree
 *		Frees allocated memory; memory is removed from the set.
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef CLOBBER_FREED_MEMORY
#endif
#ifdef CLOBBER_FREED_MEMORY
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#endif

/*
 * AllocSetRealloc
 *		Returns new pointer to allocated memory of given size or NULL if
 *		request could not be completed; this memory is added to the set.
 *		Memory associated with given pointer is copied into the new memory,
 *		and the old memory is freed.
 *
 * Without MEMORY_CONTEXT_CHECKING, we don't know the old request size.  This
 * makes our Valgrind client requests less-precise, hazarding false negatives.
 * (In principle, we could use VALGRIND_GET_VBITS() to rediscover the old
 * request size.)
 */
#ifdef MEMORY_CONTEXT_CHECKING
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY
#endif
#ifdef USE_VALGRIND
#endif
#else							/* !MEMORY_CONTEXT_CHECKING */
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#ifdef RANDOMIZE_ALLOCATED_MEMORY
#endif
#else							/* !MEMORY_CONTEXT_CHECKING */
#endif
#ifdef MEMORY_CONTEXT_CHECKING
#else
#endif

/*
 * AllocSetGetChunkSpace
 *		Given a currently-allocated chunk, determine the total space
 *		it occupies (including all memory-allocation overhead).
 */


/*
 * AllocSetIsEmpty
 *		Is an allocset empty of any allocated space?
 */


/*
 * AllocSetStats
 *		Compute stats about memory consumption of an allocset.
 *
 * printfunc: if not NULL, pass a human-readable stats string to this.
 * passthru: pass this pointer through to printfunc.
 * totals: if not NULL, add stats about this context into *totals.
 * print_to_stderr: print stats to stderr if true, elog otherwise.
 */



#ifdef MEMORY_CONTEXT_CHECKING

/*
 * AllocSetCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */


#endif							/* MEMORY_CONTEXT_CHECKING */

void
AllocSetDeleteFreeList(MemoryContext context)
{
	AllocSet set = (AllocSet) context;
	if (set->freeListIndex >= 0)
	{
		AllocSetFreeList *freelist = &context_freelists[set->freeListIndex];

		while (freelist->first_free != NULL)
		{
			AllocSetContext *oldset = freelist->first_free;

			freelist->first_free = (AllocSetContext *) oldset->header.nextchild;
			freelist->num_free--;

			/* All that remains is to free the header/initial block */
			free(oldset);
		}
		Assert(freelist->num_free == 0);
	}
}
