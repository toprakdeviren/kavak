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
  .string_rule_count = sizeof(STRINGS) / sizeof(*STRINGS),
};

static const KavakParserConfig PARSER_CONFIG = {
  .operators = OPS,
  .operator_count = sizeof(OPS) / sizeof(*OPS),
};

typedef struct ParserFixture {
  KavakSource source;
  KavakTokenVec tokens;
  KavakDiagVec diags;
  KavakArena arena;
  KavakParser *parser;
} ParserFixture;

static int fixture_init(ParserFixture *fixture, const char *src) {
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
                                        &fixture->diags, &PARSER_CONFIG);
  return fixture->parser ? 0 : -1;
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

int main(void) {
  int fails = 0;
  fails += test_pratt_precedence();
  fails += test_tinylang_parse_smoke();
  fails += test_nonassoc_operator_chain_reports_diag();
  fails += test_group_span_includes_delimiters();
  fails += test_speculative_turbofish();
  fails += test_checkpoint_rewinds_diags();
  fails += test_recover_to_kind();
  fails += test_recover_skips_nested_punctuation();
  fails += test_expression_depth_limit();

  if (fails == 0) {
    printf("  ✓ test_parser: 9/9 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_parser: %d failure(s)\n", fails);
  return 1;
}
