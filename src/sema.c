// SPDX-License-Identifier: MIT
/**
 * @file src/sema.c
 * @brief Sema toolkit: scopes, name resolution, narrowing helpers.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KAVAK_SEMA_MAX_RESOLVE_DEPTH 1024u

struct KavakSema {
  KavakSession       *session;
  const KavakLanguage *language;
  const KavakSource  *source;
  const KavakToken   *tokens;
  uint32_t            token_count;
  KavakASTNode       *root;
  KavakDiagVec       *diags;
  KavakScope         *scope;
  uint32_t            resolve_depth;
};

static uint32_t name_len_or_strlen(const char *name, const uint32_t name_len) {
  if (!name) return 0;
  return name_len ? name_len : (uint32_t)strlen(name);
}

static int name_eq(const KavakSymbol *symbol, const char *name,
                   const uint32_t name_len) {
  if (!symbol || !symbol->name || !name) return 0;
  const uint32_t sym_len = name_len_or_strlen(symbol->name, symbol->name_len);
  if (sym_len != name_len) return 0;
  return memcmp(symbol->name, name, name_len) == 0;
}

static const char *token_text_ptr(const KavakSema *sema, const KavakToken *token,
                                  uint32_t *out_len) {
  if (out_len) *out_len = 0;
  if (!sema || !sema->source || !token) return NULL;
  if ((size_t)token->span.start + token->span.len > sema->source->len) return NULL;
  if (out_len) *out_len = token->span.len;
  return sema->source->bytes + token->span.start;
}

static KavakASTNode *walk_resolve(KavakSema *sema, KavakASTNode *node);
static void walk_declare(KavakSema *sema, KavakASTNode *node);

KavakSema *kavak_sema_new(KavakSession *session,
                          const KavakSource *source,
                          const KavakToken *tokens,
                          const uint32_t token_count,
                          KavakASTNode *root,
                          KavakDiagVec *diags) {
  if (!session) return NULL;
  KavakSema *sema = calloc(1, sizeof(*sema));
  if (!sema) return NULL;
  sema->session = session;
  sema->language = kavak_session_language(session);
  sema->source = source;
  sema->tokens = tokens;
  sema->token_count = token_count;
  sema->root = root;
  sema->diags = diags;
  if (kavak_sema_push_scope(sema) != 0) {
    free(sema);
    return NULL;
  }
  return sema;
}

void kavak_sema_free(KavakSema *sema) {
  if (!sema) return;
  while (sema->scope) kavak_sema_pop_scope(sema);
  free(sema);
}

KavakScope *kavak_sema_scope(const KavakSema *sema) {
  return sema ? sema->scope : NULL;
}

KavakTypeArena *kavak_sema_type_arena(KavakSema *sema) {
  return sema ? kavak_session_type_arena(sema->session) : NULL;
}

const KavakSource *kavak_sema_source(const KavakSema *sema) {
  return sema ? sema->source : NULL;
}

const KavakToken *kavak_sema_tokens(const KavakSema *sema) {
  return sema ? sema->tokens : NULL;
}

uint32_t kavak_sema_token_count(const KavakSema *sema) {
  return sema ? sema->token_count : 0;
}

KavakASTNode *kavak_sema_root(const KavakSema *sema) {
  return sema ? sema->root : NULL;
}

int kavak_sema_push_scope(KavakSema *sema) {
  if (!sema) return -1;
  KavakScope *scope = calloc(1, sizeof(*scope));
  if (!scope) return -1;
  scope->parent = sema->scope;
  sema->scope = scope;
  return 0;
}

void kavak_sema_pop_scope(KavakSema *sema) {
  if (!sema || !sema->scope) return;
  KavakScope *scope = sema->scope;
  sema->scope = scope->parent;
  free(scope->symbols);
  free(scope);
}

int kavak_sema_bind(KavakSema *sema, KavakSymbol symbol) {
  if (!sema || !sema->scope || !symbol.name) return -1;
  symbol.name_len = name_len_or_strlen(symbol.name, symbol.name_len);
  if (sema->scope->count == sema->scope->cap) {
    if (sema->scope->count == UINT32_MAX) return -1;
    if (sema->scope->cap > UINT32_MAX / 2u) return -1;
    const uint32_t new_cap = sema->scope->cap ? sema->scope->cap * 2u : 8u;
    if ((size_t)new_cap > SIZE_MAX / sizeof(*sema->scope->symbols)) return -1;
    KavakSymbol *new_items = realloc(sema->scope->symbols,
                                     (size_t)new_cap * sizeof(*new_items));
    if (!new_items) return -1;
    sema->scope->symbols = new_items;
    sema->scope->cap = new_cap;
  }
  sema->scope->symbols[sema->scope->count++] = symbol;
  return 0;
}

int kavak_sema_bind_narrowing(KavakSema *sema, const char *name,
                              const uint32_t name_len,
                              KavakTypeInfo *narrowed_type) {
  if (!narrowed_type) return -1;
  const KavakSymbol *base = kavak_sema_lookup(sema, name, name_len);
  KavakSymbol symbol = base ? *base : (KavakSymbol){0};
  symbol.name = name;
  symbol.name_len = name_len_or_strlen(name, name_len);
  if (symbol.kind == 0) symbol.kind = KAVAK_SYM_VALUE;
  symbol.flags |= KAVAK_SYM_FLAG_NARROWED;
  symbol.type = narrowed_type;
  return kavak_sema_bind(sema, symbol);
}

const KavakSymbol *kavak_sema_lookup(const KavakSema *sema,
                                     const char *name,
                                     const uint32_t name_len) {
  if (!sema || !name) return NULL;
  const uint32_t len = name_len_or_strlen(name, name_len);
  for (const KavakScope *scope = sema->scope; scope; scope = scope->parent) {
    for (uint32_t i = scope->count; i > 0; --i) {
      const KavakSymbol *symbol = &scope->symbols[i - 1u];
      if (name_eq(symbol, name, len)) return symbol;
    }
  }
  return NULL;
}

uint32_t kavak_sema_lookup_all(const KavakSema *sema, const char *name,
                               const uint32_t name_len,
                               KavakSymbolVisitFn visit, void *user) {
  if (!sema || !name || !visit) return 0;
  const uint32_t len = name_len_or_strlen(name, name_len);
  uint32_t visited = 0;
  for (const KavakScope *scope = sema->scope; scope; scope = scope->parent) {
    for (uint32_t i = scope->count; i > 0; --i) {
      const KavakSymbol *symbol = &scope->symbols[i - 1u];
      if (name_eq(symbol, name, len)) {
        ++visited;
        if (visit(symbol, user) != 0) return visited;
      }
    }
  }
  return visited;
}

typedef struct PickOverloadCtx {
  KavakScoreOverloadFn        score;
  void                       *user;
  const KavakTypeInfo *const *arg_types;
  uint32_t                    arg_count;
  const KavakSymbol          *best;
  int                         best_score;
} PickOverloadCtx;

static int pick_overload_visit(const KavakSymbol *symbol, void *user) {
  PickOverloadCtx *ctx = user;
  const int s = ctx->score(symbol, ctx->arg_types, ctx->arg_count, ctx->user);
  /* >= 0 accepts; strict > keeps the first (newest) candidate on ties. */
  if (s >= 0 && (!ctx->best || s > ctx->best_score)) {
    ctx->best = symbol;
    ctx->best_score = s;
  }
  return 0;  /* visit every candidate */
}

