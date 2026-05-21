/**
 * @file tests/test_arena.c
 * @brief KavakArena smoke + invariants.
 *
 * Covers the basic contract:
 *   1. init zeroes a fresh arena and stages one chunk.
 *   2. alloc returns 8-byte aligned, zeroed memory by default.
 *   3. alloc_aligned honors caller-specified alignment.
 *   4. allocations cross chunk boundaries cleanly (chunks grow).
 *   5. allocations larger than chunk_size land in dedicated chunks.
 *   6. free clears head/tail and total_used.
 *   7. impossible sizes / invalid alignments are rejected cleanly.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_init_and_free(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 256);
  ASSERT(arena.head != NULL, "init: head allocated");
  ASSERT(arena.head == arena.tail, "init: single chunk");
  ASSERT(arena.chunk_size == 256, "init: chunk_size honored");
  ASSERT(arena.total_used == 0, "init: nothing used");
  kavak_arena_free(&arena);
  ASSERT(arena.head == NULL && arena.tail == NULL, "free: chunks released");
  ASSERT(arena.total_used == 0, "free: counter reset");
  return 0;
}

static int test_default_chunk_size(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 0);
  ASSERT(arena.chunk_size == 4096, "init(0): default 4096");
  kavak_arena_free(&arena);
  return 0;
}

static int test_alloc_alignment_and_zero(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 1024);

  /* Single byte — should still come back 8-aligned. */
  void *p1 = kavak_arena_alloc(&arena, 1);
  ASSERT(p1 != NULL, "alloc(1): non-NULL");
  ASSERT(((uintptr_t)p1 & 7) == 0, "alloc(1): 8-aligned");
  ASSERT(*(unsigned char *)p1 == 0, "alloc(1): zeroed");

  /* Next alloc must be on the next 8-byte boundary, even though we
   * only consumed 1 byte. */
  void *p2 = kavak_arena_alloc(&arena, 4);
  ASSERT((char *)p2 - (char *)p1 == 8, "alloc: 8-byte stride after 1-byte");

  /* 16-byte alignment via alloc_aligned. */
  void *p3 = kavak_arena_alloc_aligned(&arena, 32, 16);
  ASSERT(((uintptr_t)p3 & 15) == 0, "alloc_aligned(16): 16-aligned");

  kavak_arena_free(&arena);
  return 0;
}

static int test_chunk_growth(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 64);
  KavakArenaChunk *first = arena.head;

  /* Drain the first chunk. */
  for (int i = 0; i < 8; ++i) {
    void *p = kavak_arena_alloc(&arena, 8);
    ASSERT(p != NULL, "drain: alloc");
  }
  ASSERT(arena.tail == first, "still on first chunk after exact fill");

  /* Next alloc forces a new chunk. */
  void *spill = kavak_arena_alloc(&arena, 8);
  ASSERT(spill != NULL, "spill: alloc on new chunk");
  ASSERT(arena.tail != first, "spill: tail advanced");
  ASSERT(arena.head == first, "spill: head unchanged");

  kavak_arena_free(&arena);
  return 0;
}

static int test_oversize_alloc(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 64);

  /* Request bigger than chunk_size — must succeed via dedicated chunk. */
  unsigned char *big = kavak_arena_alloc(&arena, 1024);
  ASSERT(big != NULL, "oversize: alloc succeeds");
  ASSERT(((uintptr_t)big & 7) == 0, "oversize: aligned");
  for (size_t i = 0; i < 1024; ++i) {
    ASSERT(big[i] == 0, "oversize: zeroed");
  }

  kavak_arena_free(&arena);
  return 0;
}

static int test_total_used_accounting(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 256);

  kavak_arena_alloc(&arena, 1);     /* 1 byte but 8 reserved (alignment) */
  ASSERT(arena.total_used == 1, "total_used: 1 after first alloc");

  kavak_arena_alloc(&arena, 8);     /* 8 bytes + 7 pad */
  ASSERT(arena.total_used == 1 + 7 + 8, "total_used: tracks pad+size");

  kavak_arena_free(&arena);
  return 0;
}

static int test_overflow_rejected(void) {
  KavakArena arena;
  kavak_arena_init(&arena, 256);
  const size_t before = arena.total_used;

  ASSERT(kavak_arena_alloc(&arena, SIZE_MAX) == NULL,
         "overflow-sized alloc rejected");
  ASSERT(kavak_arena_alloc_aligned(&arena, SIZE_MAX - 8u, 16) == NULL,
         "aligned overflow-sized alloc rejected");
  ASSERT(kavak_arena_alloc_aligned(&arena, 8, 3) == NULL,
         "non-power-of-two alignment rejected");
  ASSERT(arena.total_used == before, "failed allocs leave accounting unchanged");

  kavak_arena_free(&arena);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_init_and_free();
  fails += test_default_chunk_size();
  fails += test_alloc_alignment_and_zero();
  fails += test_chunk_growth();
  fails += test_oversize_alloc();
  fails += test_total_used_accounting();
  fails += test_overflow_rejected();

  if (fails == 0) {
    printf("  ✓ test_arena: 7/7 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_arena: %d failure(s)\n", fails);
  return 1;
}
