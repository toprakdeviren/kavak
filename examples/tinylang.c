// SPDX-License-Identifier: MIT
/**
 * @file examples/tinylang.c
 * @brief Small kavak language descriptor using lexer, parser, sema, types, dumps.
 */

#include "kavak.h"

#include <stdio.h>
#include <string.h>

enum {
  KW_FN = 1,
  KW_LET,
  KW_IF,
  KW_ELSE,
};

enum {
  OP_ASSIGN = 1,
  OP_PLUS,
  OP_EQ,
};

enum {
  TINY_AST_FN = KAVAK_AST_USER_BASE + 1,
  TINY_AST_BLOCK,
  TINY_AST_LET,
  TINY_AST_IF,
  TINY_AST_EXPR_STMT,
};

static const KavakKeyword TINY_KEYWORDS[] = {
  { "fn",   KW_FN },
  { "let",  KW_LET },
  { "if",   KW_IF },
  { "else", KW_ELSE },
};

static const KavakOperator TINY_OPERATORS[] = {
  { "=",   1, KAVAK_ASSOC_RIGHT, KAVAK_OP_FLAG_INFIX, OP_ASSIGN },
  { "==",  7, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_EQ },
  { "+",  10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_PLUS },
};

static const KavakCommentRule TINY_COMMENTS[] = {
  { .open = "//", .close = "\n", .nest = 0, .is_doc = 0 },
};

static const KavakEscape TINY_ESCAPES[] = {
  { 'n',  0x0A },
  { '"',  '"' },
  { '\\', '\\' },
};

static const KavakStringRule TINY_STRINGS[] = {
  {
    .open = "\"",
    .close = "\"",
    .flags = 0,
    .escapes = TINY_ESCAPES,
    .escape_count = sizeof(TINY_ESCAPES) / sizeof(*TINY_ESCAPES),
  },
};

static const KavakLexerConfig TINY_LEXER = {
  .keywords = TINY_KEYWORDS,
  .keyword_count = sizeof(TINY_KEYWORDS) / sizeof(*TINY_KEYWORDS),
  .operators = TINY_OPERATORS,
  .operator_count = sizeof(TINY_OPERATORS) / sizeof(*TINY_OPERATORS),
  .comments = TINY_COMMENTS,
  .comment_count = sizeof(TINY_COMMENTS) / sizeof(*TINY_COMMENTS),
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_FLOAT | KAVAK_NUM_EXPONENT,
  },
  .strings = TINY_STRINGS,
  .string_rule_count = sizeof(TINY_STRINGS) / sizeof(*TINY_STRINGS),
};

static const KavakParserConfig TINY_PARSER = {
  .operators = TINY_OPERATORS,
  .operator_count = sizeof(TINY_OPERATORS) / sizeof(*TINY_OPERATORS),
};

static int eat_keyword(KavakParser *parser, const uint32_t keyword_id) {
  return kavak_parser_eat_v(parser, KAVAK_TOK_KEYWORD, keyword_id) != NULL;
}

static int check_keyword(KavakParser *parser, const uint32_t keyword_id) {
  return kavak_parser_check_v(parser, KAVAK_TOK_KEYWORD, keyword_id);
}

static uint32_t previous_token_index(const KavakParser *parser) {
  const uint32_t pos = kavak_parser_pos(parser);
  return pos == 0 ? 0 : pos - 1u;
}

static uint64_t pack_decl_type(const uint32_t type_token, const uint32_t type_kind) {
  return ((uint64_t)type_token << 32u) | type_kind;
}

static uint32_t decl_type_token(const KavakASTNode *node) {
  return node ? (uint32_t)(node->payload.user.b >> 32u) : 0;
}

static uint32_t decl_type_kind(const KavakASTNode *node) {
  return node ? (uint32_t)node->payload.user.b : KAVAK_TY_INVALID;
}

