#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/reg.h> // needed for __WORDSIZE in musl
#include "tlsf.h"

// Detect whether or not we are building for a 32- or 64-bit architecture
#if __WORDSIZE == 64
#  define TLSF_64BIT
#elif !defined(__WORDSIZE)
#  error __WORDSIZE is not defined
#endif

/*
 * log2 of number of linear subdivisions of block sizes. Larger
 * values require more memory in the control structure. Values of
 * 4 or 5 are typical.
 */
#define SL_INDEX_COUNT_SHIFT 5

// All allocation sizes and addresses are aligned.
#ifdef TLSF_64BIT
#define ALIGN_SIZE_SHIFT 3
#else
#define ALIGN_SIZE_SHIFT 2
#endif

#define ALIGN_SIZE (1UL << ALIGN_SIZE_SHIFT)

/*
 * We support allocations of sizes up to (1 << FL_INDEX_MAX) bits.
 * However, because we linearly subdivide the second-level lists, and
 * our minimum size granularity is 4 bytes, it doesn't make sense to
 * create first-level lists for sizes smaller than SL_INDEX_COUNT * 4,
 * or (1 << (SL_INDEX_COUNT_SHIFT + 2)) bytes, as there we will be
 * trying to split size ranges into more slots than we have available.
 * Instead, we calculate the minimum threshold size, and place all
 * blocks below that size into the 0th first-level list.
 *
 * We can increase this to support larger sizes, at the expense
 * of more overhead in the TLSF structure.
 *
*/
#ifdef TLSF_64BIT
#define FL_INDEX_MAX 33 // 8G
#else
#define FL_INDEX_MAX 29 // 1G
#endif

#define SL_INDEX_COUNT (1U << SL_INDEX_COUNT_SHIFT)
#define FL_INDEX_SHIFT (SL_INDEX_COUNT_SHIFT + ALIGN_SIZE_SHIFT)
#define FL_INDEX_COUNT (FL_INDEX_MAX - FL_INDEX_SHIFT + 1)

#define SMALL_BLOCK_SIZE (1U << FL_INDEX_SHIFT)

/*
 * The size of the block header exposed to used blocks is the size field.
 * The prev_phys_block field is stored *inside* the previous free block.
*/
#define BLOCK_OVERHEAD (sizeof (size_t))
#define POOL_OVERHEAD  (2 * BLOCK_OVERHEAD)

// User data starts directly after the size field in a used block.
#define BLOCK_START_OFFSET (offsetof(struct block_s, header) + sizeof (size_t))

/*
 * A free block must be large enough to store its header minus the size of
 * the prev_phys_block field, and no larger than the number of addressable
 * bits for FL_INDEX.
*/
#define BLOCK_SIZE_MIN (sizeof (struct block_s) - sizeof (block_t))
#define BLOCK_SIZE_MAX (1UL << FL_INDEX_MAX)

/*
 * Size of the TLSF structures in a given memory block passed to
 * tlsf_create, equal to the size of a tlsf_s
 */
#define TLSF_SIZE  (sizeof (struct tlsf_s))

// Return location of next block after block of given size.
#define OFFSET_TO_BLOCK(p, s) ((block_t)((char*)(p) + (s)))

#ifdef TLSF_ASSERT
#define ASSERT(cond, msg) assert((cond) && msg)
#else
#define ASSERT(cond, msg)
#endif

