// SPDX-License-Identifier: MIT
/**
 * @file src/parser.c
 * @brief Parser toolkit: recursive-descent helpers, checkpoints, Pratt core.
 */

#include "kavak.h"
#include "kavak_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Shares the single KAVAK_RECURSION_LIMIT with the type-graph traversal guard. */
#define KAVAK_PARSER_MAX_EXPR_DEPTH KAVAK_RECURSION_LIMIT

struct KavakParser {
  const KavakSource *source;
  const KavakToken  *tokens;
  uint32_t           token_count;
  uint32_t           pos;
  uint32_t           expr_depth;
  KavakArena        *arena;
  KavakDiagVec      *diags;
  KavakParserConfig  config;
};

static void parser_diag(KavakParser *parser, const KavakSpan span,
                        const char *message) {
  if (!parser || !parser->diags) return;
  (void)kavak_diag_vec_push(parser->diags, (KavakDiag){
    .severity = KAVAK_SEV_ERROR,
    .message = message,
    .span = span,
  });
}

static int token_text_eq(const KavakParser *parser, const KavakToken *token,
                         const char *text) {
  if (!parser || !parser->source || !token || !text) return 0;
  const size_t len = strlen(text);
  if (token->span.len != len) return 0;
  if ((size_t)token->span.start + len > parser->source->len) return 0;
  return memcmp(parser->source->bytes + token->span.start, text, len) == 0;
}

static const KavakOperator *operator_for_token(const KavakParser *parser,
                                               const KavakToken *token,
                                               const uint8_t flag) {
  if (!parser || !token || token->kind != KAVAK_TOK_OP) return NULL;
  if (!parser->config.operators) return NULL;
  for (uint32_t i = 0; i < parser->config.operator_count; ++i) {
    const KavakOperator *op = &parser->config.operators[i];
    if (op->id == token->v && (op->flags & flag)) return op;
  }
  return NULL;
}

static uint16_t next_min_prec(const KavakOperator *op) {
  if (op->assoc == KAVAK_ASSOC_RIGHT) return op->prec;
  return op->prec == UINT16_MAX ? UINT16_MAX : (uint16_t)(op->prec + 1u);
}

static int same_precedence_infix_ahead(const KavakParser *parser,
                                       const KavakOperator *op) {
  const KavakToken *token = kavak_parser_peek(parser);
  const KavakOperator *next = operator_for_token(parser, token, KAVAK_OP_FLAG_INFIX);
  return next && next->prec == op->prec;
}

KavakParser *kavak_parser_new(const KavakSource       *source,
                              const KavakToken        *tokens,
                              const uint32_t           token_count,
                              KavakArena              *arena,
                              KavakDiagVec            *diags,
                              const KavakParserConfig *config) {
  if (!tokens || token_count == 0 || !arena) return NULL;
  KavakParser *parser = calloc(1, sizeof(*parser));
  if (!parser) return NULL;
  parser->source = source;
  parser->tokens = tokens;
  parser->token_count = token_count;
  parser->arena = arena;
  parser->diags = diags;
  if (config) parser->config = *config;
  return parser;
}

void kavak_parser_free(KavakParser *parser) {
  free(parser);
}

const KavakToken *kavak_parser_peek(const KavakParser *parser) {
  if (!parser || parser->token_count == 0) return NULL;
  if (parser->pos >= parser->token_count) return &parser->tokens[parser->token_count - 1u];
  return &parser->tokens[parser->pos];
}

const KavakToken *kavak_parser_peek_at(const KavakParser *parser,
                                       const uint32_t offset) {
  if (!parser || parser->token_count == 0) return NULL;
  if (parser->pos >= parser->token_count) {
    return &parser->tokens[parser->token_count - 1u];
  }
  /* Compare against the remaining count so pos + offset can never overflow. */
  const uint32_t remaining = parser->token_count - parser->pos;  /* >= 1 */
  const uint32_t index = offset < remaining ? parser->pos + offset
                                            : parser->token_count - 1u;
  return &parser->tokens[index];
}

const KavakToken *kavak_parser_previous(const KavakParser *parser) {
  if (!parser || parser->pos == 0 || parser->token_count == 0) return NULL;
  const uint32_t i = parser->pos - 1u;
  return &parser->tokens[i < parser->token_count ? i : parser->token_count - 1u];
}

uint32_t kavak_parser_pos(const KavakParser *parser) {
  return parser ? parser->pos : 0;
}

int kavak_parser_at_end(const KavakParser *parser) {
  const KavakToken *token = kavak_parser_peek(parser);
  return !token || token->kind == KAVAK_TOK_EOF;
}