static uint32_t parse_type_ref(KavakParser *parser, uint32_t *out_token_index) {
  const uint32_t token_index = kavak_parser_pos(parser);
  if (kavak_parser_eat_text(parser, KAVAK_TOK_IDENT, "Int")) {
    *out_token_index = token_index;
    return KAVAK_TY_INT;
  }
  if (kavak_parser_eat_text(parser, KAVAK_TOK_IDENT, "String")) {
    *out_token_index = token_index;
    return KAVAK_TY_STRING;
  }
  if (kavak_parser_eat_text(parser, KAVAK_TOK_IDENT, "Bool")) {
    *out_token_index = token_index;
    return KAVAK_TY_BOOL;
  }

  if (kavak_parser_expect(parser, KAVAK_TOK_IDENT, "expected type name")) {
    *out_token_index = token_index;
  }
  return KAVAK_TY_INVALID;
}

static KavakASTNode *parse_block(KavakParser *parser);
static KavakASTNode *parse_stmt(KavakParser *parser);

static KavakASTNode *parse_let(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_LET);
  const KavakSpan start_span = node ? node->span : KAVAK_SPAN_NONE;
  uint32_t type_token = 0;
  uint32_t type_kind = KAVAK_TY_INVALID;

  (void)eat_keyword(parser, KW_LET);
  if (kavak_parser_expect(parser, KAVAK_TOK_IDENT, "expected binding name")) {
    node->payload.user.a = previous_token_index(parser);
  }
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ":", "expected ':'");
  type_kind = parse_type_ref(parser, &type_token);
  node->payload.user.b = pack_decl_type(type_token, type_kind);

  (void)kavak_parser_expect_text(parser, KAVAK_TOK_OP, "=", "expected '='");
  kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
  if (!kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ";", "expected ';'")) {
    static const uint32_t recovery[] = {
      KAVAK_TOK_KEYWORD,
      KAVAK_TOK_PUNCT,
      KAVAK_TOK_EOF,
    };
    node->modifiers |= KAVAK_AST_FLAG_ERROR;
    kavak_parser_recover_to(parser, recovery, sizeof(recovery) / sizeof(*recovery));
  }

  kavak_parser_finish_node(parser, node);
  if (node) node->span = kavak_span_union(start_span, node->span);
  return node;
}

static KavakASTNode *parse_if(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_IF);
  const KavakSpan start_span = node ? node->span : KAVAK_SPAN_NONE;
  (void)eat_keyword(parser, KW_IF);
  kavak_ast_append_child(node, kavak_parse_expression(parser, 0));
  kavak_ast_append_child(node, parse_block(parser));
  if (check_keyword(parser, KW_ELSE)) {
    (void)eat_keyword(parser, KW_ELSE);
    kavak_ast_append_child(node, parse_block(parser));
  }
  kavak_parser_finish_node(parser, node);
  if (node) node->span = kavak_span_union(start_span, node->span);
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
  KavakSpan start_span = node ? node->span : KAVAK_SPAN_NONE;
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "{", "expected '{'");
  if (kavak_parser_previous(parser)) start_span = kavak_parser_previous(parser)->span;
  while (!kavak_parser_at_end(parser) &&
         !kavak_parser_check_text(parser, KAVAK_TOK_PUNCT, "}")) {
    kavak_ast_append_child(node, parse_stmt(parser));
  }
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "}", "expected '}'");
  const KavakSpan end_span = kavak_parser_previous(parser)
                           ? kavak_parser_previous(parser)->span
                           : KAVAK_SPAN_NONE;
  kavak_parser_finish_node(parser, node);
  if (node) node->span = kavak_span_union(start_span,
                                          kavak_span_union(node->span, end_span));
  return node;
}

