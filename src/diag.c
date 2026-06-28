// SPDX-License-Identifier: MIT
/**
 * @file src/diag.c
 * @brief KavakDiagVec — push, count errors, format GCC-style line.
 *
 * The vec uses doubling-grow malloc. The format function is snprintf-
 * shaped — the caller chooses the buffer size; truncation is silent and
 * reported via the return value.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kavak_diag_vec_init(KavakDiagVec *vector) {
  vector->items = NULL;
  vector->count = 0;
  vector->cap   = 0;
}

void kavak_diag_vec_free(KavakDiagVec *vector) {
  free(vector->items);
  vector->items = NULL;
  vector->count = 0;
  vector->cap   = 0;
}

int kavak_diag_vec_push(KavakDiagVec *vector, const KavakDiag diag) {
  if (!vector) return -1;
  if (vector->count == vector->cap) {
    if (vector->count == UINT32_MAX) return -1;
    uint32_t new_cap = vector->cap ? vector->cap : 8;
    while (new_cap < vector->count + 1u) {
      if (new_cap > UINT32_MAX / 2u) return -1;
      new_cap *= 2u;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(KavakDiag)) return -1;
    KavakDiag *new_items = realloc(vector->items, new_cap * sizeof(*new_items));
    if (!new_items) return -1;
    vector->items = new_items;
    vector->cap   = new_cap;
  }
  vector->items[vector->count++] = diag;
  return 0;
}

uint32_t kavak_diag_error_count(const KavakDiagVec *vector) {
  if (!vector) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < vector->count; ++i) {
    if (vector->items[i].severity == KAVAK_SEV_ERROR) ++n;
  }
  return n;
}

static const char *sev_word(const KavakSeverity severity) {
  switch (severity) {
    case KAVAK_SEV_ERROR:   return "error";
    case KAVAK_SEV_WARNING: return "warning";
    case KAVAK_SEV_NOTE:    return "note";
  }
  return "diag";  /* unreachable for valid enum values */
}

size_t kavak_diag_format(const KavakDiag *diag, const KavakSource *source,
                          char *buf, const size_t buf_len) {
  if (!diag) {
    if (buf && buf_len != 0) buf[0] = '\0';
    return 0;
  }
  uint32_t line = 1, col = 1;
  const char *filename = "<unknown>";
  if (source) {
    if (source->filename) filename = source->filename;
    kavak_source_pos(source, diag->span.start, &line, &col);
  }
  /* snprintf returns < 0 only on encoding error, which can't happen for
   * our format string + the bounded-width %u/%s pieces above. Cast to
   * size_t is safe; pre-flight (buf==NULL, buf_len==0) is C99 standard. */
  const int n = snprintf(buf, buf_len, "%s:%u:%u: %s: %s\n",
                          filename, line, col,
                          sev_word(diag->severity),
                          diag->message ? diag->message : "");
  return n < 0 ? 0 : (size_t)n;
}
