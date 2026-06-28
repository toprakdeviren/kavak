// SPDX-License-Identifier: MIT
/**
 * @file tests/test_parser.c
 * @brief Parser toolkit tests: RD helpers, Pratt, recovery, speculation.
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

enum {
  KW_FN = 1, KW_LET, KW_IF, KW_ELSE,
  OP_PLUS = 1, OP_STAR, OP_EQ, OP_ASSIGN, OP_LT, OP_GT,
};

enum {
  TINY_AST_FN = KAVAK_AST_USER_BASE + 1,
  TINY_AST_BLOCK,
  TINY_AST_LET,
  TINY_AST_IF,
  TINY_AST_EXPR_STMT,
};

static const KavakKeyword KEYWORDS[] = {
  { "fn", KW_FN },
  { "let", KW_LET },
  { "if", KW_IF },
  { "else", KW_ELSE },
};

static const KavakOperator OPS[] = {
  { "+",  10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_PLUS },
  { "*",  20, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_STAR },
  { "==",  7, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_EQ },
  { "=",   1, KAVAK_ASSOC_RIGHT, KAVAK_OP_FLAG_INFIX, OP_ASSIGN },
  { "<",   8, KAVAK_ASSOC_NONE,  KAVAK_OP_FLAG_INFIX, OP_LT },
  { ">",   8, KAVAK_ASSOC_NONE,  KAVAK_OP_FLAG_INFIX, OP_GT },
};

static const KavakCommentRule COMMENTS[] = {
  { .open = "//", .close = "\n", .nest = 0, .is_doc = 0 },
};

static const KavakEscape ESCAPES[] = {
  { 'n', 0x0A },
  { '"', '"' },
  { '\\', '\\' },
};

static const KavakStringRule STRINGS[] = {
  {
    .open = "\"", .close = "\"", .flags = 0,
    .escapes = ESCAPES,
    .escape_count = sizeof(ESCAPES) / sizeof(*ESCAPES),
  },
};

static const KavakLexerConfig LEXER_CONFIG = {
  .keywords = KEYWORDS,
  .keyword_count = sizeof(KEYWORDS) / sizeof(*KEYWORDS),
  .operators = OPS,
  .operator_count = sizeof(OPS) / sizeof(*OPS),
  .comments = COMMENTS,
  .comment_count = sizeof(COMMENTS) / sizeof(*COMMENTS),
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_FLOAT | KAVAK_NUM_EXPONENT,
  },
  .strings = STRINGS,
  .string_count = sizeof(STRINGS) / sizeof(*STRINGS),
};

static const KavakParserConfig PARSER_CONFIG = {
  .operators = OPS,
  .operator_count = sizeof(OPS) / sizeof(*OPS),
};

/* Pratt extension hooks: `if` becomes an expression-position atom, and `(args)`
 * becomes a postfix call. Neither is expressible with the operator table alone,
 * so these exercise the generic seams without any Kotlin-specific knowledge. */
static KavakASTNode *hook_primary(KavakParser *parser) {
  if (kavak_parser_check_v(parser, KAVAK_TOK_KEYWORD, KW_IF)) {
    KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_IF);
    (void)kavak_parser_eat_v(parser, KAVAK_TOK_KEYWORD, KW_IF);
    kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
    kavak_parser_finish_node(parser, node);
    return node;
  }
  return NULL;  /* not ours: fall through to the built-in error */
}

static KavakASTNode *hook_postfix(KavakParser *parser, KavakASTNode *lhs) {
  return kavak_parser_parse_call(parser, lhs, '(', ',', ')');
}

static const KavakParserConfig HOOK_CONFIG = {
  .operators = OPS,
  .operator_count = sizeof(OPS) / sizeof(*OPS),
  .parse_primary = hook_primary,
  .parse_postfix = hook_postfix,
};

typedef struct ParserFixture {
  KavakSource source;
  KavakTokenVec tokens;
  KavakDiagVec diags;
  KavakArena arena;
  KavakParser *parser;
} ParserFixture;

