// SPDX-License-Identifier: MIT
/**
 * @file tests/test_source.c
 * @brief KavakSource line-offset table + (pos) → (line, col) lookup.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

/* The line-offset table is built lazily on the first position query. These
 * tests inspect line_count / line_offsets[] directly, so they touch the table
 * once (via the documented accessor) to force the build before asserting. */
static void build_lines(KavakSource *s) {
  uint32_t line = 0, col = 0;
  kavak_source_pos(s, 0, &line, &col);
}

static int test_empty(void) {
  KavakSource source;
  ASSERT(kavak_source_init(&source, "", 0, "empty.kv") == 0, "init empty: ok");
  build_lines(&source);
  ASSERT(source.line_count == 1, "empty: 1 logical line");
  ASSERT(source.line_offsets[0] == 0, "empty: line 1 starts at 0");
  ASSERT(source.line_offsets[1] == 0, "empty: sentinel = len = 0");
  uint32_t L, C;
  kavak_source_pos(&source, 0, &L, &C);
  ASSERT(L == 1 && C == 1, "empty: pos(0) = (1,1)");
  kavak_source_free(&source);
  return 0;
}

static int test_single_line(void) {
  const char *src = "hello";
  KavakSource source;
  kavak_source_init(&source, src, strlen(src), "one.kv");
  build_lines(&source);
  ASSERT(source.line_count == 1, "single: 1 line");
  ASSERT(source.line_offsets[0] == 0, "single: starts at 0");
  ASSERT(source.line_offsets[1] == 5, "single: sentinel = 5");

  uint32_t L, C;
  kavak_source_pos(&source, 0, &L, &C); ASSERT(L == 1 && C == 1, "pos(0)");
  kavak_source_pos(&source, 4, &L, &C); ASSERT(L == 1 && C == 5, "pos(4) = end of 'hello'");
  kavak_source_pos(&source, 5, &L, &C); ASSERT(L == 1 && C == 6, "pos(5) = EOF position");

  kavak_source_free(&source);
  return 0;
}

static int test_multi_line(void) {
  /*  0123 4567 89
   *  abc\nde\nfgh
   *      4   7   */
  const char *src = "abc\nde\nfgh";
  KavakSource source;
  kavak_source_init(&source, src, strlen(src), "multi.kv");
  build_lines(&source);
  ASSERT(source.line_count == 3, "multi: 3 lines");
  ASSERT(source.line_offsets[0] == 0, "line 1 starts at 0");
  ASSERT(source.line_offsets[1] == 4, "line 2 starts after \\n at 4");
  ASSERT(source.line_offsets[2] == 7, "line 3 starts after \\n at 7");
  ASSERT(source.line_offsets[3] == 10, "sentinel = len = 10");

  uint32_t L, C;
  kavak_source_pos(&source, 0, &L, &C); ASSERT(L == 1 && C == 1, "pos(0)=(1,1)");
  kavak_source_pos(&source, 2, &L, &C); ASSERT(L == 1 && C == 3, "pos(2)=(1,3) // 'c'");
  kavak_source_pos(&source, 3, &L, &C); ASSERT(L == 1 && C == 4, "pos(3)=(1,4) // \\n on line 1");
  kavak_source_pos(&source, 4, &L, &C); ASSERT(L == 2 && C == 1, "pos(4)=(2,1) // 'd'");
  kavak_source_pos(&source, 6, &L, &C); ASSERT(L == 2 && C == 3, "pos(6)=(2,3) // \\n on line 2");
  kavak_source_pos(&source, 7, &L, &C); ASSERT(L == 3 && C == 1, "pos(7)=(3,1) // 'f'");
  kavak_source_pos(&source, 9, &L, &C); ASSERT(L == 3 && C == 3, "pos(9)=(3,3) // 'h'");

  kavak_source_free(&source);
  return 0;
}

static int test_default_newline_policy(void) {
  const char *src = "a\rb\r\nc\nd";
  KavakSource source;
  ASSERT(kavak_source_init(&source, src, strlen(src), "newlines.kv") == 0,
         "init mixed physical newlines");
  build_lines(&source);
  ASSERT(source.newline_flags == KAVAK_NEWLINE_DEFAULT, "default newline policy stored");
  ASSERT(source.line_count == 4, "CR, CRLF, LF count as physical newlines");
  ASSERT(source.line_offsets[0] == 0, "line 1 starts at 0");
  ASSERT(source.line_offsets[1] == 2, "line 2 starts after CR");
  ASSERT(source.line_offsets[2] == 5, "line 3 starts after CRLF");
  ASSERT(source.line_offsets[3] == 7, "line 4 starts after LF");
  ASSERT(source.line_offsets[4] == 8, "sentinel = len");

  ASSERT(kavak_source_newline_len(&source, 1, 0) == 1, "CR newline len");
  ASSERT(kavak_source_newline_len(&source, 3, 0) == 2, "CRLF newline len");
  ASSERT(kavak_source_newline_len(&source, 6, 0) == 1, "LF newline len");

  kavak_source_free(&source);
  return 0;
}

