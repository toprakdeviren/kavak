// SPDX-License-Identifier: MIT
/**
 * @file tests/test_sema.c
 * @brief Sema toolkit tests: scopes, narrowing, generic name resolve.
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

static KavakASTNode *parse_single_expr(KavakParser *parser) {
  return kavak_parse_expression(parser, 0);
}

static void pre_resolve_bind_x(KavakSema *sema, KavakASTNode *root) {
  (void)root;
  KavakTypeInfo *int_ty = kavak_ty_builtin(kavak_sema_type_arena(sema),
                                           KAVAK_TY_INT);
  (void)kavak_sema_bind(sema, (KavakSymbol){
    .name = "x",
    .name_len = 1,
    .kind = KAVAK_SYM_VALUE,
    .type = int_ty,
  });
}

static uint32_t narrow_x_to_string(KavakSema *sema, KavakASTNode *condition,
                                   int branch, KavakNarrowing *out,
                                   uint32_t out_cap) {
  (void)condition;
  if (branch != 1) return 0;
  if (!out || out_cap == 0) return 1;
  out[0].symbol = (KavakSymbol){
    .name = "x",
    .name_len = 1,
    .kind = KAVAK_SYM_VALUE,
    .type = kavak_ty_builtin(kavak_sema_type_arena(sema), KAVAK_TY_STRING),
  };
  return 1;
}

static const KavakLanguage TEST_LANG = {
  .name = "SemaTest",
  .file_extension = ".sematest",
  .version = 1,
  .lexer = {
    .numbers = { .flags = KAVAK_NUM_BASE_DEC },
  },
  .parser = {0},
  .parse_source = parse_single_expr,
  .pre_resolve = pre_resolve_bind_x,
  .narrow_for_branch = narrow_x_to_string,
};

static int test_scope_shadowing(void) {
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");

  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, NULL, NULL, 0, NULL, &diags);
  ASSERT(sema != NULL, "sema new");

  KavakTypeArena *types = kavak_sema_type_arena(sema);
  KavakTypeInfo *int_ty = kavak_ty_builtin(types, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(types, KAVAK_TY_STRING);
  ASSERT(kavak_sema_bind(sema, (KavakSymbol){
    .name = "x", .name_len = 1, .kind = KAVAK_SYM_VALUE, .type = int_ty,
  }) == 0, "bind x");

  const KavakSymbol *x = kavak_sema_lookup(sema, "x", 1);
  ASSERT(x && x->type == int_ty, "global lookup sees original x");

  ASSERT(kavak_sema_push_scope(sema) == 0, "push inner");
  ASSERT(kavak_sema_bind_narrowing(sema, "x", 1, string_ty) == 0,
         "bind narrowed x");
  x = kavak_sema_lookup(sema, "x", 1);
  ASSERT(x && x->type == string_ty, "inner lookup sees narrowed x");
  ASSERT((x->flags & KAVAK_SYM_FLAG_NARROWED) != 0, "narrowed flag set");
  kavak_sema_pop_scope(sema);

  x = kavak_sema_lookup(sema, "x", 1);
  ASSERT(x && x->type == int_ty, "pop restores original x");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  return 0;
}

static int lookup_body(KavakSema *sema, void *user) {
  KavakTypeInfo *want = user;
  const KavakSymbol *x = kavak_sema_lookup(sema, "x", 1);
  return x && x->type == want ? 0 : 1;
}

static int test_branch_and_early_exit_narrowing(void) {
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");
  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, NULL, NULL, 0, NULL, &diags);
  ASSERT(sema != NULL, "sema new");

  KavakTypeArena *types = kavak_sema_type_arena(sema);
  KavakTypeInfo *int_ty = kavak_ty_builtin(types, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(types, KAVAK_TY_STRING);
  KavakTypeInfo *never_ty = kavak_ty_builtin(types, KAVAK_TY_NEVER);
  ASSERT(kavak_sema_bind(sema, (KavakSymbol){
    .name = "x", .name_len = 1, .kind = KAVAK_SYM_VALUE, .type = int_ty,
  }) == 0, "bind x");

  KavakASTNode cond = { .kind = KAVAK_AST_IDENT };
  ASSERT(kavak_sema_with_branch_narrowing(sema, &cond, 1,
                                          lookup_body, string_ty) == 0,
         "branch narrowing visible inside callback");
  ASSERT(kavak_sema_lookup(sema, "x", 1)->type == int_ty,
         "branch narrowing popped after callback");

  ASSERT(kavak_sema_apply_early_exit_narrowing(sema, &cond, 1, int_ty) == 0,
         "non-never terminator does nothing");
  ASSERT(kavak_sema_lookup(sema, "x", 1)->type == int_ty,
         "x remains original without never");

  ASSERT(kavak_sema_apply_early_exit_narrowing(sema, &cond, 1, never_ty) == 0,
         "never terminator applies else-side narrowing");
  ASSERT(kavak_sema_lookup(sema, "x", 1)->type == string_ty,
         "early-exit narrowing remains in enclosing scope");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  return 0;
}

static int test_resolve_names(void) {
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");

  KavakSource source;
  ASSERT(kavak_source_init(&source, "x y", 3, "<resolve>") == 0, "source init");
  KavakToken tokens[] = {
    { .kind = KAVAK_TOK_IDENT, .span = { 0, 1 } },
    { .kind = KAVAK_TOK_IDENT, .span = { 2, 1 } },
    { .kind = KAVAK_TOK_EOF,   .span = { 3, 0 } },
  };
  KavakASTNode root = { .kind = KAVAK_AST_ROOT };
  KavakASTNode x_node = { .kind = KAVAK_AST_IDENT, .payload.ident.token_index = 0 };
  KavakASTNode y_node = { .kind = KAVAK_AST_IDENT, .payload.ident.token_index = 1 };
  kavak_ast_append_child(&root, &x_node);
  kavak_ast_append_child(&root, &y_node);

  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, &source, tokens, 3, &root, &diags);
  ASSERT(sema != NULL, "sema new");
  pre_resolve_bind_x(sema, &root);

  ASSERT(kavak_sema_resolve_names(sema, &root) == 0, "resolve names");
  ASSERT(x_node.type && x_node.type->kind == KAVAK_TY_INT, "x resolved to int");
  ASSERT(y_node.type == NULL, "y remains unresolved");
  ASSERT((y_node.modifiers & KAVAK_AST_FLAG_ERROR) != 0, "y marked error");
  ASSERT(diags.count == 1, "undefined y diagnostic");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_source_free(&source);
  kavak_session_free(session);
  return 0;
}

static int test_resolve_depth_limit(void) {
  enum { DEPTH = 1100 };
  KavakASTNode nodes[DEPTH];
  memset(nodes, 0, sizeof(nodes));
  for (int i = 0; i < DEPTH; ++i) nodes[i].kind = KAVAK_AST_GROUP;
  for (int i = 0; i + 1 < DEPTH; ++i) {
    kavak_ast_append_child(&nodes[i], &nodes[i + 1]);
  }

  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");
  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, NULL, NULL, 0, &nodes[0], &diags);
  ASSERT(sema != NULL, "sema new");

  ASSERT(kavak_sema_resolve_names(sema, &nodes[0]) == 0, "resolve names");

  int found = 0;
  for (uint32_t i = 0; i < diags.count; ++i) {
    if (strcmp(diags.items[i].message, "AST nesting too deep") == 0) {
      found = 1;
      break;
    }
  }
  ASSERT(found, "deep AST reports depth diagnostic");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  return 0;
}

static int test_analyze_wires_pipeline(void) {
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");

  KavakResult *ok = kavak_analyze(session, "x", "<ok>");
  ASSERT(ok != NULL, "analyze returns result");
  ASSERT(kavak_error_count(ok) == 0, "x resolves without errors");
  ASSERT(kavak_token_count(ok) == 2, "tokens exposed");
  const KavakASTNode *root = kavak_root(ok);
  ASSERT(root && root->kind == KAVAK_AST_IDENT, "root exposed");
  ASSERT(root->type && root->type->kind == KAVAK_TY_INT, "root type filled");
  FILE *dump = tmpfile();
  ASSERT(dump != NULL, "dump tmpfile");
  kavak_dump_text(ok, dump);
  kavak_dump_json(ok, dump);
  kavak_dump_sexpr(ok, dump);
  fclose(dump);

  KavakASTNode *mutable_root = (KavakASTNode *)root;
  mutable_root->first_child = mutable_root;
  dump = tmpfile();
  ASSERT(dump != NULL, "cyclic dump tmpfile");
  kavak_dump_text(ok, dump);
  kavak_dump_json(ok, dump);
  kavak_dump_sexpr(ok, dump);
  fclose(dump);

  kavak_result_free(ok);

  const char bytes[] = { 'x', 'z' };
  KavakResult *known_len = kavak_analyze_bytes(session, bytes, 1, "<bytes>");
  ASSERT(known_len != NULL, "analyze bytes returns result");
  ASSERT(kavak_error_count(known_len) == 0, "known-length input ignores trailing byte");
  ASSERT(kavak_token_count(known_len) == 2, "known-length token count");
  kavak_result_free(known_len);

  KavakResult *bad = kavak_analyze(session, "z", "<bad>");
  ASSERT(bad != NULL, "bad analyze returns partial result");
  ASSERT(kavak_error_count(bad) == 1, "undefined z is reported");
  ASSERT(strcmp(kavak_error_message(bad, 0), "undefined symbol") == 0,
         "undefined message");
  ASSERT(kavak_error_line(bad, 0) == 1, "undefined line");
  ASSERT(kavak_error_col(bad, 0) == 1, "undefined col");
  kavak_result_free(bad);

  kavak_session_free(session);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_scope_shadowing();
  fails += test_branch_and_early_exit_narrowing();
  fails += test_resolve_names();
  fails += test_resolve_depth_limit();
  fails += test_analyze_wires_pipeline();

  if (fails == 0) {
    printf("  ✓ test_sema: 5/5 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_sema: %d failure(s)\n", fails);
  return 1;
}
