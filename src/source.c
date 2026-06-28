// SPDX-License-Identifier: MIT
/**
 * @file src/source.c
 * @brief KavakSource — bytes + line-offset table, (pos)→(line,col).
 *
 * Lazy, single-pass line mapping: init only borrows the buffer; the
 * `line_offsets` table is built on the *first* kavak_source_pos() call and
 * cached. Positions are only ever needed when a consumer asks for a
 * diagnostic's line/col, so a clean parse never scans for newlines at all,
 * and an inspected one scans exactly once (the lexer does its own newline
 * detection for layout — this avoids a second, redundant pre-scan). The
 * source layer maps bytes; UTF-8 validity is reported by the lexer. uint32_t
 * offsets cap the source at 4 GiB; init enforces that cap so spans and line
 * offsets cannot truncate.
 *
 * pos() does a binary search over `line_offsets`, so a 100K-line file
 * resolves a position in ~17 comparisons.
 *
 * Threading: the lazy build mutates the source on first use, so concurrent
 * first-use position queries on one shared source are not safe. kavak's model
 * gives each result its own source, consumed by a single thread, so this
 * matches the existing per-session isolation contract.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t normalized_newline_flags(uint32_t flags) {
  return flags ? flags : KAVAK_NEWLINE_DEFAULT;
}

uint32_t kavak_source_newline_len(const KavakSource *source,
                                  const size_t byte_pos,
                                  uint32_t newline_flags) {
  if (!source || !source->bytes || byte_pos >= source->len) return 0;
  newline_flags = normalized_newline_flags(newline_flags);

  const unsigned char c = (unsigned char)source->bytes[byte_pos];
  if (c == '\r') {
    if ((newline_flags & KAVAK_NEWLINE_CRLF) &&
        byte_pos + 1u < source->len &&
        source->bytes[byte_pos + 1u] == '\n') {
      return 2;
    }
    return (newline_flags & KAVAK_NEWLINE_CR) ? 1u : 0u;
  }
  if (c == '\n') {
    return (newline_flags & KAVAK_NEWLINE_LF) ? 1u : 0u;
  }

  if (newline_flags & KAVAK_NEWLINE_UNICODE) {
    uint32_t cp = 0;
    const int n = kavak_utf8_decode(source->bytes + byte_pos,
                                    source->bytes + source->len,
                                    &cp);
    if (n > 0 && (cp == 0x85u || cp == 0x2028u || cp == 0x2029u)) {
      return (uint32_t)n;
    }
  }

  return 0;
}

/* Build the line-offset table in a single growable pass. Idempotent: a no-op
 * once the table exists. Returns 0 on success, -1 on OOM/overflow (callers
 * then fall back to the (1,1) position). line_count = newlines + 1, with a
 * trailing sentinel = len at table[line_count] so pos() needs no last-line
 * special case. */
static int ensure_line_offsets(KavakSource *source) {
  if (source->line_offsets) return 0;
  if (source->len != 0 && !source->bytes) return -1;

  size_t cap = (source->len >> 6) + 8;  /* ~one line per 64 bytes, min 8 */
  uint32_t *table = malloc(cap * sizeof(uint32_t));
  if (!table) return -1;

  uint32_t count = 0;
  table[count++] = 0;  /* line 1 begins at offset 0 */
  for (size_t i = 0; i < source->len;) {
    const uint32_t n =
        kavak_source_newline_len(source, i, source->newline_flags);
    if (n != 0) {
      if (count >= UINT32_MAX - 1u) {  /* refuse > ~4 G lines */
        free(table);
        return -1;
      }
      if ((size_t)count + 1u >= cap) {  /* keep room for the EOF sentinel */
        if (cap > (SIZE_MAX / sizeof(uint32_t)) / 2u) {  /* doubling would wrap size_t (32-bit/wasm) */
          free(table);
          return -1;
        }
        const size_t ncap = cap * 2u;
        uint32_t *grown = realloc(table, ncap * sizeof(uint32_t));
        if (!grown) {
          free(table);
          return -1;
        }
        table = grown;
        cap = ncap;
      }
      table[count++] = (uint32_t)(i + n);
      i += n;
    } else {
      ++i;
    }
  }
  table[count] = (uint32_t)source->len;  /* EOF sentinel at [line_count] */
  source->line_offsets = table;
  source->line_count   = count;
  return 0;
}

int kavak_source_init_with_newlines(KavakSource *source,
                                    const char *bytes,
                                    const size_t len,
                                    const char *filename,
                                    uint32_t newline_flags) {
  if (!source) return -1;
  memset(source, 0, sizeof(*source));
  if (len > (size_t)UINT32_MAX) return -1;
  if (len != 0 && !bytes) return -1;

  /* Borrow the buffer only; the line-offset table is built lazily on the
   * first kavak_source_pos() call (most analyses never need it). */
  source->bytes         = bytes;
  source->len           = len;
  source->filename      = filename;
  source->newline_flags = normalized_newline_flags(newline_flags);
  source->line_offsets  = NULL;
  source->line_count    = 0;
  return 0;
}

int kavak_source_init(KavakSource *source, const char *bytes, const size_t len,
                       const char *filename) {
  return kavak_source_init_with_newlines(source, bytes, len, filename, 0);
}

void kavak_source_free(KavakSource *source) {
  if (!source) return;
  free(source->line_offsets);
  source->line_offsets = NULL;
  source->line_count   = 0;
  source->bytes        = NULL;
  source->len          = 0;
  source->filename     = NULL;
  source->newline_flags = 0;
}

const char *kavak_source_slice(const KavakSource *source, const KavakSpan span,
                               size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!source || !source->bytes || (size_t)span.start >= source->len) return NULL;
  const size_t avail = source->len - (size_t)span.start;
  const size_t len = (size_t)span.len < avail ? (size_t)span.len : avail;
  if (out_len) *out_len = len;
  return source->bytes + span.start;
}

void kavak_source_pos(const KavakSource *source, const size_t byte_pos,
                       uint32_t *out_line, uint32_t *out_col) {
  /* Build the table on first use (const cache: logical state is unchanged). */
  if (source) ensure_line_offsets((KavakSource *)source);
  if (!source || !source->line_offsets || source->line_count == 0) {
    if (out_line) *out_line = 1;
    if (out_col) *out_col = 1;
    return;
  }
  const size_t query = byte_pos > source->len ? source->len : byte_pos;
  /* Binary search: largest line whose start offset is ≤ byte_pos.
   * `lo` ends pointing at that line (0-based); the return is 1-based. */
  uint32_t lo = 0;
  uint32_t hi = source->line_count;  /* exclusive upper — sentinel at [line_count] */
  while (lo + 1 < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (source->line_offsets[mid] <= query) lo = mid;
    else                                       hi = mid;
  }
  if (out_line) *out_line = lo + 1;
  if (out_col)  *out_col  = (uint32_t)(query - source->line_offsets[lo]) + 1;
}
