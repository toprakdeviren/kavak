// SPDX-License-Identifier: MIT
/**
 * @file src/intern.c
 * @brief String intern pool — equal byte sequences map to one stable pointer.
 *
 * Open-addressing hash (FNV-1a, linear probing, power-of-two buckets, 0.75 max
 * load) over strings copied into the pool's own byte arena. Interned pointers
 * are stable for the pool's lifetime, so callers compare names by identity
 * instead of strcmp. Single-threaded, matching the kavak session model.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KAVAK_INTERN_INITIAL_CAP   16u  /* power of two */
#define KAVAK_INTERN_LOAD_NUM       3u  /* grow when count/cap exceeds 3/4 */
#define KAVAK_INTERN_LOAD_DEN       4u

struct KavakInternPool {
  const char **keys;    /* bucket -> interned pointer (NULL = empty) */
  uint32_t    *lens;    /* parallel byte lengths                     */
  uint32_t     cap;     /* power of two                              */
  uint32_t     count;
  KavakArena   strings; /* owns the NUL-terminated string bytes      */
};

static uint64_t fnv1a(const char *s, const size_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

KavakInternPool *kavak_intern_pool_new(void) {
  KavakInternPool *pool = calloc(1, sizeof(*pool));
  if (!pool) return NULL;
  pool->cap = KAVAK_INTERN_INITIAL_CAP;
  pool->keys = calloc(pool->cap, sizeof(*pool->keys));
  pool->lens = calloc(pool->cap, sizeof(*pool->lens));
  kavak_arena_init(&pool->strings, 0);
  if (!pool->keys || !pool->lens || !pool->strings.tail) {
    kavak_intern_pool_free(pool);
    return NULL;
  }
  return pool;
}

void kavak_intern_pool_free(KavakInternPool *pool) {
  if (!pool) return;
  free(pool->keys);
  free(pool->lens);
  kavak_arena_free(&pool->strings);
  free(pool);
}

/* Place a known-unique (key,len) — no growth, no duplicate check. */
static void intern_put(const char **keys, uint32_t *lens, const uint32_t cap,
                       const char *key, const uint32_t len, const uint64_t hash) {
  uint32_t i = (uint32_t)(hash & (cap - 1u));
  while (keys[i]) i = (i + 1u) & (cap - 1u);
  keys[i] = key;
  lens[i] = len;
}

static int intern_grow(KavakInternPool *pool) {
  if (pool->cap > UINT32_MAX / 2u) return -1;
  const uint32_t ncap = pool->cap * 2u;
  const char **nkeys = calloc(ncap, sizeof(*nkeys));
  uint32_t    *nlens = calloc(ncap, sizeof(*nlens));
  if (!nkeys || !nlens) {
    free(nkeys);
    free(nlens);
    return -1;
  }
  for (uint32_t i = 0; i < pool->cap; ++i) {
    if (pool->keys[i]) {
      intern_put(nkeys, nlens, ncap, pool->keys[i], pool->lens[i],
                 fnv1a(pool->keys[i], pool->lens[i]));
    }
  }
  free(pool->keys);
  free(pool->lens);
  pool->keys = nkeys;
  pool->lens = nlens;
  pool->cap  = ncap;
  return 0;
}

const char *kavak_intern(KavakInternPool *pool, const char *s, size_t len) {
  if (!pool) return NULL;
  if (!s) { s = ""; len = 0; }
  if (len > UINT32_MAX) return NULL;
  const uint32_t len32 = (uint32_t)len;

  /* Keep load below 3/4 before probing, so the table never fills. */
  if ((uint64_t)(pool->count + 1u) * KAVAK_INTERN_LOAD_DEN >
      (uint64_t)pool->cap * KAVAK_INTERN_LOAD_NUM) {
    if (intern_grow(pool) != 0) return NULL;
  }

  const uint64_t hash = fnv1a(s, len);
  uint32_t i = (uint32_t)(hash & (pool->cap - 1u));
  while (pool->keys[i]) {
    if (pool->lens[i] == len32 && memcmp(pool->keys[i], s, len) == 0) {
      return pool->keys[i];  /* identical bytes already interned */
    }
    i = (i + 1u) & (pool->cap - 1u);
  }

  char *copy = kavak_arena_alloc(&pool->strings, len + 1u);
  if (!copy) return NULL;
  memcpy(copy, s, len);
  copy[len] = '\0';

  pool->keys[i] = copy;
  pool->lens[i] = len32;
  pool->count++;
  return copy;
}