#define INSIST(cond, msg)                                           \
  do {                                                              \
    if (!(cond)) {                                                  \
      fprintf(stderr, "TLSF FAILURE: %s - %s\n", msg, #cond);  \
      abort();                                                      \
    }                                                               \
  } while (0)

// This code has been tested on 32- and 64-bit (LP/LLP) architectures.
_Static_assert(sizeof(int) * 8 == 32, "Integer must be 32 bit");
_Static_assert(sizeof(size_t) * 8 >= 32, "size_t must be 32 bit or more");
_Static_assert(sizeof(size_t) * 8 <= 64, "size_t must be 64 bit or less");
_Static_assert(sizeof(unsigned int) * 8 >= SL_INDEX_COUNT, "SL_INDEX_COUNT must be <= number of bits in sl_bitmap's storage type.");
_Static_assert(ALIGN_SIZE == SMALL_BLOCK_SIZE / SL_INDEX_COUNT, "Ensure we've properly tuned our sizes.");

/*
 * Data structures and associated constants.
*/

/*
 * Block header structure.
 *
 * There are several implementation subtleties involved:
 * - The prev_phys_block field is only valid if the previous block is free.
 * - The prev_phys_block field is actually stored at the end of the
 *   previous block. It appears at the beginning of this structure only to
 *   simplify the implementation.
 * - The next_free / prev_free fields are only valid if the block is free.
*/
typedef struct block_s {
  // Points to the previous physical block.
  struct block_s* prev_phys_block;

  // The size of this block and header bits
  union {
    size_t header;
    struct {
      size_t size         : __WORDSIZE - 3;
      bool   is_free      : 1;
      bool   is_prev_free : 1;
      bool   is_pool      : 1;
    };
  };

  // Next and previous free blocks.
  struct block_s* next_free;
  struct block_s* prev_free;
} *block_t;

// The TLSF control structure.
struct tlsf_s {
  // Bitmaps for free lists.
  unsigned int fl_bitmap;
  unsigned int sl_bitmap[FL_INDEX_COUNT];

  // Empty lists point at this block to indicate they are free.
  struct block_s block_null;

  // Head of free lists.
  block_t blocks[FL_INDEX_COUNT][SL_INDEX_COUNT];

#ifdef TLSF_STATS
  tlsf_stats_t stats;
#endif

  tlsf_map_t   map;
  tlsf_unmap_t unmap;
  void*        user;
};

/*
 * TLSF achieves O(1) cost for malloc and free operations by limiting
 * the search for a free block to a free list of guaranteed size
 * adequate to fulfill the request, combined with efficient free list
 * queries using bitmasks and bit-manipulation routines.
 *
 * NOTE: TLSF spec relies on ffs/fls returning value 0..31.
 * ffs/fls return 1-32 by default, returning 0 for error.
 */

static inline unsigned int ffs(unsigned int x) {
  unsigned int i = (unsigned int)__builtin_ffs((int)x);
  ASSERT(i, "No set bit found");
  return i - 1U;
}

static inline unsigned int flsl(size_t x) {
  return (unsigned int)(x ? 8 * sizeof (size_t) - (unsigned int)__builtin_clzl(x) - 1 : 0);
}

/*
 * block_t member functions.
*/

static inline bool block_is_last(const block_t block) {
  return block->size == 0;
}

static inline block_t block_from_ptr(void* ptr) {
  return OFFSET_TO_BLOCK(ptr, -BLOCK_START_OFFSET);
}

static inline void* block_to_ptr(const block_t block) {
  return (void*)((char*)block + BLOCK_START_OFFSET);
}

// Return location of previous block.
static inline block_t block_prev(const block_t block) {
  ASSERT(block->is_prev_free, "previous block must be free");
  return block->prev_phys_block;
}

// Return location of next existing block.
static inline block_t block_next(const block_t block) {
  block_t next = OFFSET_TO_BLOCK(block_to_ptr(block), block->size - BLOCK_OVERHEAD);
  ASSERT(!block_is_last(block), "Block is last");
  return next;
}

// Link a new block with its physical neighbor, return the neighbor.
static inline block_t block_link_next(block_t block) {
  block_t next = block_next(block);
  next->prev_phys_block = block;
  return next;
}

static inline bool block_can_split(block_t block, size_t size) {
  return block->size >= sizeof (struct block_s) + size;
}

static inline void block_set_free(block_t block, bool free) {
  ASSERT(free ? !block->is_free : block->is_free, "Block is already free/used");
  block->is_free = free;
  block_link_next(block)->is_prev_free = free;
}

static inline size_t align_up(size_t x) {
  return (x + (ALIGN_SIZE - 1)) & ~(ALIGN_SIZE - 1);
}

static inline void* align_ptr(const void* ptr) {
  return (void*)align_up((size_t)ptr);
}

/*
 * Adjust an allocation size to be aligned to word size, and no smaller
 * than internal minimum.
*/
static inline size_t adjust_size(size_t size) {
  size = align_up(size);
  if (size < BLOCK_SIZE_MIN)
    return BLOCK_SIZE_MIN;
  INSIST(size < BLOCK_SIZE_MAX, "too large size");
  return size;
}

/*
 * TLSF utility functions. In most cases, these are direct translations of
 * the documentation found in the white paper.
*/

static void mapping_insert(size_t size, unsigned int *fli, unsigned int *sli) {
  unsigned int fl, sl;
  if (size < SMALL_BLOCK_SIZE) {
    // Store small blocks in first list.
    fl = 0;
    sl = (unsigned int)size / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT);
  } else {
    fl = flsl(size);
    sl = (unsigned int)(size >> (fl - SL_INDEX_COUNT_SHIFT)) ^
      (1UL << SL_INDEX_COUNT_SHIFT);
    fl -= (FL_INDEX_SHIFT - 1);
  }
  *fli = fl;
  *sli = sl;
}

