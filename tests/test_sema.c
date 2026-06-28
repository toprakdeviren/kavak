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

/* ── Faz 2 hooks: post-order synthesis, declare pre-pass, overload ──────────── */

static KavakTypeInfo *synth_post(KavakSema *sema, KavakASTNode *node) {
  KavakTypeArena *types = kavak_sema_type_arena(sema);
  if (node->kind == KAVAK_AST_LITERAL) return kavak_ty_builtin(types, KAVAK_TY_INT);
  /* Post-order: children are resolved, so synthesize from the first operand. */
  if (node->kind == KAVAK_AST_BINARY && node->first_child) {
    return node->first_child->type;
  }
  return NULL;
}

static const KavakLanguage SYNTH_LANG = {
  .name = "Synth",
  .file_extension = ".synth",
  .version = 1,
  .resolve_node_post = synth_post,
};

static int declare_foo(KavakSema *sema, KavakASTNode *node) {
  if (node->kind == KAVAK_AST_VAR) {
    (void)kavak_sema_bind(sema, (KavakSymbol){
      .name = "foo", .name_len = 3, .kind = KAVAK_SYM_VALUE,
      .type = kavak_ty_builtin(kavak_sema_type_arena(sema), KAVAK_TY_INT),
    });
  }
  return 0;
}

static const KavakLanguage DECLARE_LANG = {
  .name = "Declare", .file_extension = ".decl", .version = 1,
  .declare_node = declare_foo,
};

static const KavakLanguage NODECLARE_LANG = {
  .name = "NoDeclare", .file_extension = ".nodecl", .version = 1,
};

static int test_post_order_synthesis(void) {
  KavakSession *session = kavak_session_new(&SYNTH_LANG);
  ASSERT(session != NULL, "session new");
  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);

  /* BINARY( LITERAL, LITERAL ): the parent's type must be synthesized from the
   * children, which only works if children are resolved first (post-order). */
  KavakASTNode bin = { .kind = KAVAK_AST_BINARY };
  KavakASTNode lhs = { .kind = KAVAK_AST_LITERAL };
  KavakASTNode rhs = { .kind = KAVAK_AST_LITERAL };
  kavak_ast_append_child(&bin, &lhs);
  kavak_ast_append_child(&bin, &rhs);

  KavakSema *sema = kavak_sema_new(session, NULL, NULL, 0, &bin, &diags);
  ASSERT(sema != NULL, "sema new");
  ASSERT(kavak_sema_resolve_names(sema, &bin) == 0, "resolve");
  ASSERT(lhs.type && lhs.type->kind == KAVAK_TY_INT, "leaf type synthesized");
  ASSERT(bin.type && bin.type->kind == KAVAK_TY_INT,
         "parent type synthesized bottom-up from its children");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  return 0;
}

/* root -> [ use(foo), decl ] with the use PRECEDING the declaration. Resolves
 * under `lang` and copies out scalar results (resolved?, the type kind, the use
 * node's modifiers, diag count) WHILE the session — and the type arena its
 * builtins live in — is still alive, so the caller never derefs freed memory. */
static int run_forward_ref(const KavakLanguage *lang, int *resolved_out,
                           uint32_t *kind_out, uint32_t *mods_out,
                           uint32_t *diags_out) {
  KavakSource source;
  if (kavak_source_init(&source, "foo", 3, "<decl>") != 0) return -1;
  KavakToken tokens[] = {
    { .kind = KAVAK_TOK_IDENT, .span = { 0, 3 } },
    { .kind = KAVAK_TOK_EOF,   .span = { 3, 0 } },
  };
  KavakASTNode root = { .kind = KAVAK_AST_ROOT };
  KavakASTNode use  = { .kind = KAVAK_AST_IDENT, .payload.ident.token_index = 0 };
  KavakASTNode decl = { .kind = KAVAK_AST_VAR };
  kavak_ast_append_child(&root, &use);
  kavak_ast_append_child(&root, &decl);

  KavakSession *session = kavak_session_new(lang);
  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, &source, tokens, 2, &root, &diags);
  int rc = -1;
  if (session && sema) {
    rc = kavak_sema_resolve_names(sema, &root);
    *resolved_out = use.type != NULL;
    *kind_out = use.type ? use.type->kind : 0u;
    *mods_out = use.modifiers;
    *diags_out = diags.count;
  }
  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  kavak_source_free(&source);
  return rc;
}

