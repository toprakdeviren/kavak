/**
 * @file src/arena.c
 * @brief KavakArena — bump allocator with growable chunks.
 *
 * Each chunk is calloc'd, so first-pass allocations return zeroed
 * memory. Allocations larger than `chunk_size` get a dedicated
 * chunk sized exactly to fit. Free is one walk over the chunk list.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdlib.h>

#define KAVAK_ARENA_DEFAULT_CHUNK 4096
#define KAVAK_ARENA_DEFAULT_ALIGN 8

struct KavakArenaChunk {
  KavakArenaChunk *next;
  size_t           used;
  size_t           cap;
  /* `data` follows immediately. Calloc-zeroed. data[0] is only
   * guaranteed to be 8-byte aligned (the struct prefix is 24 bytes
   * on LP64 — not a 16-byte multiple), so alloc_aligned does its
   * alignment math on the absolute pointer, not on `used`. */
  unsigned char    data[];
};

static KavakArenaChunk *kavak__chunk_new(const size_t cap) {
  if (cap > SIZE_MAX - sizeof(KavakArenaChunk)) return NULL;
  KavakArenaChunk *chunk = calloc(1, sizeof(*chunk) + cap);
  if (chunk) chunk->cap = cap;
  return chunk;
}

static int kavak__add_overflows_size(const size_t a, const size_t b,
                                     size_t *out) {
  if (a > SIZE_MAX - b) return 1;
  if (out) *out = a + b;
  return 0;
}

static int kavak__align_forward(const uintptr_t base, const size_t align,
                                uintptr_t *out) {
  if (align == 0 || (align & (align - 1u)) != 0) return -1;
  if (align - 1u > (size_t)(UINTPTR_MAX - base)) return -1;
  *out = (base + (align - 1u)) & ~(uintptr_t)(align - 1u);
  return 0;
}

void kavak_arena_init(KavakArena *arena, size_t chunk_size) {
  if (!arena) return;
  if (chunk_size == 0) chunk_size = KAVAK_ARENA_DEFAULT_CHUNK;
  arena->chunk_size = chunk_size;
  arena->total_used = 0;
  arena->head = arena->tail = kavak__chunk_new(chunk_size);
  /* On OOM head=tail=NULL — alloc returns NULL, free is a no-op. */
}

void *kavak_arena_alloc_aligned(KavakArena *arena, const size_t size, size_t align) {
  if (!arena || !arena->tail) return NULL;
  if (align == 0) align = KAVAK_ARENA_DEFAULT_ALIGN;

  KavakArenaChunk *tail = arena->tail;
  uintptr_t base    = (uintptr_t)tail->data + tail->used;
  uintptr_t aligned = 0;
  if (kavak__align_forward(base, align, &aligned) != 0) return NULL;
  size_t    pad     = (size_t)(aligned - base);

  size_t used_with_pad = 0;
  size_t used_needed = 0;
  if (kavak__add_overflows_size(tail->used, pad, &used_with_pad) ||
      kavak__add_overflows_size(used_with_pad, size, &used_needed)) {
    return NULL;
  }

  if (used_needed > tail->cap) {
    /* Need a new chunk. Big asks get a chunk sized to fit; reserve
     * extra so the requested alignment can always be satisfied even
     * when data[0] of the new chunk isn't `align`-aligned itself. */
    const size_t extra = align > KAVAK_ARENA_DEFAULT_ALIGN ? align : 0;
    size_t need = 0;
    if (kavak__add_overflows_size(size, extra, &need)) return NULL;
    const size_t cap  = arena->chunk_size > need ? arena->chunk_size : need;
    KavakArenaChunk *fresh = kavak__chunk_new(cap);
    if (!fresh) return NULL;
    tail->next = fresh;
    arena->tail = tail = fresh;

    base    = (uintptr_t)tail->data;
    if (kavak__align_forward(base, align, &aligned) != 0) return NULL;
    pad     = (size_t)(aligned - base);
  }

  size_t used_delta = 0;
  size_t total_used = 0;
  if (kavak__add_overflows_size(pad, size, &used_delta) ||
      used_delta > tail->cap ||
      kavak__add_overflows_size(arena->total_used, used_delta, &total_used)) {
    return NULL;
  }

  void *out = tail->data + tail->used + pad;
  tail->used += used_delta;
  arena->total_used = total_used;
  return out;
}

void *kavak_arena_alloc(KavakArena *arena, const size_t size) {
  return kavak_arena_alloc_aligned(arena, size, KAVAK_ARENA_DEFAULT_ALIGN);
}

void kavak_arena_free(KavakArena *arena) {
  if (!arena) return;
  KavakArenaChunk *chunk = arena->head;
  while (chunk) {
    KavakArenaChunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  arena->head = arena->tail = NULL;
  arena->chunk_size = 0;
  arena->total_used = 0;
}