static int fixture_init_cfg(ParserFixture *fixture, const char *src,
                            const KavakParserConfig *config) {
  memset(fixture, 0, sizeof(*fixture));
  kavak_token_vec_init(&fixture->tokens);
  kavak_diag_vec_init(&fixture->diags);
  kavak_arena_init(&fixture->arena, 0);
  if (kavak_source_init(&fixture->source, src, strlen(src), "<parser-test>") != 0) {
    return -1;
  }
  if (kavak_lex(&fixture->source, &LEXER_CONFIG, &fixture->tokens, &fixture->diags) != 0) {
    return -1;
  }
  fixture->parser = kavak_parser_new(&fixture->source, fixture->tokens.items,
                                        fixture->tokens.count, &fixture->arena,
                                        &fixture->diags, config);
  return fixture->parser ? 0 : -1;
}

static int fixture_init(ParserFixture *fixture, const char *src) {
  return fixture_init_cfg(fixture, src, &PARSER_CONFIG);
}

static void fixture_free(ParserFixture *fixture) {
  kavak_parser_free(fixture->parser);
  kavak_arena_free(&fixture->arena);
  kavak_diag_vec_free(&fixture->diags);
  kavak_token_vec_free(&fixture->tokens);
  kavak_source_free(&fixture->source);
}

static KavakASTNode *parse_block(KavakParser *parser);
static KavakASTNode *parse_stmt(KavakParser *parser);

static int eat_keyword(KavakParser *parser, const uint32_t keyword_id) {
  return kavak_parser_eat_v(parser, KAVAK_TOK_KEYWORD, keyword_id) != NULL;
}

static int check_keyword(KavakParser *parser, const uint32_t keyword_id) {
  return kavak_parser_check_v(parser, KAVAK_TOK_KEYWORD, keyword_id);
}

static KavakASTNode *parse_let(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_LET);
  (void)eat_keyword(parser, KW_LET);
  (void)kavak_parser_expect(parser, KAVAK_TOK_IDENT, "expected binding name");
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_OP, "=", "expected '='");
  kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
  if (!kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ";", "expected ';'")) {
    static const uint32_t recovery[] = { KAVAK_TOK_KEYWORD, KAVAK_TOK_PUNCT, KAVAK_TOK_EOF };
    node->modifiers |= KAVAK_AST_FLAG_ERROR;
    kavak_parser_recover_to(parser, recovery, sizeof(recovery) / sizeof(*recovery));
  }
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *parse_if(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_IF);
  (void)eat_keyword(parser, KW_IF);
  kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
  kavak_ast_append_child(node, parse_block(parser));
  if (check_keyword(parser, KW_ELSE)) {
    (void)eat_keyword(parser, KW_ELSE);
    kavak_ast_append_child(node, parse_block(parser));
  }
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *parse_expr_stmt(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_EXPR_STMT);
  kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ";", "expected ';'");
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *parse_stmt(KavakParser *parser) {
  if (check_keyword(parser, KW_LET)) return parse_let(parser);
  if (check_keyword(parser, KW_IF)) return parse_if(parser);
  return parse_expr_stmt(parser);
}

static KavakASTNode *parse_block(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_BLOCK);
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "{", "expected '{'");
  while (!kavak_parser_at_end(parser) &&
         !kavak_parser_check_text(parser, KAVAK_TOK_PUNCT, "}")) {
    kavak_ast_append_child(node, parse_stmt(parser));
  }
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "}", "expected '}'");
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *parse_fn(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_FN);
  (void)eat_keyword(parser, KW_FN);
  (void)kavak_parser_expect(parser, KAVAK_TOK_IDENT, "expected function name");
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "(", "expected '('");
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ")", "expected ')'");
  kavak_ast_append_child(node, parse_block(parser));
  kavak_parser_finish_node(parser, node);
  return node;
}

static KavakASTNode *parse_file(KavakParser *parser) {
  KavakASTNode *root = kavak_parser_make_node(parser, KAVAK_AST_ROOT);
  while (!kavak_parser_at_end(parser)) {
    if (check_keyword(parser, KW_FN)) {
      kavak_ast_append_child(root, parse_fn(parser));
    } else {
      kavak_ast_append_child(root, parse_stmt(parser));
    }
  }
  kavak_parser_finish_node(parser, root);
  return root;
}