static int test_declare_pre_pass(void) {
  /* Without a declare pass the forward reference stays unresolved. */
  int resolved1 = 0;
  uint32_t kind1 = 0, mods1 = 0, diags1 = 0;
  ASSERT(run_forward_ref(&NODECLARE_LANG, &resolved1, &kind1, &mods1, &diags1) == 0,
         "resolve (no declare)");
  ASSERT(!resolved1 && (mods1 & KAVAK_AST_FLAG_ERROR),
         "no declare pass => forward reference unresolved");

  /* With declare_node the name is forward-registered and resolution succeeds. */
  int resolved2 = 0;
  uint32_t kind2 = 0, mods2 = 0, diags2 = 0;
  ASSERT(run_forward_ref(&DECLARE_LANG, &resolved2, &kind2, &mods2, &diags2) == 0,
         "resolve (declare)");
  ASSERT(resolved2 && kind2 == KAVAK_TY_INT,
         "declare pass makes the forward reference resolve to the declared type");
  ASSERT(diags2 == 0, "no diagnostics with forward declaration");
  return 0;
}

static int score_exact(const KavakSymbol *cand, const KavakTypeInfo *const *args,
                       uint32_t argc, void *user) {
  (void)user;
  if (!cand->type || cand->type->kind != KAVAK_TY_FUNCTION) return -1;
  if (cand->type->payload.function.param_count != argc) return -1;
  int score = 0;
  for (uint32_t i = 0; i < argc; ++i) {
    if (!kavak_ty_equal_deep(cand->type->payload.function.params[i], args[i])) {
      return -1;
    }
    ++score;
  }
  return score;
}

static int count_visits(const KavakSymbol *sym, void *user) {
  (void)sym;
  ++*(uint32_t *)user;
  return 0;
}

static int test_lookup_all_and_overload(void) {
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");
  KavakDiagVec diags;
  kavak_diag_vec_init(&diags);
  KavakSema *sema = kavak_sema_new(session, NULL, NULL, 0, NULL, &diags);
  ASSERT(sema != NULL, "sema new");

  KavakTypeArena *t = kavak_sema_type_arena(sema);
  KavakTypeInfo *int_ty = kavak_ty_builtin(t, KAVAK_TY_INT);
  KavakTypeInfo *str_ty = kavak_ty_builtin(t, KAVAK_TY_STRING);
  KavakTypeInfo *ii[] = { int_ty, int_ty };
  KavakTypeInfo *ss[] = { str_ty, str_ty };
  KavakTypeInfo *add_ii = kavak_ty_function(t, NULL, ii, 2, int_ty, 0);
  KavakTypeInfo *add_ss = kavak_ty_function(t, NULL, ss, 2, str_ty, 0);

  ASSERT(kavak_sema_bind(sema, (KavakSymbol){ .name = "add", .name_len = 3,
         .kind = KAVAK_SYM_FUNCTION, .type = add_ii }) == 0, "bind add(Int,Int)");
  ASSERT(kavak_sema_bind(sema, (KavakSymbol){ .name = "add", .name_len = 3,
         .kind = KAVAK_SYM_FUNCTION, .type = add_ss }) == 0, "bind add(String,String)");
  ASSERT(kavak_sema_bind(sema, (KavakSymbol){ .name = "other", .name_len = 5,
         .kind = KAVAK_SYM_VALUE, .type = int_ty }) == 0, "bind other");

  uint32_t seen = 0;
  ASSERT(kavak_sema_lookup_all(sema, "add", 3, count_visits, &seen) == 2 && seen == 2,
         "lookup_all surfaces both add overloads");
  seen = 0;
  ASSERT(kavak_sema_lookup_all(sema, "missing", 7, count_visits, &seen) == 0,
         "lookup_all of an unknown name visits nothing");

  const KavakTypeInfo *int_args[] = { int_ty, int_ty };
  const KavakSymbol *pick = kavak_sema_pick_overload(sema, "add", 3, score_exact,
                                                     NULL, int_args, 2);
  ASSERT(pick && pick->type == add_ii, "overload picks add(Int,Int)");

  const KavakTypeInfo *str_args[] = { str_ty, str_ty };
  pick = kavak_sema_pick_overload(sema, "add", 3, score_exact, NULL, str_args, 2);
  ASSERT(pick && pick->type == add_ss, "overload picks add(String,String)");

  const KavakTypeInfo *one[] = { int_ty };
  ASSERT(kavak_sema_pick_overload(sema, "add", 3, score_exact, NULL, one, 1) == NULL,
         "no candidate matches arity 1 => NULL");

  kavak_sema_free(sema);
  kavak_diag_vec_free(&diags);
  kavak_session_free(session);
  return 0;
}