// This version rounds up to the next block size (for allocations)
static void mapping_search(size_t size, unsigned int *fli, unsigned int *sli) {
  if (size >= SMALL_BLOCK_SIZE) {
    const size_t round = (1UL << (flsl(size) - SL_INDEX_COUNT_SHIFT)) - 1;
    size += round;
  }
  mapping_insert(size, fli, sli);
}

static block_t search_suitable_block(tlsf_t t, unsigned int *fli, unsigned int *sli) {
  unsigned int fl = *fli;
  unsigned int sl = *sli;

  /*
   * First, search for a block in the list associated with the given
   * fl/sl index.
   */
  unsigned int sl_map = t->sl_bitmap[fl] & (~0U << sl);
  if (!sl_map) {
    // No block exists. Search in the next largest first-level list.
    const unsigned int fl_map = t->fl_bitmap & (~0U << (fl + 1));
    // No free blocks available, memory has been exhausted.
    if (!fl_map)
      return 0;

    *fli = fl = ffs(fl_map);
    sl_map = t->sl_bitmap[fl];
  }
  ASSERT(sl_map, "Second level bitmap is null");
  *sli = sl = ffs(sl_map);

  // Return the first block in the free list.
  return t->blocks[fl][sl];
}

// Remove a free block from the free list.
static void remove_free_block(tlsf_t t, block_t block, unsigned int fl, unsigned int sl) {
  block_t prev = block->prev_free;
  block_t next = block->next_free;
  ASSERT(prev, "prev_free field can not be null");
  ASSERT(next, "next_free field can not be null");
  next->prev_free = prev;
  prev->next_free = next;

  // If this block is the head of the free list, set new head.
  if (t->blocks[fl][sl] == block) {
    t->blocks[fl][sl] = next;

    // If the new head is null, clear the bitmap.
    if (next == &t->block_null) {
      t->sl_bitmap[fl] &= ~(1U << sl);

      // If the second bitmap is now empty, clear the fl bitmap.
      if (!t->sl_bitmap[fl])
        t->fl_bitmap &= ~(1U << fl);
    }
  }

#ifdef TLSF_STATS
  ASSERT(t->stats.free_size >= block->size, "wrong free");
  t->stats.free_size -= block->size;
  t->stats.used_size += block->size;
#endif
}

// Insert a free block into the free block list.
static void insert_free_block(tlsf_t t, block_t block, unsigned int fl, unsigned int sl) {
  block_t current = t->blocks[fl][sl];
  ASSERT(current, "free list cannot have a null entry");
  ASSERT(block, "cannot insert a null entry into the free list");
  block->next_free = current;
  block->prev_free = &t->block_null;
  current->prev_free = block;

  ASSERT(block_to_ptr(block) == align_ptr(block_to_ptr(block)), "block not aligned properly");
  /*
   * Insert the new block at the head of the list, and mark the first-
   * and second-level bitmaps appropriately.
  */
  t->blocks[fl][sl] = block;
  t->fl_bitmap |= (1U << fl);
  t->sl_bitmap[fl] |= (1U << sl);

#ifdef TLSF_STATS
  ASSERT(t->stats.used_size >= block->size, "wrong used");
  t->stats.free_size += block->size;
  t->stats.used_size -= block->size;
#endif
}

// Remove a given block from the free list.
static void block_remove(tlsf_t t, block_t block) {
  unsigned int fl, sl;
  mapping_insert(block->size, &fl, &sl);
  remove_free_block(t, block, fl, sl);
}

// Insert a given block into the free list.
static void block_insert(tlsf_t t, block_t block) {
  unsigned int fl, sl;
  mapping_insert(block->size, &fl, &sl);
  insert_free_block(t, block, fl, sl);
}