const KavakSymbol *kavak_sema_pick_overload(const KavakSema *sema,
                                            const char *name,
                                            const uint32_t name_len,
                                            KavakScoreOverloadFn score, void *user,
                                            const KavakTypeInfo *const *arg_types,
                                            const uint32_t arg_count) {
  if (!sema || !name || !score) return NULL;
  PickOverloadCtx ctx = {
    .score = score,
    .user = user,
    .arg_types = arg_types,
    .arg_count = arg_count,
    .best = NULL,
    .best_score = 0,
  };
  (void)kavak_sema_lookup_all(sema, name, name_len, pick_overload_visit, &ctx);
  return ctx.best;
}

int kavak_sema_apply_narrowings(KavakSema *sema,
                                const KavakNarrowing *narrowings,
                                const uint32_t narrowing_count) {
  if (narrowing_count == 0) return 0;
  if (!sema || !narrowings) return -1;
  for (uint32_t i = 0; i < narrowing_count; ++i) {
    KavakSymbol symbol = narrowings[i].symbol;
    if (!symbol.name || !symbol.type) return -1;
    symbol.flags |= KAVAK_SYM_FLAG_NARROWED;
    if (symbol.kind == 0) symbol.kind = KAVAK_SYM_VALUE;
    if (kavak_sema_bind(sema, symbol) != 0) return -1;
  }
  return 0;
}

int kavak_sema_with_narrowings(KavakSema *sema,
                               const KavakNarrowing *narrowings,
                               const uint32_t narrowing_count,
                               KavakSemaBodyFn body,
                               void *user) {
  if (!sema || !body) return -1;
  if (kavak_sema_push_scope(sema) != 0) return -1;
  int rc = kavak_sema_apply_narrowings(sema, narrowings, narrowing_count);
  if (rc == 0) rc = body(sema, user);
  kavak_sema_pop_scope(sema);
  return rc;
}

static int branch_narrowings(KavakSema *sema, KavakASTNode *condition,
                             const int branch, KavakNarrowing **out,
                             uint32_t *out_count) {
  *out = NULL;
  *out_count = 0;
  if (!sema || !sema->language || !sema->language->narrow_for_branch) return 0;
  const uint32_t count = sema->language->narrow_for_branch(sema, condition,
                                                           branch, NULL, 0);
  if (count == 0) return 0;
  if ((size_t)count > SIZE_MAX / sizeof(**out)) return -1;
  KavakNarrowing *items = calloc(count, sizeof(*items));
  if (!items) return -1;
  const uint32_t filled = sema->language->narrow_for_branch(sema, condition,
                                                            branch, items, count);
  if (filled == 0) {
    free(items);
    return 0;
  }
  *out = items;
  *out_count = filled < count ? filled : count;
  return 0;
}

