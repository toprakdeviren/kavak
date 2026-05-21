// SPDX-License-Identifier: MIT
/**
 * @file tests/test_lexer_offside.c
 * @brief Offside-rule lexer tests.
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

enum { PY_KW_DEF = 1, PY_KW_IF, PY_KW_RETURN };

static const KavakKeyword PY_KEYWORDS[] = {
  { "def", PY_KW_DEF },
  { "if", PY_KW_IF },
  { "return", PY_KW_RETURN },
};

static const KavakCommentRule PY_COMMENTS[] = {
  { .open = "#", .close = "\n", .nest = 0, .is_doc = 0 },
};

static const KavakOffsideConfig PY_OFFSIDE = {
  .unit = KAVAK_INDENT_SPACES,
  .dedent_on_lower_or_equal = 0,
};

static const KavakLexerConfig PY_CONFIG = {
  .keywords      = PY_KEYWORDS,
  .keyword_count = sizeof(PY_KEYWORDS) / sizeof(*PY_KEYWORDS),
  .comments      = PY_COMMENTS,
  .comment_count = sizeof(PY_COMMENTS) / sizeof(*PY_COMMENTS),
  .offside       = &PY_OFFSIDE,
};

static int run_lex(const char *bytes, KavakTokenVec *tokens, KavakDiagVec *diags) {
  KavakSource source;
  if (kavak_source_init(&source, bytes, strlen(bytes), "<offside-test>") != 0) return -1;
  const int rc = kavak_lex(&source, &PY_CONFIG, tokens, diags);
  kavak_source_free(&source);
  return rc;
}

static int test_python_offside_smoke(void) {
  static const uint32_t expected[] = {
    KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_PUNCT, KAVAK_TOK_NEWLINE,
    KAVAK_TOK_INDENT, KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_PUNCT,
    KAVAK_TOK_NEWLINE, KAVAK_TOK_INDENT, KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT,
    KAVAK_TOK_PUNCT, KAVAK_TOK_NEWLINE, KAVAK_TOK_INDENT, KAVAK_TOK_KEYWORD,
    KAVAK_TOK_IDENT, KAVAK_TOK_NEWLINE, KAVAK_TOK_DEDENT, KAVAK_TOK_KEYWORD,
    KAVAK_TOK_IDENT, KAVAK_TOK_NEWLINE, KAVAK_TOK_DEDENT, KAVAK_TOK_DEDENT,
    KAVAK_TOK_EOF,
  };
  const char *src =
    "def f:\n"
    "    if x:\n"
    "        if y:\n"
    "            return y\n"
    "        return x";

  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex(src, &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == sizeof(expected) / sizeof(*expected), "token count");
  for (uint32_t i = 0; i < tokens.count; ++i) {
    ASSERT(tokens.items[i].kind == expected[i], "Python offside kind sequence");
  }
  ASSERT(tokens.items[18].kind == KAVAK_TOK_DEDENT,
         "one DEDENT between sibling return lines");
  ASSERT(tokens.items[22].kind == KAVAK_TOK_DEDENT &&
         tokens.items[23].kind == KAVAK_TOK_DEDENT,
         "two DEDENTs drain at EOF");
  ASSERT(tokens.items[24].span.start == strlen(src), "EOF anchors at len");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_comment_only_line_does_not_indent(void) {
  static const uint32_t expected[] = {
    KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_PUNCT, KAVAK_TOK_NEWLINE,
    KAVAK_TOK_INDENT, KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_NEWLINE,
    KAVAK_TOK_DEDENT, KAVAK_TOK_EOF,
  };
  const char *src =
    "def f:\n"
    "    # comment-only line\n"
    "    return x";

  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex(src, &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == sizeof(expected) / sizeof(*expected), "token count");
  for (uint32_t i = 0; i < tokens.count; ++i) {
    ASSERT(tokens.items[i].kind == expected[i], "comment-only line kind sequence");
  }
  ASSERT(diags.count == 0, "comment-only line is ignored by layout");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_misaligned_dedent_reports_diag(void) {
  const char *src =
    "def f:\n"
    "    if x:\n"
    "        return x\n"
    "      return y";

  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex(src, &tokens, &diags) == 0, "rc 0");
  ASSERT(diags.count == 1, "one misaligned-dedent diag");
  ASSERT(tokens.count >= 2, "tokens still emitted after recovery");
  ASSERT(tokens.items[tokens.count - 1].kind == KAVAK_TOK_EOF, "EOF still emitted");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_indent_depth_overflow_is_fatal(void) {
  enum { LEVELS = 130, BUF_SIZE = (LEVELS * (LEVELS + 1)) / 2 + LEVELS * 10 + 1 };
  char src[BUF_SIZE];
  size_t pos = 0;
  for (uint32_t level = 0; level < LEVELS; ++level) {
    for (uint32_t i = 0; i < level; ++i) src[pos++] = ' ';
    memcpy(src + pos, "return x\n", 9);
    pos += 9;
  }
  src[pos] = '\0';

  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex(src, &tokens, &diags) == -1, "deep indent is fatal");
  int found = 0;
  for (uint32_t i = 0; i < diags.count; ++i) {
    if (strcmp(diags.items[i].message, "indentation nesting too deep") == 0) {
      found = 1;
      break;
    }
  }
  ASSERT(found, "indent depth diagnostic emitted");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_python_offside_smoke();
  fails += test_comment_only_line_does_not_indent();
  fails += test_misaligned_dedent_reports_diag();
  fails += test_indent_depth_overflow_is_fatal();

  if (fails == 0) {
    printf("  ✓ test_lexer_offside: 4/4 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_lexer_offside: %d failure(s)\n", fails);
  return 1;
}