static int test_unicode_newline_policy(void) {
  const char *src = "a" "\xE2\x80\xA8" "b" "\xC2\x85" "c";
  KavakSource source;
  ASSERT(kavak_source_init_with_newlines(&source, src, strlen(src), "unicode.kv",
                                         KAVAK_NEWLINE_DEFAULT |
                                         KAVAK_NEWLINE_UNICODE) == 0,
         "init Unicode physical newlines");
  build_lines(&source);
  ASSERT(source.line_count == 3, "LS and NEL count as physical newlines");
  ASSERT(source.line_offsets[0] == 0, "line 1 starts at 0");
  ASSERT(source.line_offsets[1] == 4, "line 2 starts after U+2028");
  ASSERT(source.line_offsets[2] == 7, "line 3 starts after U+0085");
  ASSERT(source.line_offsets[3] == 8, "sentinel = len");
  ASSERT(kavak_source_newline_len(&source, 1,
                                  KAVAK_NEWLINE_DEFAULT |
                                  KAVAK_NEWLINE_UNICODE) == 3,
         "U+2028 newline len");
  ASSERT(kavak_source_newline_len(&source, 5,
                                  KAVAK_NEWLINE_DEFAULT |
                                  KAVAK_NEWLINE_UNICODE) == 2,
         "U+0085 newline len");

  kavak_source_free(&source);
  return 0;
}

static int test_trailing_newline(void) {
  /*  0123 4
   *  abc\n        — trailing newline */
  const char *src = "abc\n";
  KavakSource source;
  kavak_source_init(&source, src, strlen(src), "trail.kv");
  build_lines(&source);
  ASSERT(source.line_count == 2, "trailing-\\n source has 2 logical lines");
  ASSERT(source.line_offsets[1] == 4, "line 2 starts at 4 (just past EOF)");
  ASSERT(source.line_offsets[2] == 4, "sentinel = len = 4");

  uint32_t L, C;
  kavak_source_pos(&source, 4, &L, &C);
  ASSERT(L == 2 && C == 1, "pos(4)=(2,1) // line 2 is empty");
  kavak_source_free(&source);
  return 0;
}

static int test_filename_borrow(void) {
  KavakSource source;
  kavak_source_init(&source, "x", 1, "borrowed.kv");
  ASSERT(strcmp(source.filename, "borrowed.kv") == 0, "filename pointer reachable");
  /* Caller still owns the string — no copy was made. The pointer just
   * has to outlive the source, which a string-literal does trivially. */
  kavak_source_free(&source);
  ASSERT(source.filename == NULL, "free clears filename pointer");
  return 0;
}

static int test_rejects_invalid_inputs(void) {
  KavakSource source;

  ASSERT(kavak_source_init(NULL, "", 0, "bad.kv") == -1,
         "NULL source is rejected");
  ASSERT(kavak_source_init(&source, NULL, 1, "bad.kv") == -1,
         "non-empty NULL bytes rejected");
  ASSERT(source.line_offsets == NULL, "failed init leaves source cleared");

#if SIZE_MAX > UINT32_MAX
  ASSERT(kavak_source_init(&source, "", (size_t)UINT32_MAX + 1u, "huge.kv") == -1,
         "source larger than uint32 span domain rejected");
  ASSERT(source.line_offsets == NULL, "overwide source leaves source cleared");
#endif

  return 0;
}

static int test_slice(void) {
  const char *text = "hello world";
  KavakSource source;
  ASSERT(kavak_source_init(&source, text, 11, "<slice>") == 0, "source init");

  size_t len = 0;
  const char *p = kavak_source_slice(&source, kavak_span_make(6, 5), &len);
  ASSERT(p && len == 5 && memcmp(p, "world", 5) == 0, "slice returns the span bytes");
  ASSERT(p == text + 6, "slice borrows into the source buffer");

  p = kavak_source_slice(&source, kavak_span_make(6, 999), &len);
  ASSERT(p && len == 5, "over-long span clamps to available bytes");

  p = kavak_source_slice(&source, kavak_span_make(0, 0), &len);
  ASSERT(p == text && len == 0, "empty span yields a valid pointer, zero length");

  p = kavak_source_slice(&source, kavak_span_make(11, 1), &len);
  ASSERT(p == NULL && len == 0, "span starting at the end is NULL");

  ASSERT(kavak_source_slice(NULL, kavak_span_make(0, 1), &len) == NULL,
         "NULL source yields NULL");

  kavak_source_free(&source);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_empty();
  fails += test_single_line();
  fails += test_multi_line();
  fails += test_default_newline_policy();
  fails += test_unicode_newline_policy();
  fails += test_trailing_newline();
  fails += test_filename_borrow();
  fails += test_rejects_invalid_inputs();
  fails += test_slice();

  if (fails == 0) {
    printf("  ✓ test_source: 9/9 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_source: %d failure(s)\n", fails);
  return 1;
}