int kavak_parser_progressed(const KavakParser *parser, const uint32_t prev_pos) {
  return parser && parser->pos > prev_pos;
}

int kavak_parser_check(const KavakParser *parser, const uint32_t kind) {
  const KavakToken *token = kavak_parser_peek(parser);
  return token && token->kind == kind;
}

int kavak_parser_check_v(const KavakParser *parser, const uint32_t kind,
                         const uint32_t v) {
  const KavakToken *token = kavak_parser_peek(parser);
  return token && token->kind == kind && token->v == v;
}

int kavak_parser_check_text(const KavakParser *parser, const uint32_t kind,
                            const char *text) {
  const KavakToken *token = kavak_parser_peek(parser);
  return token && token->kind == kind && token_text_eq(parser, token, text);
}

static const KavakToken *parser_advance(KavakParser *parser) {
  if (!parser) return NULL;
  if (!kavak_parser_at_end(parser)) parser->pos++;
  return kavak_parser_previous(parser);
}

const KavakToken *kavak_parser_eat(KavakParser *parser, const uint32_t kind) {
  return kavak_parser_check(parser, kind) ? parser_advance(parser) : NULL;
}

const KavakToken *kavak_parser_eat_v(KavakParser *parser, const uint32_t kind,
                                     const uint32_t v) {
  return kavak_parser_check_v(parser, kind, v) ? parser_advance(parser) : NULL;
}

const KavakToken *kavak_parser_eat_text(KavakParser *parser, const uint32_t kind,
                                        const char *text) {
  return kavak_parser_check_text(parser, kind, text) ? parser_advance(parser) : NULL;
}

const KavakToken *kavak_parser_expect(KavakParser *parser, const uint32_t kind,
                                      const char *message) {
  const KavakToken *token = kavak_parser_eat(parser, kind);
  if (token) return token;
  const KavakToken *at = kavak_parser_peek(parser);
  parser_diag(parser, at ? at->span : KAVAK_SPAN_NONE, message);
  return NULL;
}

const KavakToken *kavak_parser_expect_text(KavakParser *parser, const uint32_t kind,
                                           const char *text, const char *message) {
  const KavakToken *token = kavak_parser_eat_text(parser, kind, text);
  if (token) return token;
  const KavakToken *at = kavak_parser_peek(parser);
  parser_diag(parser, at ? at->span : KAVAK_SPAN_NONE, message);
  return NULL;
}

static int recovery_target_matches(const KavakToken *token,
                                   const uint32_t *kinds,
                                   const uint32_t kind_count) {
  if (!token) return 0;
  if (token->kind == KAVAK_TOK_PUNCT &&
      (token->v == '(' || token->v == '[' || token->v == '{')) {
    return 0;
  }
  for (uint32_t i = 0; i < kind_count; ++i) {
    if (token->kind == kinds[i]) return 1;
  }
  return 0;
}

static void recovery_update_depth(const KavakToken *token, uint32_t *depth) {
  if (!token || token->kind != KAVAK_TOK_PUNCT || !depth) return;
  switch (token->v) {
    case '(':
    case '[':
    case '{':
      if (*depth < UINT32_MAX) (*depth)++;
      break;
    case ')':
    case ']':
    case '}':
      if (*depth != 0 && *depth != UINT32_MAX) (*depth)--;
      break;
  }
}

void kavak_parser_recover_to(KavakParser *parser, const uint32_t *kinds,
                             const uint32_t kind_count) {
  if (!parser) return;
  uint32_t depth = 0;
  while (!kavak_parser_at_end(parser)) {
    const KavakToken *token = kavak_parser_peek(parser);
    if (depth == 0 && recovery_target_matches(token, kinds, kind_count)) return;
    recovery_update_depth(token, &depth);
    (void)parser_advance(parser);
  }
}

static int recovery_targets_match(const KavakToken *token,
                                  const KavakRecoveryTarget *targets,
                                  const uint32_t count) {
  if (!token) return 0;
  if (token->kind == KAVAK_TOK_PUNCT &&
      (token->v == '(' || token->v == '[' || token->v == '{')) {
    return 0;  /* never stop on an opening bracket; depth handles nesting */
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (token->kind != targets[i].kind) continue;
    if (targets[i].match_v && token->v != targets[i].v) continue;
    return 1;
  }
  return 0;
}