// Split a block into two, the second of which is free.
static block_t block_split(block_t block, size_t size) {
  // Calculate the amount of space left in the remaining block.
  block_t remaining = OFFSET_TO_BLOCK(block_to_ptr(block), size - BLOCK_OVERHEAD);

  const size_t remain_size = block->size - (size + BLOCK_OVERHEAD);

  ASSERT(block_to_ptr(remaining) == align_ptr(block_to_ptr(remaining)), "remaining block not aligned properly");
  ASSERT(block->size == remain_size + size + BLOCK_OVERHEAD, "remaining block size is wrong");
  ASSERT(remain_size >= BLOCK_SIZE_MIN, "block split with invalid size");

  remaining->size = remain_size;
  remaining->is_pool = false;
  remaining->is_free = false;
  remaining->is_prev_free = false;

  block_set_free(remaining, true);
  block->size = size;

  return remaining;
}

// Absorb a free block's storage into an adjacent previous free block.
static block_t block_absorb(block_t prev, block_t block) {
  ASSERT(!block_is_last(prev), "previous block can't be last");
  prev->size += block->size + BLOCK_OVERHEAD;
  block_link_next(prev);
  return prev;
}

// Merge a just-freed block with an adjacent previous free block.
static block_t block_merge_prev(tlsf_t t, block_t block) {
  if (block->is_prev_free) {
    block_t prev = block_prev(block);
    ASSERT(prev, "prev physical block can't be null");
    ASSERT(prev->is_free, "prev block is not free though marked as such");
    block_remove(t, prev);
    block = block_absorb(prev, block);
  }

  return block;
}

// Merge a just-freed block with an adjacent free block.
static block_t block_merge_next(tlsf_t t, block_t block) {
  block_t next = block_next(block);
  ASSERT(next, "next physical block can't be null");

  if (next->is_free) {
    ASSERT(!block_is_last(block), "previous block can't be last");
    block_remove(t, next);
    block = block_absorb(block, next);
  }

  return block;
}

// Trim any trailing block space off the end of a block, return to pool.
static void block_trim_free(tlsf_t t, block_t block, size_t size) {
  ASSERT(block->is_free, "block must be free");
  if (block_can_split(block, size)) {
    block_t remaining = block_split(block, size);
    block_link_next(block);
    remaining->is_prev_free = true;
    block_insert(t, remaining);
  }
}

// Trim any trailing block space off the end of a used block, return to pool.
static void block_trim_used(tlsf_t t, block_t block, size_t size) {
  ASSERT(!block->is_free, "block must be used");
  if (block_can_split(block, size)) {
    // If the next block is free, we must coalesce.
    block_t remaining = block_split(block, size);
    remaining->is_prev_free = false;

    remaining = block_merge_next(t, remaining);
    block_insert(t, remaining);
  }
}

// Locate a free block with an appropriate size.
static block_t block_locate_free(tlsf_t t, size_t size) {
  unsigned int fl = 0, sl = 0;
  mapping_search(size, &fl, &sl);
  block_t block = search_suitable_block(t, &fl, &sl);
  if (block) {
    ASSERT(block->size >= size, "insufficient block size");
    remove_free_block(t, block, fl, sl);
  }
  return block;
}

/*
 * Overhead of the TLSF structures in a given memory block passed to
 * add_pool, equal to the overhead of a free block and the
 * sentinel block.
*/

static block_t add_pool(tlsf_t t, void* mem, size_t size) {
  const size_t pool_size = size - POOL_OVERHEAD;

  ASSERT((size_t)mem % ALIGN_SIZE == 0, "wrong alignment");
  ASSERT(pool_size >= BLOCK_SIZE_MIN && pool_size <= BLOCK_SIZE_MAX, "wrong pool size");

#ifdef TLSF_STATS
  ++t->stats.pool_count;
  t->stats.total_size += pool_size;
  t->stats.used_size += pool_size;
#endif

  /*
   * Create the main free block. Offset the start of the block slightly
   * so that the prev_phys_block field falls outside of the pool -
   * it will never be used.
   */
  block_t block = OFFSET_TO_BLOCK(mem, -BLOCK_OVERHEAD);
  block->size = pool_size;
  block->is_free = true;
  block->is_prev_free = false;
  block->is_pool = false;
  block_insert(t, block);

  // Split the block to create a zero-size sentinel block.
  block_t sentinel = block_link_next(block);
  sentinel->size = 0;
  sentinel->is_free = false;
  sentinel->is_prev_free = true;
  sentinel->is_pool = false;

  return block;
}