static KavakASTNode *parse_fn(KavakParser *parser) {
  KavakASTNode *node = kavak_parser_make_node(parser, TINY_AST_FN);
  const KavakSpan start_span = node ? node->span : KAVAK_SPAN_NONE;
  (void)eat_keyword(parser, KW_FN);
  if (kavak_parser_expect(parser, KAVAK_TOK_IDENT, "expected function name")) {
    node->payload.user.a = previous_token_index(parser);
  }
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, "(", "expected '('");
  (void)kavak_parser_expect_text(parser, KAVAK_TOK_PUNCT, ")", "expected ')'");
  kavak_ast_append_child(node, parse_block(parser));
  kavak_parser_finish_node(parser, node);
  if (node) node->span = kavak_span_union(start_span, node->span);
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

static const KavakToken *token_at(const KavakSema *sema, const uint32_t index) {
  const KavakToken *tokens = kavak_sema_tokens(sema);
  return tokens && index < kavak_sema_token_count(sema) ? &tokens[index] : NULL;
}

static const char *token_text(const KavakSource *source, const KavakToken *token,
                              uint32_t *out_len) {
  if (out_len) *out_len = 0;
  if (!source || !token) return NULL;
  if ((size_t)token->span.start + token->span.len > source->len) return NULL;
  if (out_len) *out_len = token->span.len;
  return source->bytes + token->span.start;
}

static KavakTypeInfo *builtin_type(KavakSema *sema, const uint32_t kind) {
  KavakTypeInfo *type = kavak_ty_builtin(kavak_sema_type_arena(sema), kind);
  return type ? type : kavak_ty_builtin(kavak_sema_type_arena(sema), KAVAK_TY_ANY);
}

static KavakTypeInfo *annotation_type(KavakSema *sema, const KavakASTNode *node) {
  const uint32_t kind = decl_type_kind(node);
  if (kind == KAVAK_TY_INVALID) return builtin_type(sema, KAVAK_TY_ANY);
  return builtin_type(sema, kind);
}

static void diag_unknown_type(KavakSema *sema, const KavakASTNode *node) {
  if (decl_type_kind(node) != KAVAK_TY_INVALID) return;
  const KavakToken *token = token_at(sema, decl_type_token(node));
  kavak_sema_diag(sema, token ? token->span : node->span, "unknown type");
}

static int bind_decl_name(KavakSema *sema, KavakASTNode *node,
                          const uint32_t symbol_kind, KavakTypeInfo *type) {
  const KavakToken *name_token = token_at(sema, (uint32_t)node->payload.user.a);
  uint32_t name_len = 0;
  const char *name = token_text(kavak_sema_source(sema), name_token, &name_len);
  if (!name || !type) return -1;
  return kavak_sema_bind(sema, (KavakSymbol){
    .name = name,
    .name_len = name_len,
    .kind = symbol_kind,
    .decl = node,
    .type = type,
  });
}

static int tiny_resolve_node(KavakSema *sema, KavakASTNode *node) {
  if (!node) return 0;

  if (node->kind == KAVAK_AST_LITERAL) {
    switch (node->payload.literal.literal_kind) {
      case KAVAK_LIT_INT:
        node->type = builtin_type(sema, KAVAK_TY_INT);
        return 0;
      case KAVAK_LIT_FLOAT:
        node->type = builtin_type(sema, KAVAK_TY_DOUBLE);
        return 0;
      case KAVAK_LIT_STRING:
        node->type = builtin_type(sema, KAVAK_TY_STRING);
        return 0;
      default:
        node->type = builtin_type(sema, KAVAK_TY_ANY);
        return 0;
    }
  }

  if (node->kind == TINY_AST_FN) {
    KavakTypeInfo *ret = builtin_type(sema, KAVAK_TY_VOID);
    KavakTypeInfo *fn_ty = kavak_ty_function(kavak_sema_type_arena(sema),
                                             NULL, NULL, 0, ret, 0);
    node->type = fn_ty;
    return bind_decl_name(sema, node, KAVAK_SYM_FUNCTION, fn_ty);
  }

  if (node->kind == TINY_AST_LET) {
    diag_unknown_type(sema, node);
    node->type = annotation_type(sema, node);
    return bind_decl_name(sema, node, KAVAK_SYM_VALUE, node->type);
  }

  return 0;
}

