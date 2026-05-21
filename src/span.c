// SPDX-License-Identifier: MIT
/**
 * @file src/span.c
 * @brief KavakSpan helpers that don't fit as `static inline` in the
 *        public header.
 *
 * Most of the API is inline in kavak.h — span_make / span_end /
 * span_contains / span_is_empty are one-liners. `span_union` is
 * the only operation with branching worth pulling out of line.
 */

#include "kavak.h"

KavakSpan kavak_span_union(KavakSpan a, KavakSpan b) {
  /* `union(empty, x) == x` keeps callers from special-casing
   * "no span yet" sentinels in incremental-build sites (e.g.,
   * walking a child list to compute a parent's span). */
  if (kavak_span_is_empty(a)) return b;
  if (kavak_span_is_empty(b)) return a;

  const uint32_t start = a.start < b.start ? a.start : b.start;
  const uint32_t end_a = kavak_span_end(a);
  const uint32_t end_b = kavak_span_end(b);
  const uint32_t end   = end_a > end_b ? end_a : end_b;
  return (KavakSpan){ start, end - start };
}