static void remove_pool(tlsf_t t, block_t block) {
  size_t size = block->size;

#ifdef TLSF_STATS
  ASSERT(t->stats.used_size >= size, "wrong used");
  ASSERT(t->stats.total_size >= size, "wrong total");
  ASSERT(t->stats.pool_count > 0, "wrong pool count");
  t->stats.total_size -= size;
  t->stats.used_size -= size;
  --t->stats.pool_count;
#endif

  ASSERT(block_next(block)->size == 0, "sentinel should have size 0");
  ASSERT(!block_next(block)->is_free, "sentinel block should not be free");
  t->unmap((char*)block + BLOCK_OVERHEAD, size + POOL_OVERHEAD, t->user);
}

/*
 * TLSF main interface.
*/

tlsf_t tlsf_create(tlsf_map_t map, tlsf_unmap_t unmap, void* user) {
  ASSERT(map, "map must not be null");
  size_t minsize = TLSF_SIZE + POOL_OVERHEAD + BLOCK_OVERHEAD;
  size_t size = minsize;
  void* mem = map(&size, user);
  ASSERT(mem, "no memory available");
  ASSERT(size >= minsize, "not enough memory allocated");
  ASSERT((size_t)mem % ALIGN_SIZE == 0, "wrong alignment");

  tlsf_t t = (tlsf_t)mem;
  t->map = map;
  t->unmap = unmap;
  t->user = user;

#ifdef TLSF_STATS
  memset(&t->stats, 0, sizeof (t->stats));
#endif

  t->block_null.next_free = &t->block_null;
  t->block_null.prev_free = &t->block_null;

  t->fl_bitmap = 0;
  for (unsigned int i = 0; i < FL_INDEX_COUNT; ++i) {
    t->sl_bitmap[i] = 0;
    for (unsigned int j = 0; j < SL_INDEX_COUNT; ++j)
      t->blocks[i][j] = &t->block_null;
  }

  add_pool(t, (char*)mem + TLSF_SIZE, size - TLSF_SIZE);

  return t;
}

void tlsf_destroy(tlsf_t t) {
#ifdef TLSF_STATS
  ASSERT((t->unmap && t->stats.pool_count == 1) || (!t->unmap && t->stats.pool_count >= 1), "Memory leak detected. Some pools were not released.");
  ASSERT(t->stats.free_size == t->stats.total_size, "Memory leak detected.");
#endif

  if (t->unmap) {
    block_t first_block = OFFSET_TO_BLOCK(t, TLSF_SIZE  - BLOCK_OVERHEAD);
    ASSERT(block_next(first_block)->size == 0, "sentinel should have size 0");
    ASSERT(!block_next(first_block)->is_free, "sentinel block should not be free");
    t->unmap(t, TLSF_SIZE + first_block->size + POOL_OVERHEAD, t->user);
  }
}

void* tlsf_malloc(tlsf_t t, size_t size) {
  size = adjust_size(size);

  block_t block = block_locate_free(t, size);
  if (!block) {
    size_t minsize = POOL_OVERHEAD + BLOCK_OVERHEAD + size;
    size_t memsize = minsize;
    void* mem = t->map(&memsize, t->user);
    if (!mem)
      return 0;
    ASSERT(memsize >= minsize, "not enough memory allocated");
    add_pool(t, (char*)mem, memsize)->is_pool = true;
    block = block_locate_free(t, size);
  }
  ASSERT(block, "No block found");

#ifdef TLSF_STATS
  ++t->stats.malloc_count;
#endif

  block_trim_free(t, block, size);
  block_set_free(block, false);
  return block_to_ptr(block);
}

void tlsf_free(tlsf_t t, void* mem) {
  if (!mem) // to support free after zero size realloc
    return;

  block_t block = block_from_ptr(mem);
  ASSERT(!block->is_free, "block already marked as free");

#ifdef TLSF_STATS
  ++t->stats.free_count;
#endif

  block_set_free(block, true);
  block = block_merge_prev(t, block);
  block = block_merge_next(t, block);

  if (block->is_pool && block_next(block)->size == 0 && t->unmap)
    remove_pool(t, block);
  else
    block_insert(t, block);
}

