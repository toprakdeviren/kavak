// SPDX-License-Identifier: MIT
/**
 * @file tests/test_span.c
 * @brief KavakSpan constructors, queries, and union.
 */

#include "kavak.h"

#include <stdio.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_make_and_query(void) {
  KavakSpan s = kavak_span_make(10, 5);
  ASSERT(s.start == 10 && s.len == 5, "make: fields");
  ASSERT(kavak_span_end(s) == 15, "end = start + len");
  ASSERT(!kavak_span_is_empty(s), "non-empty");
  ASSERT(kavak_span_contains(s, 10), "contains start");
  ASSERT(kavak_span_contains(s, 14), "contains end-1");
  ASSERT(!kavak_span_contains(s, 15), "doesn't contain end (half-open)");
  ASSERT(!kavak_span_contains(s, 9), "doesn't contain pre-start");
  KavakSpan huge = kavak_span_make(UINT32_MAX - 1u, 10);
  ASSERT(kavak_span_end(huge) == UINT32_MAX, "overflowing end saturates");
  ASSERT(kavak_span_contains(huge, UINT32_MAX - 1u), "saturating span contains start");
  return 0;
}

static int test_from_to(void) {
  KavakSpan s = kavak_span_from_to(10, 15);
  ASSERT(s.start == 10 && s.len == 5, "from_to: start, len");

  /* end ≤ start collapses to empty rather than wrapping uint32_t. */
  KavakSpan z = kavak_span_from_to(10, 10);
  ASSERT(kavak_span_is_empty(z), "from_to(10,10) = empty");
  KavakSpan w = kavak_span_from_to(10, 5);
  ASSERT(kavak_span_is_empty(w), "from_to(10,5) clamps to empty");
  return 0;
}

static int test_none_sentinel(void) {
  KavakSpan n = KAVAK_SPAN_NONE;
  ASSERT(n.start == 0 && n.len == 0, "NONE: zeroed");
  ASSERT(kavak_span_is_empty(n), "NONE: empty");
  return 0;
}

static int test_union(void) {
  KavakSpan a = kavak_span_make(10, 5);   /* [10,15) */
  KavakSpan b = kavak_span_make(20, 3);   /* [20,23) */
  KavakSpan u = kavak_span_union(a, b);
  ASSERT(u.start == 10 && kavak_span_end(u) == 23, "union(disjoint)");

  /* Empty side is absorbed. */
  KavakSpan e = KAVAK_SPAN_NONE;
  ASSERT(kavak_span_union(e, a).start == a.start, "union(empty, a) = a");
  ASSERT(kavak_span_union(a, e).len == a.len,     "union(a, empty) = a");

  /* Overlapping. */
  KavakSpan c = kavak_span_make(13, 10);  /* [13,23) — overlaps a */
  KavakSpan v = kavak_span_union(a, c);
  ASSERT(v.start == 10 && kavak_span_end(v) == 23, "union(overlap)");

  /* Containment. */
  KavakSpan outer = kavak_span_make(0, 100);
  KavakSpan inner = kavak_span_make(40, 5);
  KavakSpan w = kavak_span_union(outer, inner);
  ASSERT(w.start == 0 && w.len == 100, "union(outer, inner) = outer");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_make_and_query();
  fails += test_from_to();
  fails += test_none_sentinel();
  fails += test_union();

  if (fails == 0) {
    printf("  ✓ test_span: 4/4 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_span: %d failure(s)\n", fails);
  return 1;
}