void kavak_parser_recover_to_targets(KavakParser *parser,
                                     const KavakRecoveryTarget *targets,
                                     const uint32_t count) {
  if (!parser) return;
  uint32_t depth = 0;
  while (!kavak_parser_at_end(parser)) {
    const KavakToken *token = kavak_parser_peek(parser);
    if (depth == 0 && recovery_targets_match(token, targets, count)) return;
    recovery_update_depth(token, &depth);
    (void)parser_advance(parser);
  }
}

static KavakASTNode *parser_make_node_at(KavakParser *parser,
                                         const uint32_t kind,
                                         const uint32_t first_token) {
  if (!parser || !parser->arena) return NULL;
  KavakASTNode *node = kavak_arena_alloc(parser->arena, sizeof(*node));
  if (!node) return NULL;
  node->kind = kind;
  node->payload.range.first_token = first_token;
  node->payload.range.last_token = first_token;
  if (first_token < parser->token_count) node->span = parser->tokens[first_token].span;
  return node;
}

KavakASTNode *kavak_parser_make_node(KavakParser *parser, const uint32_t kind) {
  return parser_make_node_at(parser, kind, parser ? parser->pos : 0);
}

void kavak_parser_finish_node(KavakParser *parser, KavakASTNode *node) {
  if (!parser || !node) return;

  /* Anchor on node->span — a DEDICATED field set to the first token at
   * make_node time — never the payload union. Reading the union here would let
   * a descriptor that populates payload.{user,op,literal,...} between make_node
   * and finish_node silently corrupt first_token and the computed span; using
   * node->span removes that foot-gun entirely (and matches how descriptors such
   * as examples/tinylang.c already capture the start span).
   *
   * The full span = first token .. last consumed token, unioned with every
   * child. Tokens are contiguous, so unioning the two endpoints reproduces the
   * whole token range; kavak_span_union ignores empty spans, so trailing EOF
   * tokens contribute nothing. The `.start` guard drops a candidate that
   * precedes this node (i.e. when no token was consumed past the first). */
  KavakSpan span = node->span;

  uint32_t end = parser->pos;
  if (end > parser->token_count) end = parser->token_count;
  while (end > 0 && parser->tokens[end - 1u].kind == KAVAK_TOK_EOF) --end;
  if (end > 0) {
    const KavakSpan last = parser->tokens[end - 1u].span;
    if (last.start >= span.start) span = kavak_span_union(span, last);
  }

  for (KavakASTNode *child = node->first_child; child; child = child->next_sibling) {
    span = kavak_span_union(span, child->span);
  }
  node->span = span;
}

KavakParserCheckpoint kavak_parser_checkpoint(const KavakParser *parser) {
  return (KavakParserCheckpoint){
    .pos = parser ? parser->pos : 0,
    .diag_count = parser && parser->diags ? parser->diags->count : 0,
  };
}

void kavak_parser_rewind(KavakParser *parser, const KavakParserCheckpoint checkpoint) {
  if (!parser) return;
  parser->pos = checkpoint.pos <= parser->token_count ? checkpoint.pos
                                                       : parser->token_count;
  if (parser->diags && parser->diags->count > checkpoint.diag_count) {
    parser->diags->count = checkpoint.diag_count;
  }
}

static KavakASTNode *make_error_node(KavakParser *parser, const char *message) {
  KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_ERROR);
  if (!node) return NULL;
  node->modifiers |= KAVAK_AST_FLAG_ERROR;
  const KavakToken *token = kavak_parser_peek(parser);
  parser_diag(parser, token ? token->span : KAVAK_SPAN_NONE, message);
  if (!kavak_parser_at_end(parser)) (void)parser_advance(parser);
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *make_token_node(KavakParser *parser, const uint32_t kind,
                                     const uint32_t token_index,
                                     const uint32_t literal_kind) {
  if (!parser || token_index >= parser->token_count) return NULL;
  KavakASTNode *node = kavak_arena_alloc(parser->arena, sizeof(*node));
  if (!node) return NULL;
  node->kind = kind;
  node->span = parser->tokens[token_index].span;
  if (kind == KAVAK_AST_IDENT) {
    node->payload.ident.token_index = token_index;
  } else {
    node->payload.literal.token_index = token_index;
    node->payload.literal.literal_kind = literal_kind;
  }
  return node;
}