static int test_pratt_precedence(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "1 + 2 * 3") == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_BINARY, "root is binary");
  ASSERT(expr->payload.op.op_id == OP_PLUS, "+ is the outer operator");
  ASSERT(expr->first_child && expr->first_child->kind == KAVAK_AST_LITERAL,
         "lhs is literal");
  ASSERT(expr->first_child->next_sibling &&
         expr->first_child->next_sibling->kind == KAVAK_AST_BINARY,
         "rhs is nested binary");
  ASSERT(expr->first_child->next_sibling->payload.op.op_id == OP_STAR,
         "* binds tighter than +");
  ASSERT(fixture.diags.count == 0, "parse clean");

  fixture_free(&fixture);
  return 0;
}

static int test_tinylang_parse_smoke(void) {
  const char *src =
    "fn main() {"
    " let x = 1 + 2 * 3;"
    " if x == 7 { let y = \"ok\"; } else { let y = \"no\"; }"
    "}";
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, src) == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *root = parse_file(fixture.parser);
  ASSERT(root && root->kind == KAVAK_AST_ROOT, "root parsed");
  ASSERT(root->first_child && root->first_child->kind == TINY_AST_FN,
         "root has function decl");
  ASSERT(root->first_child->next_sibling == NULL, "single top-level decl");

  KavakASTNode *block = root->first_child->first_child;
  ASSERT(block && block->kind == TINY_AST_BLOCK, "function has block");
  ASSERT(block->first_child && block->first_child->kind == TINY_AST_LET,
         "block starts with let");
  ASSERT(block->first_child->next_sibling &&
         block->first_child->next_sibling->kind == TINY_AST_IF,
         "block has if");
  ASSERT(block->first_child->next_sibling->first_child &&
         block->first_child->next_sibling->first_child->kind == KAVAK_AST_BINARY,
         "if condition parsed by Pratt");
  ASSERT(kavak_parser_at_end(fixture.parser), "parser consumed file");
  ASSERT(fixture.diags.count == 0, "parse clean");

  fixture_free(&fixture);
  return 0;
}

static int test_nonassoc_operator_chain_reports_diag(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "1 < 2 < 3") == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_BINARY, "first comparison parsed");
  ASSERT(expr->payload.op.op_id == OP_LT, "outer op is <");
  ASSERT(fixture.diags.count == 1, "non-assoc chain reports one diag");
  ASSERT(strcmp(fixture.diags.items[0].message,
                "non-associative operator cannot be chained") == 0,
         "diag message");
  ASSERT(kavak_parser_check_v(fixture.parser, KAVAK_TOK_OP, OP_LT),
         "offending operator left for caller recovery");

  fixture_free(&fixture);
  return 0;
}

static int test_group_span_includes_delimiters(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "(1 + 2)") == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_GROUP, "group parsed");
  ASSERT(expr->span.start == 0, "group span starts at open paren");
  ASSERT(expr->span.len == 7, "group span includes closing paren");

  fixture_free(&fixture);
  return 0;
}

/* finish_node must compute the span from node->span (a dedicated field), NOT
 * from the payload union — so a descriptor that stashes its own payload between
 * make_node and finish_node cannot corrupt the span. Mirror of the same input
 * as test_group_span_includes_delimiters: both must yield span [0, 7). */
static int test_finish_node_span_survives_payload_write(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "(1 + 2)") == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *node = kavak_parser_make_node(fixture.parser, KAVAK_AST_GROUP);
  ASSERT(node, "node made");

  /* Descriptor writes its own payload BEFORE finishing — this overwrites the
   * payload.range bookkeeping the old finish_node relied on for first_token. */
  node->payload.user.a = 0xDEADBEEFCAFEBABEull;
  node->payload.user.b = 0x0123456789ABCDEFull;

  /* Advance over the whole "(1 + 2)" so the node spans every token. */
  while (kavak_parser_peek(fixture.parser)->kind != KAVAK_TOK_EOF) {
    kavak_parser_eat(fixture.parser, kavak_parser_peek(fixture.parser)->kind);
  }
  kavak_parser_finish_node(fixture.parser, node);

  ASSERT(node->span.start == 0, "span start intact despite payload write");
  ASSERT(node->span.len == 7, "span len intact despite payload write");

  fixture_free(&fixture);
  return 0;
}

static int try_parse_turbofish(KavakParser *parser) {
  KavakParserCheckpoint checkpoint = kavak_parser_checkpoint(parser);
  if (!kavak_parser_eat(parser, KAVAK_TOK_IDENT)) goto fail;
  if (!kavak_parser_eat_v(parser, KAVAK_TOK_OP, OP_LT)) goto fail;
  if (!kavak_parser_eat(parser, KAVAK_TOK_IDENT)) goto fail;
  if (!kavak_parser_eat_v(parser, KAVAK_TOK_OP, OP_GT)) goto fail;
  return 1;
fail:
  kavak_parser_rewind(parser, checkpoint);
  return 0;
}

