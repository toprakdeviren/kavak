/**
 * @file tests/test_token.c
 * @brief KavakToken shape + KavakTokenVec push/reserve/grow.
 *
 * The struct layout test pins KavakToken at 16 bytes — anyone who grows
 * a field accidentally trips this gate at compile time (via the
 * runtime check below). The kernel reservation of token-kind 0..255
 * is also verified here so language descriptors don't accidentally
 * collide with built-in kinds.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_token_size(void) {
  /* 16 bytes on LP64: 4 (kind) + 8 (span: 4+4) + 4 (v) = 16, no
   * struct-padding tail because alignof(uint32_t) == 4. */
  ASSERT(sizeof(KavakToken) == 16, "KavakToken must stay at 16 bytes");
  return 0;
}

static int test_kind_namespace(void) {
  /* Kernel built-ins live below the user base. */
  ASSERT(KAVAK_TOK_INVALID == 0, "INVALID = 0 sentinel");
  ASSERT(KAVAK_TOK_EOF     <  KAVAK_TOK_USER_BASE, "EOF in kernel range");
  ASSERT(KAVAK_TOK_OP      <  KAVAK_TOK_USER_BASE, "OP in kernel range");
  ASSERT(KAVAK_TOK_USER_BASE == 256u, "user base is 256");
  return 0;
}

static int test_init_and_free(void) {
  KavakTokenVec vector;
  kavak_token_vec_init(&vector);
  ASSERT(vector.items == NULL && vector.count == 0 && vector.cap == 0, "init: zeroed");
  kavak_token_vec_free(&vector);
  ASSERT(vector.items == NULL, "free: NULL");
  return 0;
}

static int test_push_and_grow(void) {
  KavakTokenVec vector;
  kavak_token_vec_init(&vector);

  /* Push past initial implicit cap (16) to force a regrow. */
  for (uint32_t i = 0; i < 40; ++i) {
    const KavakToken token = { KAVAK_TOK_IDENT, kavak_span_make(i, 1), i };
    ASSERT(kavak_token_vec_push(&vector, token) == 0, "push: ok");
  }
  ASSERT(vector.count == 40, "count = 40");
  ASSERT(vector.cap   >= 40, "cap grew to fit");
  ASSERT(vector.items[ 0].kind  == KAVAK_TOK_IDENT, "first kind preserved");
  ASSERT(vector.items[ 0].span.start == 0,           "first span preserved");
  ASSERT(vector.items[39].v     == 39,               "last v preserved");
  ASSERT(vector.items[39].span.start == 39,          "last span preserved");

  kavak_token_vec_free(&vector);
  return 0;
}

static int test_reserve(void) {
  KavakTokenVec vector;
  kavak_token_vec_init(&vector);

  ASSERT(kavak_token_vec_reserve(&vector, 100) == 0, "reserve(100)");
  ASSERT(vector.cap >= 100, "cap >= 100");
  ASSERT(vector.count == 0, "count untouched");

  /* Reserve below current cap is a no-op (same buffer). */
  KavakToken *before = vector.items;
  ASSERT(kavak_token_vec_reserve(&vector, 50) == 0, "reserve(50): ok");
  ASSERT(vector.items == before, "reserve below cap: no realloc");

  kavak_token_vec_free(&vector);
  return 0;
}

static int test_user_extension(void) {
  /* A language can extend KavakToken kinds from the user base; the kernel
   * just stores whatever number it sees. */
  KavakToken user_tok = { KAVAK_TOK_USER_BASE + 7, kavak_span_make(0, 0), 0 };
  ASSERT(user_tok.kind == 263, "user extension stored");
  ASSERT(user_tok.kind > KAVAK_TOK_USER_BASE - 1, "above user base");
  return 0;
}

static int test_push_rejects_full_vector(void) {
  KavakTokenVec vector = {
    .items = NULL,
    .count = UINT32_MAX,
    .cap = UINT32_MAX,
  };
  const KavakToken token = { KAVAK_TOK_EOF, kavak_span_make(0, 0), 0 };
  ASSERT(kavak_token_vec_push(&vector, token) == -1,
         "push at uint32 capacity limit rejected");
  ASSERT(vector.count == UINT32_MAX, "failed push leaves count unchanged");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_token_size();
  fails += test_kind_namespace();
  fails += test_init_and_free();
  fails += test_push_and_grow();
  fails += test_reserve();
  fails += test_user_extension();
  fails += test_push_rejects_full_vector();

  if (fails == 0) {
    printf("  ✓ test_token: 7/7 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_token: %d failure(s)\n", fails);
  return 1;
}