int kavak_sema_with_branch_narrowing(KavakSema *sema,
                                     KavakASTNode *condition,
                                     const int branch,
                                     KavakSemaBodyFn body,
                                     void *user) {
  KavakNarrowing *items = NULL;
  uint32_t count = 0;
  const int narrow_rc = branch_narrowings(sema, condition, branch, &items, &count);
  if (narrow_rc != 0) return -1;
  const int rc = kavak_sema_with_narrowings(sema, items, count, body, user);
  free(items);
  return rc;
}

int kavak_sema_apply_early_exit_narrowing(KavakSema *sema,
                                          KavakASTNode *condition,
                                          const int branch,
                                          const KavakTypeInfo *terminator_type) {
  if (!terminator_type || terminator_type->kind != KAVAK_TY_NEVER) return 0;
  KavakNarrowing *items = NULL;
  uint32_t count = 0;
  const int narrow_rc = branch_narrowings(sema, condition, branch, &items, &count);
  if (narrow_rc != 0) return -1;
  const int rc = kavak_sema_apply_narrowings(sema, items, count);
  free(items);
  return rc;
}

void kavak_sema_diag(KavakSema *sema, const KavakSpan span,
                     const char *message) {
  if (!sema || !sema->diags) return;
  (void)kavak_diag_vec_push(sema->diags, (KavakDiag){
    .severity = KAVAK_SEV_ERROR,
    .message = message,
    .span = span,
  });
}

static void resolve_ident(KavakSema *sema, KavakASTNode *node) {
  if (!sema || !node || node->kind != KAVAK_AST_IDENT) return;
  const uint32_t token_index = node->payload.ident.token_index;
  if (token_index >= sema->token_count) return;
  const KavakToken *token = &sema->tokens[token_index];
  uint32_t len = 0;
  const char *name = token_text_ptr(sema, token, &len);
  const KavakSymbol *symbol = kavak_sema_lookup(sema, name, len);
  if (symbol) {
    node->type = symbol->type;
    return;
  }
  node->modifiers |= KAVAK_AST_FLAG_ERROR;
  kavak_sema_diag(sema, token->span, "undefined symbol");
}

static KavakASTNode *walk_resolve(KavakSema *sema, KavakASTNode *node) {
  if (!node) return NULL;
  if (sema && sema->resolve_depth >= KAVAK_SEMA_MAX_RESOLVE_DEPTH) {
    node->modifiers |= KAVAK_AST_FLAG_ERROR;
    kavak_sema_diag(sema, node->span, "AST nesting too deep");
    return node;
  }
  if (sema) sema->resolve_depth++;
  if (sema && sema->language && sema->language->resolve_node) {
    if (sema->language->resolve_node(sema, node) != 0) {
      node->modifiers |= KAVAK_AST_FLAG_ERROR;
    }
  }
  resolve_ident(sema, node);
  for (KavakASTNode *child = node->first_child; child; child = child->next_sibling) {
    (void)walk_resolve(sema, child);
  }
  /* Post-order: children are resolved, so the language can now synthesize this
   * node's type from theirs (and pop any scope it pushed in resolve_node). */
  if (sema && sema->language && sema->language->resolve_node_post) {
    KavakTypeInfo *synthesized = sema->language->resolve_node_post(sema, node);
    if (synthesized) node->type = synthesized;
  }
  if (sema) sema->resolve_depth--;
  return node;
}

/* First traversal: let the language forward-register declarations so the
 * resolve walk can see names regardless of source order. Pure tree walk; the
 * descriptor's declare_node hook owns all "what is a declaration" knowledge. */
static void walk_declare(KavakSema *sema, KavakASTNode *node) {
  if (!node) return;
  if (sema && sema->resolve_depth >= KAVAK_SEMA_MAX_RESOLVE_DEPTH) {
    node->modifiers |= KAVAK_AST_FLAG_ERROR;
    kavak_sema_diag(sema, node->span, "AST nesting too deep");
    return;
  }
  if (sema) sema->resolve_depth++;
  if (sema && sema->language && sema->language->declare_node) {
    if (sema->language->declare_node(sema, node) != 0) {
      node->modifiers |= KAVAK_AST_FLAG_ERROR;
    }
  }
  for (KavakASTNode *child = node->first_child; child; child = child->next_sibling) {
    walk_declare(sema, child);
  }
  if (sema) sema->resolve_depth--;
}

int kavak_sema_resolve_names(KavakSema *sema, KavakASTNode *root) {
  if (!sema) return -1;
  KavakASTNode *target = root ? root : sema->root;
  /* Optional declaration pre-pass for forward references. */
  if (sema->language && sema->language->declare_node) {
    sema->resolve_depth = 0;
    walk_declare(sema, target);
  }
  sema->resolve_depth = 0;
  (void)walk_resolve(sema, target);
  sema->resolve_depth = 0;
  return 0;
}