static int test_speculative_turbofish(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "foo<Bar>(x) foo < Bar") == 0, "fixture init");
  ASSERT(try_parse_turbofish(fixture.parser), "first sequence is turbofish");
  ASSERT(kavak_parser_check_text(fixture.parser, KAVAK_TOK_PUNCT, "("),
         "cursor lands after >");
  while (!kavak_parser_at_end(fixture.parser) &&
         !kavak_parser_check_text(fixture.parser, KAVAK_TOK_PUNCT, ")")) {
    (void)kavak_parser_eat(fixture.parser, kavak_parser_peek(fixture.parser)->kind);
  }
  (void)kavak_parser_eat_text(fixture.parser, KAVAK_TOK_PUNCT, ")");

  const uint32_t before = kavak_parser_pos(fixture.parser);
  ASSERT(!try_parse_turbofish(fixture.parser), "second sequence is less-than expr");
  ASSERT(kavak_parser_pos(fixture.parser) == before, "failed speculation rewinds");
  ASSERT(fixture.diags.count == 0, "speculation adds no diags");

  fixture_free(&fixture);
  return 0;
}

static int test_checkpoint_rewinds_diags(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "1") == 0, "fixture init");
  KavakParserCheckpoint checkpoint = kavak_parser_checkpoint(fixture.parser);
  ASSERT(kavak_parser_expect(fixture.parser, KAVAK_TOK_IDENT, "expected identifier") == NULL,
         "expect fails");
  ASSERT(fixture.diags.count == 1, "diag recorded");
  kavak_parser_rewind(fixture.parser, checkpoint);
  ASSERT(kavak_parser_pos(fixture.parser) == 0, "cursor rewound");
  ASSERT(fixture.diags.count == 0, "speculative diag dropped");

  fixture_free(&fixture);
  return 0;
}

static int test_recover_to_kind(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "1 + 2; next") == 0, "fixture init");
  static const uint32_t recovery[] = { KAVAK_TOK_IDENT };
  kavak_parser_recover_to(fixture.parser, recovery, sizeof(recovery) / sizeof(*recovery));
  ASSERT(kavak_parser_check(fixture.parser, KAVAK_TOK_IDENT), "recovered to ident");
  ASSERT(kavak_parser_peek(fixture.parser)->span.start == 7, "recovered to next");

  fixture_free(&fixture);
  return 0;
}

static int test_recover_skips_nested_punctuation(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "(1;); next") == 0, "fixture init");
  static const uint32_t recovery[] = { KAVAK_TOK_PUNCT };
  kavak_parser_recover_to(fixture.parser, recovery, sizeof(recovery) / sizeof(*recovery));
  ASSERT(kavak_parser_check_text(fixture.parser, KAVAK_TOK_PUNCT, ";"),
         "recovered to outer punctuation");
  ASSERT(kavak_parser_peek(fixture.parser)->span.start == 4,
         "inner punctuation skipped");

  fixture_free(&fixture);
  return 0;
}

static int test_recover_to_targets_value_aware(void) {
  ParserFixture fixture;

  /* Value-aware: kind-only PUNCT recovery would stop at the first ',', but a
   * {PUNCT, ';'} target skips ',' and recovers to ';'. */
  ASSERT(fixture_init(&fixture, "a , b ; c") == 0, "fixture init");
  static const KavakRecoveryTarget semi[] = { { KAVAK_TOK_PUNCT, ';', 1 } };
  kavak_parser_recover_to_targets(fixture.parser, semi,
                                  sizeof(semi) / sizeof(*semi));
  ASSERT(kavak_parser_check_text(fixture.parser, KAVAK_TOK_PUNCT, ";"),
         "value-aware target recovers to ';', skipping ','");
  fixture_free(&fixture);

  /* match_v == 0 behaves like kind-only recovery: stop at the first ','. */
  ASSERT(fixture_init(&fixture, "a , b ; c") == 0, "fixture init");
  static const KavakRecoveryTarget any_punct[] = { { KAVAK_TOK_PUNCT, 0, 0 } };
  kavak_parser_recover_to_targets(fixture.parser, any_punct,
                                  sizeof(any_punct) / sizeof(*any_punct));
  ASSERT(kavak_parser_check_text(fixture.parser, KAVAK_TOK_PUNCT, ","),
         "match_v=0 stops at the first punctuation");
  fixture_free(&fixture);

  return 0;
}