/* ── Faz 4: dump kind-name / payload hooks + JSON escaper ───────────────────── */

static const char *dump_kind_name(uint32_t kind) {
  switch (kind) {
    case KAVAK_AST_IDENT: return "Ident";
    case KAVAK_AST_ROOT:  return "Root";
    default:              return NULL;
  }
}

static size_t dump_payload(const KavakASTNode *node, const KavakSource *source,
                           char *buf, size_t buf_len) {
  if (node->kind != KAVAK_AST_IDENT || !source) return 0;
  size_t len = 0;
  const char *s = kavak_source_slice(source, node->span, &len);
  if (!s) return 0;
  if (buf_len) {
    const size_t n = len < buf_len - 1u ? len : buf_len - 1u;
    memcpy(buf, s, n);
    buf[n] = '\0';
  }
  return len;
}

static const KavakLanguage DUMP_LANG = {
  .name = "DumpTest",
  .file_extension = ".dt",
  .version = 1,
  .lexer = { .numbers = { .flags = KAVAK_NUM_BASE_DEC } },
  .parse_source = parse_single_expr,
  .pre_resolve = pre_resolve_bind_x,
  .ast_kind_name = dump_kind_name,
  .ast_payload = dump_payload,
};

static int test_dump_hooks(void) {
  KavakSession *session = kavak_session_new(&DUMP_LANG);
  ASSERT(session != NULL, "session new");
  KavakResult *r = kavak_analyze(session, "x", "<dump>");
  ASSERT(r != NULL, "analyze");
  ASSERT(kavak_error_count(r) == 0, "x resolves");

  FILE *f = tmpfile();
  ASSERT(f != NULL, "tmpfile");
  kavak_dump_json(r, f);
  fflush(f);
  rewind(f);
  char buf[512];
  const size_t n = fread(buf, 1, sizeof(buf) - 1u, f);
  buf[n] = '\0';
  fclose(f);

  ASSERT(strstr(buf, "\"kindName\":\"Ident\"") != NULL,
         "json dump uses the kind-name hook");
  ASSERT(strstr(buf, "\"payload\":\"x\"") != NULL,
         "json dump uses the source-slicing payload hook");

  kavak_result_free(r);
  kavak_session_free(session);
  return 0;
}

static int test_json_escape(void) {
  char buf[64];
  const char *in = "a\"b\\c\n\t\x01";
  const size_t n = kavak_json_escape(in, strlen(in), buf, sizeof(buf));
  ASSERT(strcmp(buf, "a\\\"b\\\\c\\n\\t\\u0001") == 0,
         "escapes quote, backslash, and control chars");
  ASSERT(n == strlen("a\\\"b\\\\c\\n\\t\\u0001"), "returns the escaped length");

  char small[4];
  const size_t need = kavak_json_escape("hello", 5, small, sizeof(small));
  ASSERT(need == 5, "reports the full length even when truncated");
  ASSERT(small[3] == '\0', "truncated output stays NUL-terminated");
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_scope_shadowing();
  fails += test_branch_and_early_exit_narrowing();
  fails += test_resolve_names();
  fails += test_resolve_depth_limit();
  fails += test_analyze_wires_pipeline();
  fails += test_post_order_synthesis();
  fails += test_declare_pre_pass();
  fails += test_lookup_all_and_overload();
  fails += test_dump_hooks();
  fails += test_json_escape();

  if (fails == 0) {
    printf("  ✓ test_sema: 10/10 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_sema: %d failure(s)\n", fails);
  return 1;
}
