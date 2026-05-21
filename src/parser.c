// SPDX-License-Identifier: MIT
/**
 * @file src/parser.c
 * @brief Parser toolkit: recursive-descent helpers, checkpoints, Pratt core.
 */

#include "kavak.h"

#include <stdlib.h>
#include <string.h>

#define KAVAK_PARSER_MAX_EXPR_DEPTH 512u

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
    if (op->op_id == token->v && (op->flags & flag)) return op;
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

static KavakSpan span_for_token_range(const KavakParser *parser,
                                      const uint32_t first,
                                      const uint32_t last_exclusive) {
  KavakSpan span = KAVAK_SPAN_NONE;
  if (!parser || first >= parser->token_count) return span;
  const uint32_t end = last_exclusive < parser->token_count
                     ? last_exclusive
                     : parser->token_count;
  for (uint32_t i = first; i < end; ++i) {
    if (parser->tokens[i].kind == KAVAK_TOK_EOF) continue;
    span = kavak_span_union(span, parser->tokens[i].span);
  }
  return span;
}

void kavak_parser_finish_node(KavakParser *parser, KavakASTNode *node) {
  if (!parser || !node) return;

  const uint32_t first = node->payload.range.first_token;
  const uint32_t last = parser->pos > first ? parser->pos : first + 1u;
  node->payload.range.last_token = last > 0 ? last - 1u : first;

  KavakSpan child_span = KAVAK_SPAN_NONE;
  for (KavakASTNode *child = node->first_child; child; child = child->next_sibling) {
    child_span = kavak_span_union(child_span, child->span);
  }

  const KavakSpan token_span = span_for_token_range(parser, first, last);
  node->span = kavak_span_union(token_span, child_span);
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
    node->payload.op.op_id = prefix->op_id;
    kavak_ast_append_child(node, rhs);
    node->span = kavak_span_union(parser->tokens[op_token].span, rhs ? rhs->span : KAVAK_SPAN_NONE);
    return node;
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
      node->payload.op.op_id = postfix->op_id;
      kavak_ast_append_child(node, lhs);
      node->span = kavak_span_union(lhs->span, parser->tokens[op_token].span);
      lhs = node;
      continue;
    }

    const KavakOperator *infix = operator_for_token(parser, token, KAVAK_OP_FLAG_INFIX);
    if (!infix || infix->prec < min_prec) break;

    const uint32_t op_token = parser->pos;
    (void)parser_advance(parser);
    KavakASTNode *rhs = kavak_parse_expression(parser, next_min_prec(infix));
    KavakASTNode *node = kavak_parser_make_node(parser, KAVAK_AST_BINARY);
    if (!node) {
      lhs = NULL;
      break;
    }
    node->payload.op.op_token = op_token;
    node->payload.op.op_id = infix->op_id;
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