static int test_expression_depth_limit(void) {
  enum { DEPTH = 600 };
  char src[(DEPTH * 2) + 2];
  for (int i = 0; i < DEPTH; ++i) src[i] = '(';
  src[DEPTH] = '1';
  for (int i = 0; i < DEPTH; ++i) src[DEPTH + 1 + i] = ')';
  src[(DEPTH * 2) + 1] = '\0';

  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, src) == 0, "fixture init");
  ASSERT(fixture.diags.count == 0, "lex clean");

  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr != NULL, "deep expression returns partial AST");

  int found = 0;
  for (uint32_t i = 0; i < fixture.diags.count; ++i) {
    if (strcmp(fixture.diags.items[i].message, "expression nesting too deep") == 0) {
      found = 1;
      break;
    }
  }
  ASSERT(found, "deep expression reports depth diagnostic");

  fixture_free(&fixture);
  return 0;
}

static int test_peek_at(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "a + b") == 0, "fixture init");

  ASSERT(kavak_parser_peek_at(fixture.parser, 0)->kind == KAVAK_TOK_IDENT,
         "peek_at(0) is the current token");
  const KavakToken *plus = kavak_parser_peek_at(fixture.parser, 1);
  ASSERT(plus->kind == KAVAK_TOK_OP && plus->v == OP_PLUS, "peek_at(1) is +");
  ASSERT(kavak_parser_peek_at(fixture.parser, 2)->kind == KAVAK_TOK_IDENT,
         "peek_at(2) is the second ident");
  ASSERT(kavak_parser_peek_at(fixture.parser, 3)->kind == KAVAK_TOK_EOF,
         "peek_at past the last real token is EOF");
  ASSERT(kavak_parser_peek_at(fixture.parser, 9999)->kind == KAVAK_TOK_EOF,
         "huge offset clamps to EOF without overflow");
  ASSERT(kavak_parser_pos(fixture.parser) == 0, "peek_at does not move the cursor");

  fixture_free(&fixture);
  return 0;
}

static int test_progressed(void) {
  ParserFixture fixture;
  ASSERT(fixture_init(&fixture, "a b") == 0, "fixture init");

  uint32_t mark = kavak_parser_pos(fixture.parser);
  ASSERT(kavak_parser_eat(fixture.parser, KAVAK_TOK_IDENT) != NULL, "eat ident");
  ASSERT(kavak_parser_progressed(fixture.parser, mark), "advancing reports progress");

  mark = kavak_parser_pos(fixture.parser);
  ASSERT(kavak_parser_eat(fixture.parser, KAVAK_TOK_OP) == NULL, "no op to eat");
  ASSERT(!kavak_parser_progressed(fixture.parser, mark),
         "no consumption reports no progress");

  fixture_free(&fixture);
  return 0;
}

static int test_parse_primary_hook(void) {
  /* With the hook, `if` is a usable expression atom — even embedded. */
  ParserFixture fixture;
  ASSERT(fixture_init_cfg(&fixture, "if a", &HOOK_CONFIG) == 0, "fixture init");
  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == TINY_AST_IF, "if parses as an expression atom");
  ASSERT(fixture.diags.count == 0, "no diagnostics");
  fixture_free(&fixture);

  ASSERT(fixture_init_cfg(&fixture, "1 + if a", &HOOK_CONFIG) == 0, "fixture init");
  expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_BINARY, "if embeds inside an expression");
  ASSERT(expr->first_child->next_sibling &&
         expr->first_child->next_sibling->kind == TINY_AST_IF,
         "rhs of + is the if-atom");
  fixture_free(&fixture);

  /* Without the hook, `if` in expression position is an error (proves the gain). */
  ASSERT(fixture_init(&fixture, "if a") == 0, "fixture init");
  expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_ERROR, "no hook => if is not an atom");
  ASSERT(fixture.diags.count == 1, "built-in 'expected expression' still fires");
  fixture_free(&fixture);
  return 0;
}