static int type_is(const KavakTypeInfo *type, const uint32_t kind) {
  return type && type->kind == kind;
}

static void infer_node(KavakSema *sema, KavakASTNode *node) {
  if (!node) return;
  for (KavakASTNode *child = node->first_child; child; child = child->next_sibling) {
    infer_node(sema, child);
  }

  if (node->kind == KAVAK_AST_GROUP || node->kind == TINY_AST_EXPR_STMT) {
    node->type = node->first_child ? node->first_child->type : NULL;
    return;
  }

  if (node->kind == KAVAK_AST_BINARY) {
    KavakASTNode *lhs = node->first_child;
    KavakASTNode *rhs = lhs ? lhs->next_sibling : NULL;
    if (node->payload.op.op_id == OP_PLUS) {
      if (!type_is(lhs ? lhs->type : NULL, KAVAK_TY_INT) ||
          !type_is(rhs ? rhs->type : NULL, KAVAK_TY_INT)) {
        kavak_sema_diag(sema, node->span, "operator '+' expects Int operands");
        node->modifiers |= KAVAK_AST_FLAG_ERROR;
      }
      node->type = builtin_type(sema, KAVAK_TY_INT);
      return;
    }
    if (node->payload.op.op_id == OP_EQ) {
      node->type = builtin_type(sema, KAVAK_TY_BOOL);
      return;
    }
    node->type = rhs ? rhs->type : NULL;
    return;
  }

  if (node->kind == TINY_AST_LET) {
    KavakASTNode *init = node->first_child;
    KavakTypeInfo *declared = annotation_type(sema, node);
    node->type = declared;
    if (init && init->type && declared &&
        !kavak_ty_equal_nominal(declared, init->type)) {
      kavak_sema_diag(sema, init->span, "initializer type mismatch");
      node->modifiers |= KAVAK_AST_FLAG_ERROR;
    }
    return;
  }

  if (node->kind == TINY_AST_IF) {
    KavakASTNode *condition = node->first_child;
    if (!type_is(condition ? condition->type : NULL, KAVAK_TY_BOOL)) {
      kavak_sema_diag(sema, condition ? condition->span : node->span,
                      "if condition must be Bool");
      node->modifiers |= KAVAK_AST_FLAG_ERROR;
    }
    node->type = builtin_type(sema, KAVAK_TY_VOID);
    return;
  }

  if (node->kind == TINY_AST_BLOCK || node->kind == KAVAK_AST_ROOT) {
    node->type = builtin_type(sema, KAVAK_TY_VOID);
  }
}

static void tiny_post_sema(KavakSema *sema, KavakASTNode *root) {
  infer_node(sema, root);
}

static const KavakLanguage TINY_LANG = {
  .name = "TinyLang",
  .file_extension = ".tiny",
  .version = 1,
  .lexer = TINY_LEXER,
  .parser = TINY_PARSER,
  .parse_source = parse_file,
  .resolve_node = tiny_resolve_node,
  .post_sema = tiny_post_sema,
};

static const char *kind_name(const uint32_t kind) {
  switch (kind) {
    case KAVAK_TOK_EOF: return "EOF";
    case KAVAK_TOK_IDENT: return "IDENT";
    case KAVAK_TOK_KEYWORD: return "KEYWORD";
    case KAVAK_TOK_INT: return "INT";
    case KAVAK_TOK_FLOAT: return "FLOAT";
    case KAVAK_TOK_STRING: return "STRING";
    case KAVAK_TOK_OP: return "OP";
    case KAVAK_TOK_PUNCT: return "PUNCT";
    case KAVAK_TOK_INVALID: return "INVALID";
    default: return "OTHER";
  }
}

