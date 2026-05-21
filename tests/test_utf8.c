// SPDX-License-Identifier: MIT
/**
 * @file tests/test_utf8.c
 * @brief UTF-8 decode validation + ASCII identifier predicates.
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

/* Helper: decode the full string, expecting a single codepoint. */
static int expect_decode(const char *bytes, size_t len, uint32_t want_cp,
                          int want_bytes, const char *what) {
  uint32_t cp = 0;
  int n = kavak_utf8_decode(bytes, bytes + len, &cp);
  if (n != want_bytes || cp != want_cp) {
    fprintf(stderr, "  ✗ %s: got n=%d cp=U+%04X, want n=%d cp=U+%04X\n",
             what, n, cp, want_bytes, want_cp);
    return 1;
  }
  return 0;
}

static int test_ascii(void) {
  int f = 0;
  f += expect_decode("A", 1, 'A', 1, "'A'");
  f += expect_decode("0", 1, '0', 1, "'0'");
  f += expect_decode("\x00", 1, 0, 1, "U+0000");
  f += expect_decode("\x7F", 1, 0x7F, 1, "DEL");
  return f;
}

static int test_two_byte(void) {
  /* ñ = U+00F1 = 0xC3 0xB1 */
  int f = 0;
  f += expect_decode("\xC3\xB1", 2, 0x00F1, 2, "ñ");
  /* ü = U+00FC = 0xC3 0xBC */
  f += expect_decode("\xC3\xBC", 2, 0x00FC, 2, "ü");
  /* Smallest 2-byte: U+0080 = 0xC2 0x80 */
  f += expect_decode("\xC2\x80", 2, 0x0080, 2, "U+0080");
  /* Largest 2-byte: U+07FF = 0xDF 0xBF */
  f += expect_decode("\xDF\xBF", 2, 0x07FF, 2, "U+07FF");
  return f;
}

static int test_three_byte(void) {
  int f = 0;
  /* Euro sign € = U+20AC = 0xE2 0x82 0xAC */
  f += expect_decode("\xE2\x82\xAC", 3, 0x20AC, 3, "€");
  /* Smallest 3-byte: U+0800 = 0xE0 0xA0 0x80 */
  f += expect_decode("\xE0\xA0\x80", 3, 0x0800, 3, "U+0800");
  /* Largest 3-byte: U+FFFF = 0xEF 0xBF 0xBF */
  f += expect_decode("\xEF\xBF\xBF", 3, 0xFFFF, 3, "U+FFFF");
  return f;
}

static int test_four_byte(void) {
  int f = 0;
  /* Rocket 🚀 = U+1F680 = 0xF0 0x9F 0x9A 0x80 */
  f += expect_decode("\xF0\x9F\x9A\x80", 4, 0x1F680, 4, "🚀");
  /* Smallest 4-byte: U+10000 = 0xF0 0x90 0x80 0x80 */
  f += expect_decode("\xF0\x90\x80\x80", 4, 0x10000, 4, "U+10000");
  /* Largest valid: U+10FFFF = 0xF4 0x8F 0xBF 0xBF */
  f += expect_decode("\xF4\x8F\xBF\xBF", 4, 0x10FFFF, 4, "U+10FFFF");
  return f;
}

static int reject_bytes(const unsigned char *b, size_t n, const char *what) {
  uint32_t cp = 0xDEADBEEF;
  if (kavak_utf8_decode((const char *)b, (const char *)b + n, &cp) != 0) {
    fprintf(stderr, "  ✗ reject %s: decode succeeded (cp=U+%04X)\n", what, cp);
    return 1;
  }
  return 0;
}

static int advance_bytes(const unsigned char *b, size_t n, int want, const char *what) {
  int got = kavak_utf8_advance((const char *)b, (const char *)b + n);
  if (got != want) {
    fprintf(stderr, "  ✗ advance %s: got %d, want %d\n", what, got, want);
    return 1;
  }
  return 0;
}