static KavakASTNode *parse_prefix_expr(KavakParser *parser) {
  const KavakToken *token = kavak_parser_peek(parser);
  if (!token) return NULL;

  if (token->kind == KAVAK_TOK_IDENT) {
    const uint32_t token_index = parser->pos;
    (void)parser_advance(parser);
    return make_token_node(parser, KAVAK_AST_IDENT, token_index, 0);
  }

  uint32_t literal_kind = 0;
  if (token->kind == KAVAK_TOK_INT) literal_kind = KAVAK_LIT_INT;
  if (token->kind == KAVAK_TOK_FLOAT) literal_kind = KAVAK_LIT_FLOAT;
  if (token->kind == KAVAK_TOK_STRING) literal_kind = KAVAK_LIT_STRING;
  if (token->kind == KAVAK_TOK_CHAR) literal_kind = KAVAK_LIT_CHAR;
  if (literal_kind != 0) {
    const uint32_t token_index = parser->pos;
    (void)parser_advance(parser);
    return make_token_node(parser, KAVAK_AST_LITERAL, token_index, literal_kind);
  }

  if (kavak_parser_eat_text(parser, KAVAK_TOK_PUNCT, "(")) {
    const uint32_t open_token = parser->pos > 0 ? parser->pos - 1u : 0;
    KavakASTNode *group = parser_make_node_at(parser, KAVAK_AST_GROUP, open_token);
    KavakASTNode *expr = kavak_parse_expression(parser, 0);
    if (group && expr) kavak_ast_append_child(group, expr);
    (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ")", "expected ')'");
    kavak_parser_finish_node(parser, group);
    return group;
  }

  const KavakOperator *prefix = operator_for_token(parser, token, KAVAK_OP_FLAG_PREFIX);
  if (prefix) {
    const uint32_t op_token = parser->pos;
    (void)parser_advance(parser);
    KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_UNARY);
    KavakASTNode *rhs = kavak_parse_expression(parser, prefix->prec);
    if (!node) return NULL;
    node->payload.op.op_token = op_token;
    node->payload.op.op_id = prefix->id;
    kavak_ast_append_child(node, rhs);
    node->span = kavak_span_union(parser->tokens[op_token].span, rhs ? rhs->span : KAVAK_SPAN_NONE);
    return node;
  }

  /* Language-contributed atoms (if/when/lambda/...) — only when no built-in
   * atom matched. The hook consumes its tokens on success, nothing on NULL. */
  if (parser->config.parse_primary) {
    KavakASTNode *atom = parser->config.parse_primary(parser);
    if (atom) return atom;
  }

  return make_error_node(parser, "expected expression");
}