/*
 * The TLSF block information provides us with enough information to
 * provide a reasonably intelligent implementation of realloc, growing or
 * shrinking the currently allocated block as required.
 *
 * This routine handles the somewhat esoteric edge cases of realloc:
 * - a non-zero size with a null pointer will behave like malloc
 * - a zero size with a non-null pointer will behave like free
 * - a request that cannot be satisfied will leave the original buffer
 *   untouched
 * - an extended buffer size will leave the newly-allocated area with
 *   contents undefined
 */
void* tlsf_realloc(tlsf_t t, void* mem, size_t size) {
  // Zero-size requests are treated as free.
  if (mem && size == 0) {
    tlsf_free(t, mem);
    return 0;
  }

  // Requests with NULL pointers are treated as malloc.
  if (!mem)
    return tlsf_malloc(t, size);

  block_t block = block_from_ptr(mem);
  block_t next = block_next(block);

  const size_t cursize = block->size;
  const size_t combined = cursize + next->size + BLOCK_OVERHEAD;
  size = adjust_size(size);

  ASSERT(!block->is_free, "block already marked as free");

  /*
   * If the next block is used, or when combined with the current
   * block, does not offer enough space, we must reallocate and copy.
   */
  if (size > cursize && (!next->is_free || size > combined)) {
    void* p = tlsf_malloc(t, size);
    if (p) {
      memcpy(p, mem, cursize);
      tlsf_free(t, mem);
    }
    return p;
  }

  // Do we need to expand to the next block?
  if (size > cursize) {
    block_merge_next(t, block);
    block_next(block)->is_prev_free = false;
  }

  // Trim the resulting block and return the original pointer.
  block_trim_used(t, block, size);
  return mem;
}

void* tlsf_calloc(tlsf_t t, size_t size) {
  void* p = tlsf_malloc(t, size);
  if (p)
    memset(p, 0, size);
  return p;
}

#ifdef TLSF_STATS
const tlsf_stats_t* tlsf_stats(tlsf_t t) {
  return &t->stats;
}

void tlsf_printstats(tlsf_t t) {
  tlsf_stats_t* s = &t->stats;
  fprintf(stderr, "TSLF free_size=%lu used_size=%lu total_size=%lu pool_count=%lu malloc_count=%lu free_count=%lu\n",
          s->free_size, s->used_size, s->total_size, s->pool_count, s->malloc_count, s->free_count);
}
#endif

/*
 * Debugging utilities.
 */
#ifdef TLSF_DEBUG
void tlsf_check(tlsf_t t) {
  // Check that the free lists and bitmaps are accurate.
  for (unsigned int i = 0; i < FL_INDEX_COUNT; ++i) {
    for (unsigned int j = 0; j < SL_INDEX_COUNT; ++j) {
      const unsigned int fl_map = t->fl_bitmap & (1U << i);
      const unsigned int sl_list = t->sl_bitmap[i];
      const unsigned int sl_map = sl_list & (1U << j);
      block_t block = t->blocks[i][j];

      // Check that first- and second-level lists agree.
      if (!fl_map)
        INSIST(!sl_map, "second-level map must be null");

      if (!sl_map) {
        INSIST(block == &t->block_null, "block list must be null");
        continue;
      }

      // Check that there is at least one free block.
      INSIST(sl_list, "no free blocks in second-level map");
      INSIST(block != &t->block_null, "block should not be null");

      while (block != &t->block_null) {
        unsigned int fli, sli;
        INSIST(block->is_free, "block should be free");
        INSIST(!block->is_prev_free, "blocks should have coalesced");
        INSIST(!block_next(block)->is_free, "blocks should have coalesced");
        INSIST(block_next(block)->is_prev_free, "block should be free");
        INSIST(block->size >= BLOCK_SIZE_MIN, "block not minimum size");

        mapping_insert(block->size, &fli, &sli);
        INSIST(fli == i && sli == j, "block size indexed in wrong list");
        block = block->next_free;
      }
    }
  }
  INSIST(t->stats.free_size + t->stats.used_size == t->stats.total_size, "wrong total memory");
  INSIST(t->stats.free_count <= t->stats.malloc_count, "wrong free and malloc count");
}
#endif // TLSF_DEBUG