static void print_token(const KavakSource *source, const KavakToken *token) {
  printf("  %-7s", kind_name(token->kind));
  if (token->kind == KAVAK_TOK_KEYWORD) {
    printf(" kw=%u", kavak_token_keyword_id(token));
  } else if (token->kind == KAVAK_TOK_OP) {
    printf(" op=%u", kavak_token_op_id(token));
  }

  printf(" span=[%u,%u)", token->span.start, kavak_span_end(token->span));
  if (token->span.len > 0) {
    printf(" text='");
    fwrite(source->bytes + token->span.start, 1, token->span.len, stdout);
    printf("'");
  }
  printf("\n");
}

static void print_tokens(const KavakResult *result) {
  const KavakSource *source = kavak_source(result);
  const KavakToken *tokens = kavak_tokens(result);
  const size_t count = kavak_token_count(result);
  printf("tokens (%zu):\n", count);
  for (size_t i = 0; i < count; ++i) print_token(source, &tokens[i]);
}

static void print_errors(const KavakResult *result) {
  const uint32_t count = kavak_error_count(result);
  printf("errors: %u\n", count);
  for (uint32_t i = 0; i < count; ++i) {
    printf("  %u:%u: %s\n",
           kavak_error_line(result, i),
           kavak_error_col(result, i),
           kavak_error_message(result, i));
  }
}

static void print_type_sample(KavakSession *session) {
  KavakTypeArena *arena = kavak_session_type_arena(session);
  KavakTypeInfo *int_ty = kavak_ty_builtin(arena, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(arena, KAVAK_TY_STRING);
  KavakTypeInfo *bool_ty = kavak_ty_builtin(arena, KAVAK_TY_BOOL);
  KavakTypeInfo *positional[] = { int_ty, string_ty };
  KavakRecordField named[] = {
    { .name = "ok", .type = bool_ty },
  };
  KavakTypeInfo *record = kavak_ty_record(arena, positional, 2, named, 1);
  char buf[128];
  (void)kavak_ty_to_string(record, buf, sizeof(buf));
  printf("type sample: %s\n", buf);
}

static int run_case(KavakSession *session, const char *label,
                    const char *code, const int want_errors) {
  printf("\n== %s ==\n%s\n", label, code);
  KavakResult *result = kavak_analyze(session, code, label);
  if (!result) {
    fprintf(stderr, "analysis failed before result creation\n");
    return 1;
  }

  print_tokens(result);
  print_errors(result);
  if (kavak_error_count(result) == 0) {
    printf("ast dump:\n");
    kavak_dump_text(result, stdout);
  }

  const int ok = want_errors ? kavak_error_count(result) != 0
                             : kavak_error_count(result) == 0;
  kavak_result_free(result);
  return ok ? 0 : 1;
}

int main(void) {
  static const char *GOOD =
    "// lexer: comments, strings, numbers, operators\n"
    "fn main() {\n"
    "  let seed: Int = 2;\n"
    "  let answer: Int = 40 + seed;\n"
    "  if answer == 42 {\n"
    "    let label: String = \"ok\";\n"
    "    let matched: Bool = answer == 42;\n"
    "  } else {\n"
    "    let label: String = \"no\";\n"
    "    let matched: Bool = answer == 0;\n"
    "  }\n"
    "}\n";

  static const char *BAD =
    "fn main() {\n"
    "  let broken: Int = \"oops\";\n"
    "}\n";

  KavakSession *session = kavak_session_new(&TINY_LANG);
  if (!session) return 1;

  printf("kavak %s / %s descriptor v%u\n",
         kavak_version(), TINY_LANG.name, TINY_LANG.version);
  print_type_sample(session);

  int failed = 0;
  failed += run_case(session, "ok.tiny", GOOD, 0);
  failed += run_case(session, "bad.tiny", BAD, 1);

  kavak_session_free(session);
  return failed == 0 ? 0 : 1;
}
