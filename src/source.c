/**
 * @file src/source.c
 * @brief KavakSource — bytes + line-offset table, (pos)→(line,col).
 *
 * Two-pass init: first counts newlines to size the table, then walks
 * again to fill in offsets. uint32_t offsets cap the source at 4 GiB;
 * init enforces that cap so spans and line offsets cannot truncate.
 *
 * pos() does a binary search over `line_offsets`, so a 100K-line file
 * resolves a position in ~17 comparisons.
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

int kavak_source_init_with_newlines(KavakSource *source,
                                    const char *bytes,
                                    const size_t len,
                                    const char *filename,
                                    uint32_t newline_flags) {
  if (!source) return -1;
  memset(source, 0, sizeof(*source));
  newline_flags = normalized_newline_flags(newline_flags);
  source->newline_flags = newline_flags;
  if (len > (size_t)UINT32_MAX) {
    memset(source, 0, sizeof(*source));
    return -1;
  }
  if (len != 0 && !bytes) {
    memset(source, 0, sizeof(*source));
    return -1;
  }

  /* Count newlines to size the offset table. line_count = newlines + 1
   * (every file has at least one logical line, even if empty). */
  uint32_t newlines = 0;
  source->bytes = bytes;
  source->len = len;
  for (size_t i = 0; i < len;) {
    const uint32_t n = kavak_source_newline_len(source, i, newline_flags);
    if (n != 0) {
      if (newlines == UINT32_MAX - 1u) {
        memset(source, 0, sizeof(*source));
        return -1;
      }
      ++newlines;
      i += n;
    } else {
      ++i;
    }
  }
  const uint32_t lines = newlines + 1;
  /* +1 sentinel slot for the EOF offset (= len) — pos() leans on it
   * to find the last line's end without a special case. */
  uint32_t *table = calloc((size_t)lines + 1, sizeof(uint32_t));
  if (!table) {
    memset(source, 0, sizeof(*source));
    return -1;
  }

  table[0] = 0;
  uint32_t line_idx = 1;
  for (size_t i = 0; i < len;) {
    const uint32_t n = kavak_source_newline_len(source, i, newline_flags);
    if (n != 0) {
      table[line_idx++] = (uint32_t)(i + n);
      i += n;
    } else {
      ++i;
    }
  }
  table[lines] = (uint32_t)len;

  source->bytes        = bytes;
  source->len          = len;
  source->filename     = filename;
  source->newline_flags = newline_flags;
  source->line_offsets = table;
  source->line_count   = lines;
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

void kavak_source_pos(const KavakSource *source, const size_t byte_pos,
                       uint32_t *out_line, uint32_t *out_col) {
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