static int test_parse_postfix_hook_call(void) {
  /* With the hook, `f(1, 2)` is a call binding tighter than any operator. */
  ParserFixture fixture;
  ASSERT(fixture_init_cfg(&fixture, "f(1, 2)", &HOOK_CONFIG) == 0, "fixture init");
  KavakASTNode *expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_CALL, "call parsed");
  KavakASTNode *callee = expr->first_child;
  ASSERT(callee && callee->kind == KAVAK_AST_IDENT, "first child is the callee");
  ASSERT(callee->next_sibling && callee->next_sibling->next_sibling,
         "two argument children follow the callee");
  ASSERT(kavak_parser_at_end(fixture.parser) && fixture.diags.count == 0,
         "whole call consumed, no diagnostics");
  fixture_free(&fixture);

  ASSERT(fixture_init_cfg(&fixture, "1 + f(2)", &HOOK_CONFIG) == 0, "fixture init");
  expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_BINARY, "outer is +");
  ASSERT(expr->first_child->next_sibling &&
         expr->first_child->next_sibling->kind == KAVAK_AST_CALL,
         "call binds tighter than + (rhs is the call)");
  fixture_free(&fixture);

  /* Without the hook, the call syntax is left unparsed (proves the gain). */
  ASSERT(fixture_init(&fixture, "f(1, 2)") == 0, "fixture init");
  expr = kavak_parse_expression(fixture.parser, 0);
  ASSERT(expr && expr->kind == KAVAK_AST_IDENT, "no hook => only the callee parses");
  ASSERT(!kavak_parser_at_end(fixture.parser), "the '(' is left for the caller");
  fixture_free(&fixture);
  return 0;
}

static int test_parse_delimited(void) {
  ParserFixture fixture;

  ASSERT(fixture_init(&fixture, "(1, 2, 3)") == 0, "fixture init");
  KavakASTNode *node = kavak_parser_make_node(fixture.parser, KAVAK_AST_ROOT);
  ASSERT(kavak_parser_parse_delimited(fixture.parser, node, '(', ',', ')') == 3,
         "three comma-separated items");
  ASSERT(kavak_parser_at_end(fixture.parser) && fixture.diags.count == 0,
         "consumed through close, no diagnostics");
  fixture_free(&fixture);

  ASSERT(fixture_init(&fixture, "()") == 0, "fixture init");
  node = kavak_parser_make_node(fixture.parser, KAVAK_AST_ROOT);
  ASSERT(kavak_parser_parse_delimited(fixture.parser, node, '(', ',', ')') == 0,
         "empty list parses to zero items");
  ASSERT(node->first_child == NULL, "no children for empty list");
  fixture_free(&fixture);

  ASSERT(fixture_init(&fixture, "(1,)") == 0, "fixture init");
  node = kavak_parser_make_node(fixture.parser, KAVAK_AST_ROOT);
  ASSERT(kavak_parser_parse_delimited(fixture.parser, node, '(', ',', ')') == 1,
         "trailing separator tolerated");
  ASSERT(fixture.diags.count == 0, "trailing separator is not an error");
  fixture_free(&fixture);

  ASSERT(fixture_init(&fixture, "(1, 2") == 0, "fixture init");
  node = kavak_parser_make_node(fixture.parser, KAVAK_AST_ROOT);
  (void)kavak_parser_parse_delimited(fixture.parser, node, '(', ',', ')');
  ASSERT(fixture.diags.count == 1, "missing close reports a diagnostic");
  fixture_free(&fixture);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_pratt_precedence();
  fails += test_tinylang_parse_smoke();
  fails += test_nonassoc_operator_chain_reports_diag();
  fails += test_group_span_includes_delimiters();
  fails += test_finish_node_span_survives_payload_write();
  fails += test_speculative_turbofish();
  fails += test_checkpoint_rewinds_diags();
  fails += test_recover_to_kind();
  fails += test_recover_skips_nested_punctuation();
  fails += test_recover_to_targets_value_aware();
  fails += test_expression_depth_limit();
  fails += test_peek_at();
  fails += test_progressed();
  fails += test_parse_primary_hook();
  fails += test_parse_postfix_hook_call();
  fails += test_parse_delimited();

  if (fails == 0) {
    printf("  ✓ test_parser: 16/16 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_parser: %d failure(s)\n", fails);
  return 1;
}
