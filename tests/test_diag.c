// SPDX-License-Identifier: MIT
/**
 * @file tests/test_diag.c
 * @brief KavakDiagVec push / error count / GCC-style format.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_init_and_free(void) {
  KavakDiagVec vector;
  kavak_diag_vec_init(&vector);
  ASSERT(vector.items == NULL && vector.count == 0 && vector.cap == 0, "init: zeroed");
  kavak_diag_vec_free(&vector);
  ASSERT(vector.items == NULL, "free: NULL");
  return 0;
}

static int test_push_and_count(void) {
  KavakDiagVec vector;
  kavak_diag_vec_init(&vector);

  KavakDiag e1 = { KAVAK_SEV_ERROR,   "bad token",  kavak_span_make(0, 1) };
  KavakDiag w1 = { KAVAK_SEV_WARNING, "shadow",     kavak_span_make(2, 3) };
  KavakDiag e2 = { KAVAK_SEV_ERROR,   "type clash", kavak_span_make(5, 4) };
  KavakDiag n1 = { KAVAK_SEV_NOTE,    "see decl",   kavak_span_make(9, 1) };

  ASSERT(kavak_diag_vec_push(&vector, e1) == 0, "push 1: ok");
  ASSERT(kavak_diag_vec_push(&vector, w1) == 0, "push 2: ok");
  ASSERT(kavak_diag_vec_push(&vector, e2) == 0, "push 3: ok");
  ASSERT(kavak_diag_vec_push(&vector, n1) == 0, "push 4: ok");

  ASSERT(vector.count == 4, "count = 4");
  ASSERT(kavak_diag_error_count(&vector) == 2, "error count = 2");

  /* Push past initial cap of 8 — push 8 more to force a regrow. */
  for (int i = 0; i < 8; ++i) {
    const KavakDiag diag = { KAVAK_SEV_NOTE, "filler", kavak_span_make(0, 0) };
    ASSERT(kavak_diag_vec_push(&vector, diag) == 0, "regrow: ok");
  }
  ASSERT(vector.count == 12, "after regrow: count = 12");
  ASSERT(vector.cap >= 12,    "after regrow: cap grew");
  ASSERT(kavak_diag_error_count(&vector) == 2, "errors unchanged after notes");

  kavak_diag_vec_free(&vector);
  return 0;
}

static int test_format_with_source(void) {
  const char *src_bytes = "abc\nde\nfgh";  /* see test_source — 3 lines */
  KavakSource src;
  kavak_source_init(&src, src_bytes, strlen(src_bytes), "main.kv");

  const KavakDiag diag = {
    KAVAK_SEV_ERROR,
    "unexpected token",
    kavak_span_make(7, 1),  /* on line 3, col 1 — see test_source */
  };

  char buf[128];
  size_t n = kavak_diag_format(&diag, &src, buf, sizeof(buf));
  ASSERT(n > 0, "format: returns length");
  ASSERT(strcmp(buf, "main.kv:3:1: error: unexpected token\n") == 0,
         "format: matches GCC-style");

  /* Pre-flight: NULL buf, 0 len — should still return required length. */
  size_t pre = kavak_diag_format(&diag, &src, NULL, 0);
  ASSERT(pre == n, "format: pre-flight matches actual length");

  kavak_source_free(&src);
  return 0;
}

static int test_format_severities(void) {
  const char *bytes = "x";
  KavakSource src;
  kavak_source_init(&src, bytes, 1, "f.kv");

  char buf[128];
  KavakDiag w = { KAVAK_SEV_WARNING, "shadow",  kavak_span_make(0, 1) };
  KavakDiag n = { KAVAK_SEV_NOTE,    "see also", kavak_span_make(0, 1) };

  kavak_diag_format(&w, &src, buf, sizeof(buf));
  ASSERT(strstr(buf, ": warning: shadow\n") != NULL, "warning word");

  kavak_diag_format(&n, &src, buf, sizeof(buf));
  ASSERT(strstr(buf, ": note: see also\n") != NULL, "note word");

  kavak_source_free(&src);
  return 0;
}

static int test_format_null_source(void) {
  const KavakDiag diag = { KAVAK_SEV_ERROR, "ouch", kavak_span_make(0, 0) };
  char buf[128];
  int n = kavak_diag_format(&diag, NULL, buf, sizeof(buf));
  ASSERT(n > 0, "format: NULL source still formats");
  ASSERT(strcmp(buf, "<unknown>:1:1: error: ouch\n") == 0,
         "format: NULL src → <unknown>:1:1");
  ASSERT(kavak_diag_error_count(NULL) == 0, "NULL diag vec has zero errors");
  ASSERT(kavak_diag_format(NULL, NULL, buf, sizeof(buf)) == 0,
         "NULL diag formats as empty");
  ASSERT(buf[0] == '\0', "NULL diag clears buffer");
  return 0;
}

static int test_format_truncates(void) {
  const KavakDiag diag = { KAVAK_SEV_ERROR, "needs space", kavak_span_make(0, 0) };
  char small[16];
  size_t n = kavak_diag_format(&diag, NULL, small, sizeof(small));
  /* snprintf-style: returns the length it WOULD have written, regardless
   * of truncation. The buffer is NUL-terminated within sizeof(small). */
  ASSERT(n > sizeof(small), "truncation: return = full length");
  ASSERT(strlen(small) == sizeof(small) - 1, "truncation: NUL within buf");
  return 0;
}

static int test_push_rejects_full_vector(void) {
  KavakDiagVec vector = {
    .items = NULL,
    .count = UINT32_MAX,
    .cap = UINT32_MAX,
  };
  const KavakDiag diag = { KAVAK_SEV_NOTE, "full", kavak_span_make(0, 0) };
  ASSERT(kavak_diag_vec_push(&vector, diag) == -1,
         "push at uint32 capacity limit rejected");
  ASSERT(vector.count == UINT32_MAX, "failed push leaves count unchanged");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_init_and_free();
  fails += test_push_and_count();
  fails += test_format_with_source();
  fails += test_format_severities();
  fails += test_format_null_source();
  fails += test_format_truncates();
  fails += test_push_rejects_full_vector();

  if (fails == 0) {
    printf("  ✓ test_diag: 7/7 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_diag: %d failure(s)\n", fails);
  return 1;
}