KavakASTNode *kavak_parse_expression(KavakParser *parser, const uint16_t min_prec) {
  if (!parser) return NULL;
  if (parser->expr_depth >= KAVAK_PARSER_MAX_EXPR_DEPTH) {
    return make_error_node(parser, "expression nesting too deep");
  }
  parser->expr_depth++;

  KavakASTNode *lhs = parse_prefix_expr(parser);
  if (!lhs) {
    parser->expr_depth--;
    return NULL;
  }

  for (;;) {
    /* Language-contributed trailing forms (call/index) bind tighter than any
     * operator, so try them first. A non-NULL result that consumed input
     * becomes the new lhs; a non-NULL result with no progress is a misbehaving
     * hook — stop rather than spin. NULL means "did not apply": fall through. */
    if (parser->config.parse_postfix) {
      const KavakParserCheckpoint mark = kavak_parser_checkpoint(parser);
      KavakASTNode *ext = parser->config.parse_postfix(parser, lhs);
      if (ext && parser->pos > mark.pos) { lhs = ext; continue; }
      if (ext) break;
      /* Contract: a NULL ("did not apply") result must consume nothing. A hook
       * that breaks this would leave operator parsing at the wrong position —
       * trap it in debug builds, and rewind defensively (position + diags) so a
       * release build cannot mis-parse off a misbehaving descriptor. */
      assert(parser->pos == mark.pos &&
             "parse_postfix returned NULL after consuming tokens");
      if (parser->pos != mark.pos) kavak_parser_rewind(parser, mark);
    }

    const KavakToken *token = kavak_parser_peek(parser);
    const KavakOperator *postfix = operator_for_token(parser, token, KAVAK_OP_FLAG_POSTFIX);
    if (postfix && postfix->prec >= min_prec && !(postfix->flags & KAVAK_OP_FLAG_INFIX)) {
      const uint32_t op_token = parser->pos;
      (void)parser_advance(parser);
      KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_UNARY);
      if (!node) {
        lhs = NULL;
        break;
      }
      node->payload.op.op_token = op_token;
      node->payload.op.op_id = postfix->id;
      kavak_ast_append_child(node, lhs);
      node->span = kavak_span_union(lhs->span, parser->tokens[op_token].span);
      lhs = node;
      continue;
    }

    const KavakOperator *infix = operator_for_token(parser, token, KAVAK_OP_FLAG_INFIX);
    if (!infix || infix->prec < min_prec) break;

    const uint32_t op_token = parser->pos;
    (void)parser_advance(parser);

    if (infix->flags & KAVAK_OP_FLAG_SELECTOR) {
      const KavakToken *next = kavak_parser_peek(parser);
      if (next && next->kind == KAVAK_TOK_IDENT) {
        uint32_t rhs_token_idx = parser->pos;
        (void)parser_advance(parser);
        KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_SELECTOR);
        if (!node) {
          lhs = NULL;
          break;
        }
        node->payload.user.a = rhs_token_idx;
        kavak_ast_append_child(node, lhs);
        node->span = kavak_span_union(lhs->span, parser->tokens[rhs_token_idx].span);
        lhs = node;
      } else {
        parser_diag(parser, next ? next->span : KAVAK_SPAN_NONE, "expected identifier after selector");
        KavakASTNode *err_node = kavak_parser_make_node(parser, KAVAK_AST_ERROR);
        if (err_node) {
          err_node->modifiers |= KAVAK_AST_FLAG_ERROR;
          err_node->span = kavak_span_union(lhs->span, next ? next->span : KAVAK_SPAN_NONE);
        }
        /* Consume the offending token so an outer descriptor parse loop always
         * makes progress (mirrors make_error_node) — otherwise `a.<bad>` could
         * be re-parsed from the same position forever. */
        if (!kavak_parser_at_end(parser)) (void)parser_advance(parser);
        lhs = err_node;
        break;
      }
      continue;
    }

    KavakASTNode *rhs = kavak_parse_expression(parser, next_min_prec(infix));
    KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_BINARY);
    if (!node) {
      lhs = NULL;
      break;
    }
    node->payload.op.op_token = op_token;
    node->payload.op.op_id = infix->id;
    kavak_ast_append_child(node, lhs);
    kavak_ast_append_child(node, rhs);
    node->span = kavak_span_union(lhs->span, rhs ? rhs->span : KAVAK_SPAN_NONE);
    lhs = node;
    if (infix->assoc == KAVAK_ASSOC_NONE &&
        same_precedence_infix_ahead(parser, infix)) {
      const KavakToken *next = kavak_parser_peek(parser);
      parser_diag(parser, next ? next->span : KAVAK_SPAN_NONE,
                  "non-associative operator cannot be chained");
      break;
    }
  }

  parser->expr_depth--;
  return lhs;
}

uint32_t kavak_parser_parse_delimited(KavakParser *parser, KavakASTNode *node,
                                      const char open, const char sep,
                                      const char close) {
  if (!parser || !node) return 0;
  const char open_s[2]  = { open, 0 };
  const char sep_s[2]   = { sep, 0 };
  const char close_s[2] = { close, 0 };

  if (!kavak_parser_eat_text(parser, KAVAK_TOK_PUNCT, open_s)) return 0;

  uint32_t count = 0;
  if (!kavak_parser_check_text(parser, KAVAK_TOK_PUNCT, close_s)) {
    for (;;) {
      const uint32_t mark = parser->pos;
      KavakASTNode *item = kavak_parse_expression(parser, 0);
      if (item) {
        kavak_ast_append_child(node, item);
        ++count;
      }
      /* If the element parse consumed nothing we would loop forever; stop. */
      if (!kavak_parser_progressed(parser, mark)) break;
      if (!kavak_parser_eat_text(parser, KAVAK_TOK_PUNCT, sep_s)) break;
      /* Tolerate a trailing separator before the close. */
      if (kavak_parser_check_text(parser, KAVAK_TOK_PUNCT, close_s)) break;
    }
  }

  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, close_s,
                                 "expected closing delimiter");
  return count;
}

KavakASTNode *kavak_parser_parse_call(KavakParser *parser, KavakASTNode *callee,
                                      const char open, const char sep,
                                      const char close) {
  if (!parser || !callee) return NULL;
  const char open_s[2] = { open, 0 };
  if (!kavak_parser_check_text(parser, KAVAK_TOK_PUNCT, open_s)) return NULL;

  KavakASTNode *call = kavak_parser_make_node(parser, KAVAK_AST_CALL);
  if (!call) return NULL;
  kavak_ast_append_child(call, callee);
  (void)kavak_parser_parse_delimited(parser, call, open, sep, close);
  kavak_parser_finish_node(parser, call);
  return call;
}
