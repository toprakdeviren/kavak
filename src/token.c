// SPDX-License-Identifier: MIT
/**
 * @file src/token.c
 * @brief KavakTokenVec — push, reserve, doubling-grow.
 *
 * Resizable token storage used by descriptors and the lexer.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdlib.h>

void kavak_token_vec_init(KavakTokenVec *vector) {
  vector->items = NULL;
  vector->count = 0;
  vector->cap   = 0;
}

void kavak_token_vec_free(KavakTokenVec *vector) {
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->cap   = 0;
}

int kavak_token_vec_reserve(KavakTokenVec *vector, const uint32_t n) {
  if (!vector) return -1;
  if (vector->cap >= n) return 0;
  uint32_t new_cap = vector->cap ? vector->cap : 16;
  while (new_cap < n) {
    if (new_cap > UINT32_MAX / 2u) return -1;
    new_cap *= 2u;
  }
  if ((size_t)new_cap > SIZE_MAX / sizeof(KavakToken)) return -1;
  KavakToken *new_items = realloc(vector->items, (size_t)new_cap * sizeof(*new_items));
  if (!new_items) return -1;
  vector->items = new_items;
  vector->cap   = new_cap;
  return 0;
}

int kavak_token_vec_push(KavakTokenVec *vector, const KavakToken token) {
  if (!vector) return -1;
  if (vector->count == vector->cap) {
    if (vector->count == UINT32_MAX) return -1;
    if (kavak_token_vec_reserve(vector, vector->count + 1u) != 0) return -1;
  }
  vector->items[vector->count++] = token;
  return 0;
}
