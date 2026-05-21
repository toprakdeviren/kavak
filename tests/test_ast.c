/**
 * @file tests/test_ast.c
 * @brief KavakASTNode header layout gate.
 *
 * Mirrors the _Static_assert in src/ast.c: the first 48 bytes are the
 * fixed header prefix, and the payload begins after it.
 */

#include "kavak.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

static int test_size_lp64(void) {
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
  ASSERT(offsetof(KavakASTNode, payload) == 48,
         "KavakASTNode header prefix must be 48 bytes on LP64");
  ASSERT(sizeof(KavakASTNode) > 48,
         "KavakASTNode total size includes payload after the prefix");
#endif
  return 0;
}

static int test_field_offsets_lp64(void) {
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
  /* Pinned layout — see comment block above the struct in kavak.h. */
  ASSERT(offsetof(KavakASTNode, kind)         ==  0, "kind at 0");
  ASSERT(offsetof(KavakASTNode, modifiers)    ==  4, "modifiers at 4");
  ASSERT(offsetof(KavakASTNode, span)         ==  8, "span at 8");
  ASSERT(offsetof(KavakASTNode, type)         == 16, "type at 16");
  ASSERT(offsetof(KavakASTNode, first_child)  == 24, "first_child at 24");
  ASSERT(offsetof(KavakASTNode, last_child)   == 32, "last_child at 32");
  ASSERT(offsetof(KavakASTNode, next_sibling) == 40, "next_sibling at 40");
  ASSERT(offsetof(KavakASTNode, payload)      == 48, "payload starts after header");
#endif
  return 0;
}

static int test_namespace(void) {
  ASSERT(KAVAK_AST_USER_BASE == 256u,    "user base reserves kernel 0..255");
  ASSERT(KAVAK_AST_FLAG_ERROR == (1u << 31),
         "error flag claims modifiers' top bit");
  ASSERT(KAVAK_AST_BINARY < KAVAK_AST_USER_BASE, "kernel AST kinds are reserved");
  return 0;
}

static int test_append_child(void) {
  KavakASTNode parent = {0};
  KavakASTNode a = {0};
  KavakASTNode b = {0};

  kavak_ast_append_child(&parent, &a);
  ASSERT(parent.first_child == &a, "first append sets first_child");
  ASSERT(parent.last_child == &a, "first append sets last_child");
  ASSERT(a.next_sibling == NULL, "first child sibling cleared");

  kavak_ast_append_child(&parent, &b);
  ASSERT(parent.first_child == &a, "second append preserves first_child");
  ASSERT(parent.last_child == &b, "second append updates last_child");
  ASSERT(a.next_sibling == &b, "first child links to second");
  ASSERT(b.next_sibling == NULL, "second child sibling cleared");

  KavakASTNode inconsistent = { .first_child = &a, .last_child = NULL };
  a.next_sibling = NULL;
  b.next_sibling = NULL;
  kavak_ast_append_child(&inconsistent, &b);
  ASSERT(inconsistent.first_child == &a, "inconsistent append preserves first child");
  ASSERT(inconsistent.last_child == &b, "inconsistent append repairs last child");
  ASSERT(a.next_sibling == &b, "inconsistent append links after existing child");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_size_lp64();
  fails += test_field_offsets_lp64();
  fails += test_namespace();
  fails += test_append_child();

  if (fails == 0) {
    printf("  ✓ test_ast: 4/4 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_ast: %d failure(s)\n", fails);
  return 1;
}
