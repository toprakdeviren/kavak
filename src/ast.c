// SPDX-License-Identifier: MIT
/**
 * @file src/ast.c
 * @brief KavakASTNode layout guard.
 *
 * The common traversal fields stay in the 48-byte header prefix:
 * kind/modifiers/span/type plus first/last_child and next_sibling.
 * Parent context is carried by visitors instead of stored per node.
 * The payload union follows this prefix, so the guard pins the prefix
 * size rather than the total node size.
 */

#include "kavak.h"

#include <stddef.h>

/* LP64 gate. Pointers are 8 bytes, uint32_t is 4 bytes; field order
 * keeps the u32 group before the 8-byte-aligned pointer group. ILP32 /
 * WASM32 uses 4-byte pointers, so this host-size guard only applies to
 * LP64 builds. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(offsetof(struct KavakASTNode, payload) == 48,
               "KavakASTNode header prefix must stay at 48 bytes on LP64. "
               "Payload additions belong after the fixed header prefix.");
#endif

void kavak_ast_append_child(KavakASTNode *parent, KavakASTNode *child) {
  if (!parent || !child) return;
  child->next_sibling = NULL;
  if (!parent->first_child) {
    parent->first_child = child;
    parent->last_child = child;
    return;
  }
  /* Repair an externally-inconsistent node (first_child set, last_child
   * cleared) by walking to the tail — a tested defensive path for callers that
   * splice first_child directly (see test_ast.c). The normal append below is
   * O(1) because last_child already tracks the tail. */
  if (!parent->last_child) {
    KavakASTNode *tail = parent->first_child;
    while (tail->next_sibling) tail = tail->next_sibling;
    tail->next_sibling = child;
    parent->last_child = child;
    return;
  }
  parent->last_child->next_sibling = child;
  parent->last_child = child;
}
