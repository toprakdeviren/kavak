// SPDX-License-Identifier: MIT
/**
 * @file tests/test_intern.c
 * @brief String intern pool: pointer identity, edge cases, growth.
 */

#include "kavak.h"

#include <stdio.h>
#include <string.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_identity(void) {
  KavakInternPool *pool = kavak_intern_pool_new();
  ASSERT(pool != NULL, "pool new");

  const char *a = kavak_intern(pool, "hello", 5);
  const char *b = kavak_intern(pool, "hello", 5);
  const char *c = kavak_intern(pool, "world", 5);
  ASSERT(a && b && c, "interned non-null");
  ASSERT(a == b, "equal byte sequences share one pointer");
  ASSERT(a != c, "distinct strings get distinct pointers");
  ASSERT(strcmp(a, "hello") == 0, "content preserved, NUL-terminated");

  /* Interning is by (s, len), not strlen — a prefix of a longer buffer. */
  const char *prefix = kavak_intern(pool, "hello world", 5);
  ASSERT(prefix == a, "length-bounded interning matches the prefix");

  kavak_intern_pool_free(pool);
  return 0;
}

static int test_empty_and_null(void) {
  KavakInternPool *pool = kavak_intern_pool_new();
  ASSERT(pool != NULL, "pool new");

  const char *e1 = kavak_intern(pool, "", 0);
  const char *e2 = kavak_intern(pool, NULL, 0);
  const char *e3 = kavak_intern(pool, NULL, 99);  /* NULL treated as "" */
  ASSERT(e1 && e1[0] == '\0', "empty string interned");
  ASSERT(e1 == e2 && e2 == e3, "empty and NULL collapse to one pointer");
  ASSERT(kavak_intern(NULL, "x", 1) == NULL, "NULL pool yields NULL");

  kavak_intern_pool_free(pool);
  return 0;
}

static int test_growth_keeps_identity(void) {
  KavakInternPool *pool = kavak_intern_pool_new();
  ASSERT(pool != NULL, "pool new");

  enum { N = 500 };
  const char *ptrs[N];
  char buf[16];
  for (int i = 0; i < N; ++i) {
    const int len = snprintf(buf, sizeof(buf), "sym%d", i);
    ptrs[i] = kavak_intern(pool, buf, (size_t)len);
    ASSERT(ptrs[i] != NULL, "interned across growth");
  }
  /* After many rehashes, re-interning returns the original pointers. */
  for (int i = 0; i < N; ++i) {
    const int len = snprintf(buf, sizeof(buf), "sym%d", i);
    ASSERT(kavak_intern(pool, buf, (size_t)len) == ptrs[i],
           "pointer stable across table growth");
  }
  for (int i = 1; i < N; ++i) {
    ASSERT(ptrs[i] != ptrs[i - 1], "distinct symbols stay distinct");
  }

  kavak_intern_pool_free(pool);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_identity();
  fails += test_empty_and_null();
  fails += test_growth_keeps_identity();

  if (fails == 0) {
    printf("  ✓ test_intern: 3/3 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_intern: %d failure(s)\n", fails);
  return 1;
}