static int test_invalid(void) {
  uint32_t cp = 0xDEADBEEF;
  ASSERT(kavak_utf8_decode(NULL, NULL, &cp) == 0, "empty: 0");

  static const unsigned char b1[] = { 0x80 };
  static const unsigned char b2[] = { 0xC3 };
  static const unsigned char b3[] = { 0xC3, 0x40 };
  static const unsigned char b4[] = { 0xC0, 0x80 };
  static const unsigned char b5[] = { 0xE0, 0x81, 0xBF };
  static const unsigned char b6[] = { 0xED, 0xA0, 0x80 };
  static const unsigned char b7[] = { 0xF4, 0x90, 0x80, 0x80 };
  static const unsigned char b8[] = { 0xF8, 0x80, 0x80, 0x80, 0x80 };

  int f = 0;
  f += reject_bytes(b1, sizeof(b1), "lone-cont");
  f += reject_bytes(b2, sizeof(b2), "truncated-2");
  f += reject_bytes(b3, sizeof(b3), "bad-cont");
  f += reject_bytes(b4, sizeof(b4), "overlong-2 (U+0000)");
  f += reject_bytes(b5, sizeof(b5), "overlong-3 (U+007F)");
  f += reject_bytes(b6, sizeof(b6), "surrogate U+D800");
  f += reject_bytes(b7, sizeof(b7), "above U+10FFFF");
  f += reject_bytes(b8, sizeof(b8), "5-byte leading");
  return f;
}

static int test_advance(void) {
  static const unsigned char a1[] = { 'A' };
  static const unsigned char a2[] = { 0xC3, 0xB1 };
  static const unsigned char a4[] = { 0xF0, 0x9F, 0x9A, 0x80 };
  static const unsigned char ab[] = { 0x80 };

  int f = 0;
  f += advance_bytes(a1, sizeof(a1), 1, "ASCII");
  f += advance_bytes(a2, sizeof(a2), 2, "2-byte");
  f += advance_bytes(a4, sizeof(a4), 4, "4-byte");
  f += advance_bytes(ab, sizeof(ab), 0, "bad");
  return f;
}

static int test_ascii_ident(void) {
  /* Start: letters + underscore only. */
  ASSERT( kavak_ascii_is_ident_start('A'),  "'A' starts");
  ASSERT( kavak_ascii_is_ident_start('z'),  "'z' starts");
  ASSERT( kavak_ascii_is_ident_start('_'),  "'_' starts");
  ASSERT(!kavak_ascii_is_ident_start('0'),  "'0' doesn't start");
  ASSERT(!kavak_ascii_is_ident_start('-'),  "'-' doesn't start");
  ASSERT(!kavak_ascii_is_ident_start(0x00F1), "ñ doesn't start (ASCII-only)");

  /* Cont: start chars + digits. */
  ASSERT( kavak_ascii_is_ident_cont('a'),   "'a' continues");
  ASSERT( kavak_ascii_is_ident_cont('0'),   "'0' continues");
  ASSERT( kavak_ascii_is_ident_cont('9'),   "'9' continues");
  ASSERT(!kavak_ascii_is_ident_cont('-'),   "'-' doesn't continue");
  ASSERT(!kavak_ascii_is_ident_cont(' '),   "space doesn't continue");
  return 0;
}

static int test_unicode_ident(void) {
  ASSERT( kavak_unicode_is_ident_start('A'),       "'A' Unicode-starts");
  ASSERT( kavak_unicode_is_ident_start('_'),       "'_' Unicode-starts");
  ASSERT( kavak_unicode_is_ident_start(0x00F1),    "ñ Unicode-starts");
  ASSERT( kavak_unicode_is_ident_start(0x4E2D),    "CJK U+4E2D Unicode-starts");
  ASSERT(!kavak_unicode_is_ident_start('0'),       "'0' doesn't Unicode-start");

  ASSERT( kavak_unicode_is_ident_cont('0'),        "'0' Unicode-continues");
  ASSERT( kavak_unicode_is_ident_cont(0x0301),     "combining acute continues");
  ASSERT(!kavak_unicode_is_ident_cont('-'),        "'-' doesn't Unicode-continue");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_ascii();
  fails += test_two_byte();
  fails += test_three_byte();
  fails += test_four_byte();
  fails += test_invalid();
  fails += test_advance();
  fails += test_ascii_ident();
  fails += test_unicode_ident();

  if (fails == 0) {
    printf("  ✓ test_utf8: 8/8 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_utf8: %d failure(s)\n", fails);
  return 1;
}
