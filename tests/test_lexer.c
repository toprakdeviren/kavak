// SPDX-License-Identifier: MIT
/**
 * @file tests/test_lexer.c
 * @brief kavak_lex driver and scanner tests.
 *
 * Two groups of tests:
 *
 * LOOP SHAPE:
 *   - empty source → just EOF
 *   - whitespace-only → just EOF
 *   - one bad byte → INVALID + diag + EOF, advance worked
 *   - bad bytes interleaved with whitespace → recovery doesn't desync
 *   - NULL diags pointer → no crash, INVALID still emitted
 *
 * RECOGNITION:
 *   - bare identifier → one IDENT spanning the run
 *   - keyword table → IDENT retagged to KEYWORD (v = id)
 *   - keyword prefix ("lettuce" ≠ "let") → IDENT, not KEYWORD
 *   - custom is_ident_start hook → "$x" is one IDENT
 *   - decoder-backed Unicode identifier hooks admit non-ASCII names
 *   - ident then unknown byte → IDENT, INVALID, EOF (boundary + recovery)
 *
 * COMMENTS:
 *   - line comment skipped, newline left for whitespace; EOF/CR-terminated ok
 *   - optional Unicode newline policy terminates line comments
 *   - block comment skipped; lone '/' is INVALID (open needs full delim)
 *   - unterminated block comment → INVALID + diag at EOF
 *   - same bytes, nest off vs on → clean close vs unterminated
 *   - doc comment ("///" beats "//") emitted iff keep_doc_comments
 *   - keep_comments retains every style as COMMENT; errors stay INVALID
 *
 * OPERATORS:
 *   - single operator → OP (v = op_id)
 *   - longest-match: "==" beats "=", "<<=" beats "<<" beats "<"
 *   - multi-byte UTF-8 spelling ("≠") matched as raw bytes
 *   - operator/ident boundary ("a+b")
 *   - structural punctuation → PUNCT (v = ASCII byte), with and without operators
 *
 * NUMBERS:
 *   - zero-config decimal integers
 *   - gated base prefixes; flag-off `0x10` becomes INT `0`, IDENT `x10`
 *   - underscores only between digits
 *   - decimal floats + e/E exponents
 *   - malformed resync (`1.2.3`, `0x`) and longest-match suffix consumption
 *   - underscore runs between digits (`1__2`); trailing `_` resyncs
 *   - leading-dot floats (`.5`, `.5e3`) behind KAVAK_NUM_LEADING_DOT
 *
 * STRINGS:
 *   - normal strings validate configured escapes
 *   - single-quote rule emits CHAR
 *   - longest-open triple quote beats the plain quote rule
 *   - raw string open beats identifier scanning and skips escapes
 *   - invalid escape reports a diag but still emits STRING
 *   - unterminated strings emit INVALID and recover
 *   - IDENT-flagged rules emit quoted identifiers (backticks), no
 *     keyword retag, string-style unterminated recovery
 *
 * INTERPOLATION + AUTO-SEMI:
 *   - template strings emit STRING fragments around bare `$name`
 *     and braced `${expr}` tokens
 *   - braced interpolation balances inner braces
 *   - lone `$` (no ident/brace after) is literal text in dual-form
 *     rules; explicit-close rules always enter the expression
 *   - marker kinds remap fragments (head/interior/tail v-tags), `${`,
 *     `}`, and `$ident` entries onto user token kinds
 *   - emit_newlines yields a NEWLINE per physical newline — blank
 *     lines and `${...}` holes included, comments and EOF excluded
 *   - Go-style auto-semi emits configured virtual semicolons before
 *     newline / EOF after terminating token kinds
 *
 * SMOKE:
 *   - TinyLang config exercises keywords, ops, comments, numbers,
 *     punctuation, and strings in one full token-kind stream
 *
 * The loop-shape assertions stay green with the full scanner set.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ASSERT(cond, msg)                                                  \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "  ✗ %s:%d  %s\n", __FILE__, __LINE__, (msg));      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

/* Loop-shape tests use the empty config — no keywords, NULL ident hooks
 * (so the ASCII default applies). Recognition tests below supply
 * their own configs. */
static const KavakLexerConfig EMPTY_CONFIG = {0};

/* Keyword table for the retag tests. */
static const KavakKeyword KW_TABLE[] = {
  { "let", 7 },
  { "if",  3 },
};
static const KavakLexerConfig KW_CONFIG = {
  .keywords      = KW_TABLE,
  .keyword_count = sizeof(KW_TABLE) / sizeof(*KW_TABLE),
};

/* Soft (contextual) keywords: id 2 ("soft") is listed in soft_keywords, so it
 * lexes as IDENT carrying its id; id 1 ("hard") retags to KEYWORD as usual. */
static const KavakKeyword SOFT_KW_TABLE[] = {
  { "hard", 1 },
  { "soft", 2 },
};
static const uint32_t SOFT_KW_IDS[] = { 2 };
static const KavakLexerConfig SOFT_KW_CONFIG = {
  .keywords           = SOFT_KW_TABLE,
  .keyword_count      = sizeof(SOFT_KW_TABLE) / sizeof(*SOFT_KW_TABLE),
  .soft_keywords      = SOFT_KW_IDS,
  .soft_keyword_count = sizeof(SOFT_KW_IDS) / sizeof(*SOFT_KW_IDS),
};

/* Custom ident predicates that admit '$' — proves the hook is dispatched
 * (the ASCII default rejects '$'). */
static int dollar_is_ident_start(const uint32_t cp) {
  return kavak_ascii_is_ident_start(cp) || cp == '$';
}
static int dollar_is_ident_cont(const uint32_t cp) {
  return kavak_ascii_is_ident_cont(cp) || cp == '$';
}
static const KavakLexerConfig DOLLAR_CONFIG = {
  .is_ident_start = dollar_is_ident_start,
  .is_ident_cont  = dollar_is_ident_cont,
};
static const KavakLexerConfig UNICODE_IDENT_CONFIG = {
  .is_ident_start = kavak_unicode_is_ident_start,
  .is_ident_cont  = kavak_unicode_is_ident_cont,
};

/* Comment configs. C-style line "//" plus a slash-star/star-slash
 * block rule. The block rule here does NOT nest. */
static const KavakCommentRule CMT_RULES[] = {
  { .open = "//", .close = "\n", .nest = 0, .is_doc = 0 },
  { .open = "/*", .close = "*/", .nest = 0, .is_doc = 0 },
};
static const KavakLexerConfig CMT_CONFIG = {
  .comments      = CMT_RULES,
  .comment_count = sizeof(CMT_RULES) / sizeof(*CMT_RULES),
};
static const KavakLexerConfig CMT_UNICODE_NEWLINE_CONFIG = {
  .comments      = CMT_RULES,
  .comment_count = sizeof(CMT_RULES) / sizeof(*CMT_RULES),
  .newline_flags = KAVAK_NEWLINE_DEFAULT | KAVAK_NEWLINE_UNICODE,
};

/* Same block delimiters, nesting enabled (Rust-style). */
static const KavakCommentRule CMT_NEST_RULES[] = {
  { .open = "/*", .close = "*/", .nest = 1, .is_doc = 0 },
};
static const KavakLexerConfig CMT_NEST_CONFIG = {
  .comments      = CMT_NEST_RULES,
  .comment_count = 1,
};

/* "///" doc line vs "//" plain line — exercises longest-open precedence
 * and the keep_doc_comments gate. */
static const KavakCommentRule CMT_DOC_RULES[] = {
  { .open = "//",  .close = "\n", .nest = 0, .is_doc = 0 },
  { .open = "///", .close = "\n", .nest = 0, .is_doc = 1 },
};
static const KavakLexerConfig CMT_DOC_KEEP = {
  .comments          = CMT_DOC_RULES,
  .comment_count     = sizeof(CMT_DOC_RULES) / sizeof(*CMT_DOC_RULES),
  .keep_doc_comments = 1,
};
static const KavakLexerConfig CMT_DOC_SKIP = {
  .comments          = CMT_DOC_RULES,
  .comment_count     = sizeof(CMT_DOC_RULES) / sizeof(*CMT_DOC_RULES),
  .keep_doc_comments = 0,
};

/* keep_comments retains EVERY comment style, doc or not. */
static const KavakLexerConfig CMT_KEEP_ALL = {
  .comments      = CMT_RULES,
  .comment_count = sizeof(CMT_RULES) / sizeof(*CMT_RULES),
  .keep_comments = 1,
};

/* Operator table. Listed shortest-first on purpose: a naive
 * first-match scan would stop at "<" / "=", so passing the longest-match
 * tests proves table order does not affect matching. "\xE2\x89\xA0" is "≠"
 * (U+2260) — a 3-byte UTF-8 spelling. prec/assoc/flags are Pratt
 * data the lexer ignores; only text + op_id matter here. */
enum {
  OP_LT = 1, OP_SHL, OP_SHL_EQ, OP_ASSIGN, OP_EQ, OP_PLUS, OP_NE,
};
static const KavakOperator OP_TABLE[] = {
  { "<",          10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_LT     },
  { "<<",         20, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_SHL    },
  { "<<=",         5, KAVAK_ASSOC_RIGHT, KAVAK_OP_FLAG_INFIX, OP_SHL_EQ },
  { "=",           5, KAVAK_ASSOC_RIGHT, KAVAK_OP_FLAG_INFIX, OP_ASSIGN },
  { "==",         10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_EQ     },
  { "+",          30, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, OP_PLUS   },
  { "\xE2\x89\xA0", 10, KAVAK_ASSOC_LEFT, KAVAK_OP_FLAG_INFIX, OP_NE    },
};
static const KavakLexerConfig OP_CONFIG = {
  .operators      = OP_TABLE,
  .operator_count = sizeof(OP_TABLE) / sizeof(*OP_TABLE),
};

/* Number configs. Zero flags already mean decimal-integer-only,
 * so EMPTY_CONFIG doubles as the baseline number config. */
static const KavakLexerConfig NUM_BASE_CONFIG = {
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_BASE_HEX |
             KAVAK_NUM_BASE_OCT | KAVAK_NUM_BASE_BIN |
             KAVAK_NUM_UNDERSCORES,
  },
};
static const KavakLexerConfig NUM_STRICT_CONFIG = {
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_BASE_HEX |
             KAVAK_NUM_STRICT_INVALID_DIGITS,
  },
};
static const KavakLexerConfig NUM_FLOAT_CONFIG = {
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_FLOAT |
             KAVAK_NUM_EXPONENT | KAVAK_NUM_UNDERSCORES,
  },
};
static const char *const NUM_SUFFIXES[] = { "u8", "u" };
static const KavakLexerConfig NUM_SUFFIX_CONFIG = {
  .numbers = {
    .flags        = KAVAK_NUM_BASE_DEC,
    .suffixes     = NUM_SUFFIXES,
    .suffix_count = sizeof(NUM_SUFFIXES) / sizeof(*NUM_SUFFIXES),
  },
};
static const char *const NUM_SUFFIXES_UNSORTED[] = { "u", "u64" };
static const KavakLexerConfig NUM_SUFFIX_UNSORTED_CONFIG = {
  .numbers = {
    .flags        = KAVAK_NUM_BASE_DEC,
    .suffixes     = NUM_SUFFIXES_UNSORTED,
    .suffix_count = sizeof(NUM_SUFFIXES_UNSORTED) / sizeof(*NUM_SUFFIXES_UNSORTED),
  },
};
static const char *const NUM_SUFFIXES_U[] = { "u" };
static const KavakLexerConfig NUM_SUFFIX_BOUNDARY_CONFIG = {
  .numbers = {
    .flags        = KAVAK_NUM_BASE_DEC | KAVAK_NUM_SUFFIX_REQUIRES_BOUNDARY,
    .suffixes     = NUM_SUFFIXES_U,
    .suffix_count = sizeof(NUM_SUFFIXES_U) / sizeof(*NUM_SUFFIXES_U),
  },
};
static const KavakLexerConfig NUM_SUFFIX_BOUNDARY_CONFIG_OFF = {
  .numbers = {
    .flags        = KAVAK_NUM_BASE_DEC,  /* same suffix, no boundary flag */
    .suffixes     = NUM_SUFFIXES_U,
    .suffix_count = sizeof(NUM_SUFFIXES_U) / sizeof(*NUM_SUFFIXES_U),
  },
};

/* Leading-dot floats plus a `..` range operator — the first dot of `..`
 * is not followed by a digit, so the operator table still claims it. */
enum { OP_RANGE = 21 };
static const KavakOperator RANGE_OPS[] = {
  { "..", 10, KAVAK_ASSOC_LEFT, KAVAK_OP_FLAG_INFIX, OP_RANGE },
};
static const KavakLexerConfig NUM_LEADING_DOT_CONFIG = {
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_FLOAT |
             KAVAK_NUM_EXPONENT | KAVAK_NUM_LEADING_DOT,
  },
  .operators      = RANGE_OPS,
  .operator_count = sizeof(RANGE_OPS) / sizeof(*RANGE_OPS),
};

/* String rules. The plain quote rule intentionally appears
 * before the triple-quote rule; passing the triple test proves the scanner
 * picks the longest matching opener rather than first table entry. */
static const KavakEscape STR_ESCAPES[] = {
  { 'n',  0x0A },
  { 't',  0x09 },
  { '\\', '\\' },
  { '"',  '"' },
  { '\'', '\'' },
};
static const KavakStringRule STR_RULES[] = {
  {
    .open = "\"", .close = "\"", .flags = 0,
    .escapes = STR_ESCAPES,
    .escape_count = sizeof(STR_ESCAPES) / sizeof(*STR_ESCAPES),
  },
  {
    .open = "'", .close = "'", .flags = 0,
    .escapes = STR_ESCAPES,
    .escape_count = sizeof(STR_ESCAPES) / sizeof(*STR_ESCAPES),
  },
  {
    .open = "r\"", .close = "\"", .flags = KAVAK_STR_FLAG_RAW,
  },
  {
    .open = "\"\"\"", .close = "\"\"\"",
    .flags = KAVAK_STR_FLAG_TRIPLE | KAVAK_STR_FLAG_MULTILINE,
    .escapes = STR_ESCAPES,
    .escape_count = sizeof(STR_ESCAPES) / sizeof(*STR_ESCAPES),
  },
};
static const KavakLexerConfig STR_CONFIG = {
  .strings           = STR_RULES,
  .string_count = sizeof(STR_RULES) / sizeof(*STR_RULES),
};

/* Backtick quoted-identifier rule (Kotlin-style). RAW keeps `\` a plain
 * byte; the keyword table proves quoted names never retag. */
static const KavakStringRule QUOTED_IDENT_RULES[] = {
  {
    .open = "`", .close = "`",
    .flags = KAVAK_STR_FLAG_IDENT | KAVAK_STR_FLAG_RAW,
  },
};
static const KavakLexerConfig QUOTED_IDENT_CONFIG = {
  .keywords          = KW_TABLE,
  .keyword_count     = sizeof(KW_TABLE) / sizeof(*KW_TABLE),
  .strings           = QUOTED_IDENT_RULES,
  .string_count = sizeof(QUOTED_IDENT_RULES) / sizeof(*QUOTED_IDENT_RULES),
};

static const KavakStringRule TEMPLATE_RULES[] = {
  {
    .open = "\"", .close = "\"",
    .flags = KAVAK_STR_FLAG_TEMPLATE,
    .interp_open = "$",
    .interp_close = NULL,
    .escapes = STR_ESCAPES,
    .escape_count = sizeof(STR_ESCAPES) / sizeof(*STR_ESCAPES),
  },
};
static const KavakOperator TEMPLATE_OPS[] = {
  { "+", 10, KAVAK_ASSOC_LEFT, KAVAK_OP_FLAG_INFIX, OP_PLUS },
};
static const KavakLexerConfig TEMPLATE_CONFIG = {
  .strings           = TEMPLATE_RULES,
  .string_count = sizeof(TEMPLATE_RULES) / sizeof(*TEMPLATE_RULES),
  .operators         = TEMPLATE_OPS,
  .operator_count    = sizeof(TEMPLATE_OPS) / sizeof(*TEMPLATE_OPS),
};

/* Kotlin-shaped marker remapping: fragments and interpolation markers
 * land on user kinds so a parser can rebuild template structure. */
enum {
  MARK_FRAGMENT     = KAVAK_TOK_USER_BASE + 1,
  MARK_INTERP_OPEN  = KAVAK_TOK_USER_BASE + 2,
  MARK_INTERP_CLOSE = KAVAK_TOK_USER_BASE + 3,
  MARK_BARE_IDENT   = KAVAK_TOK_USER_BASE + 4,
};
static const KavakStringRule MARKER_TEMPLATE_RULES[] = {
  {
    .open = "\"", .close = "\"",
    .flags = KAVAK_STR_FLAG_TEMPLATE,
    .interp_open = "$",
    .interp_close = NULL,
    .escapes = STR_ESCAPES,
    .escape_count = sizeof(STR_ESCAPES) / sizeof(*STR_ESCAPES),
    .fragment_kind     = MARK_FRAGMENT,
    .interp_open_kind  = MARK_INTERP_OPEN,
    .interp_close_kind = MARK_INTERP_CLOSE,
    .bare_ident_kind   = MARK_BARE_IDENT,
  },
};
static const KavakLexerConfig MARKER_TEMPLATE_CONFIG = {
  .strings           = MARKER_TEMPLATE_RULES,
  .string_count = sizeof(MARKER_TEMPLATE_RULES) / sizeof(*MARKER_TEMPLATE_RULES),
  .operators         = TEMPLATE_OPS,
  .operator_count    = sizeof(TEMPLATE_OPS) / sizeof(*TEMPLATE_OPS),
};

/* Swift-style explicit-close interpolation — `\(expr)` always enters
 * the expression, whatever follows the marker. */
static const KavakStringRule SWIFT_TEMPLATE_RULES[] = {
  {
    .open = "\"", .close = "\"",
    .flags = KAVAK_STR_FLAG_TEMPLATE,
    .interp_open = "\\(",
    .interp_close = ")",
  },
};
static const KavakLexerConfig SWIFT_TEMPLATE_CONFIG = {
  .strings           = SWIFT_TEMPLATE_RULES,
  .string_count = sizeof(SWIFT_TEMPLATE_RULES) / sizeof(*SWIFT_TEMPLATE_RULES),
};

/* emit_newlines without offside/auto_semi: every physical newline emits
 * a NEWLINE token, including inside `${...}` holes. */
static const KavakLexerConfig EMIT_NEWLINES_CONFIG = {
  .strings           = TEMPLATE_RULES,
  .string_count = sizeof(TEMPLATE_RULES) / sizeof(*TEMPLATE_RULES),
  .operators         = TEMPLATE_OPS,
  .operator_count    = sizeof(TEMPLATE_OPS) / sizeof(*TEMPLATE_OPS),
  .comments          = CMT_RULES,
  .comment_count     = sizeof(CMT_RULES) / sizeof(*CMT_RULES),
  .emit_newlines     = 1,
};

enum { PUNCT_VIRTUAL_SEMI = 99 };

static int auto_semi_terminates(const KavakToken *token) {
  if (!token) return 0;
  if (token->kind == KAVAK_TOK_IDENT || token->kind == KAVAK_TOK_INT ||
      token->kind == KAVAK_TOK_STRING) {
    return 1;
  }
  return token->kind == KAVAK_TOK_PUNCT && token->v == ')';
}

static const KavakAutoSemicolon AUTO_SEMI = {
  .is_terminating = auto_semi_terminates,
  .emit_kind      = KAVAK_TOK_PUNCT,
  .emit_v         = PUNCT_VIRTUAL_SEMI,
};

static const KavakLexerConfig AUTO_SEMI_CONFIG = {
  .operators      = OP_TABLE,
  .operator_count = sizeof(OP_TABLE) / sizeof(*OP_TABLE),
  .auto_semi      = &AUTO_SEMI,
};

enum {
  TINY_KW_FN = 1, TINY_KW_LET, TINY_KW_IF, TINY_KW_ELSE,
  TINY_OP_PLUS = 11, TINY_OP_STAR, TINY_OP_EQ, TINY_OP_AND, TINY_OP_ASSIGN,
};
static const KavakKeyword TINY_KEYWORDS[] = {
  { "fn", TINY_KW_FN },
  { "let", TINY_KW_LET },
  { "if", TINY_KW_IF },
  { "else", TINY_KW_ELSE },
};
static const KavakOperator TINY_OPS[] = {
  { "+",  20, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, TINY_OP_PLUS   },
  { "*",  30, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, TINY_OP_STAR   },
  { "==", 10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX, TINY_OP_EQ     },
  { "&&",  5, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX |
                                      KAVAK_OP_FLAG_SHORT_CIRCUIT, TINY_OP_AND },
  { "=",   1, KAVAK_ASSOC_RIGHT, KAVAK_OP_FLAG_INFIX, TINY_OP_ASSIGN },
};
static const KavakLexerConfig TINY_CONFIG = {
  .keywords          = TINY_KEYWORDS,
  .keyword_count     = sizeof(TINY_KEYWORDS) / sizeof(*TINY_KEYWORDS),
  .operators         = TINY_OPS,
  .operator_count    = sizeof(TINY_OPS) / sizeof(*TINY_OPS),
  .comments          = CMT_RULES,
  .comment_count     = sizeof(CMT_RULES) / sizeof(*CMT_RULES),
  .numbers = {
    .flags = KAVAK_NUM_BASE_DEC | KAVAK_NUM_FLOAT |
             KAVAK_NUM_EXPONENT | KAVAK_NUM_UNDERSCORES,
  },
  .strings           = STR_RULES,
  .string_count = sizeof(STR_RULES) / sizeof(*STR_RULES),
};

static int run_lex_with(const KavakLexerConfig *config, const char *bytes,
                        KavakTokenVec *tokens, KavakDiagVec *diags) {
  KavakSource source;
  if (kavak_source_init(&source, bytes, strlen(bytes), "<test>") != 0) return -1;
  const int rc = kavak_lex(&source, config, tokens, diags);
  kavak_source_free(&source);
  return rc;
}

static int run_lex(const char *bytes, KavakTokenVec *tokens, KavakDiagVec *diags) {
  return run_lex_with(&EMPTY_CONFIG, bytes, tokens, diags);
}

static int test_empty_source(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex("", &tokens, &diags) == 0, "lex empty: rc 0");
  ASSERT(tokens.count == 1, "empty source: only EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_EOF, "kind = EOF");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 0,
         "EOF span anchors at end-of-source, zero length");
  ASSERT(diags.count == 0, "no diags on clean input");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_invalid_config_rejected(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(NULL, "x", &tokens, &diags) == -1, "NULL config rejected");

  const KavakLexerConfig bad_keywords = {
    .keywords = NULL,
    .keyword_count = 1,
  };
  ASSERT(run_lex_with(&bad_keywords, "x", &tokens, &diags) == -1,
         "missing keyword table rejected");

  const KavakOperator bad_ops[] = {
    { "", 1, 1, KAVAK_ASSOC_LEFT, KAVAK_OP_FLAG_INFIX },
  };
  const KavakLexerConfig bad_operator = {
    .operators = bad_ops,
    .operator_count = 1,
  };
  ASSERT(run_lex_with(&bad_operator, "+", &tokens, &diags) == -1,
         "empty operator spelling rejected");

  /* soft_keyword_count > 0 with a NULL table: the identifier retagger would
   * dereference soft_keywords[j] when "let" matches a keyword. Validation must
   * reject the config up front instead of crashing. */
  const KavakLexerConfig bad_soft_keywords = {
    .keywords           = KW_TABLE,
    .keyword_count      = sizeof(KW_TABLE) / sizeof(*KW_TABLE),
    .soft_keywords      = NULL,
    .soft_keyword_count = 1,
  };
  ASSERT(run_lex_with(&bad_soft_keywords, "let", &tokens, &diags) == -1,
         "soft_keyword_count>0 with NULL table rejected");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_whitespace_only(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* All ASCII whitespace forms — space, tab, LF, CR, FF, VT. */
  ASSERT(run_lex(" \t\n\r\f\v  \t\n", &tokens, &diags) == 0, "lex ws: rc 0");
  ASSERT(tokens.count == 1, "whitespace: only EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_EOF, "kind = EOF");
  ASSERT(tokens.items[0].span.start == 10, "EOF anchors at len(input)");
  ASSERT(diags.count == 0, "whitespace produces no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_one_unknown_byte(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex("@", &tokens, &diags) == 0, "lex '@': rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "first = INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "INVALID covers exactly the bad byte");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "EOF anchors after");
  ASSERT(tokens.items[1].span.start == 1, "EOF after the bad byte");
  ASSERT(diags.count == 1, "one diag for one bad byte");
  ASSERT(diags.items[0].severity == KAVAK_SEV_ERROR, "diag is error");
  ASSERT(diags.items[0].span.start == 0 && diags.items[0].span.len == 1,
         "diag span matches INVALID span");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_recovery_no_desync(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Three bad bytes separated by whitespace. The single-byte advance
   * keeps each one isolated — recovery doesn't swallow the gaps. */
  ASSERT(run_lex("@ # $", &tokens, &diags) == 0, "lex '@ # $': rc 0");
  ASSERT(tokens.count == 4, "3 INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0, "[0] at byte 0");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INVALID, "[1] INVALID");
  ASSERT(tokens.items[1].span.start == 2, "[1] at byte 2 (after space)");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INVALID, "[2] INVALID");
  ASSERT(tokens.items[2].span.start == 4, "[2] at byte 4");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF,     "[3] EOF");
  ASSERT(diags.count == 3, "one diag per bad byte");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_null_diags_no_crash(void) {
  /* Per the kavak_lex contract, out_diags may be NULL — diags are
   * discarded. INVALID tokens still record over the bad span. */
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);

  ASSERT(run_lex("@", &tokens, NULL) == 0, "NULL diags: still rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF still emitted");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "INVALID without diag sink");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF,     "EOF anchors");

  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Recognition tests ───────────────────────────────────────────────── */

static int test_identifier_basic(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex("foo123", &tokens, &diags) == 0, "lex ident: rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "first = IDENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 6,
         "IDENT spans the whole run");
  ASSERT(tokens.items[0].v == 0, "IDENT carries v = 0");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "EOF after");
  ASSERT(diags.count == 0, "clean ident, no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_keyword_match(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&KW_CONFIG, "let if foo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "KEYWORD KEYWORD IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_KEYWORD, "[0] KEYWORD");
  ASSERT(tokens.items[0].v == 7, "[0] keyword id = let");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_KEYWORD, "[1] KEYWORD");
  ASSERT(tokens.items[1].v == 3, "[1] keyword id = if");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT, "[2] IDENT (foo)");
  ASSERT(tokens.items[2].v == 0, "[2] IDENT v = 0");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_soft_keyword(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&SOFT_KW_CONFIG, "hard soft other", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "KEYWORD IDENT IDENT EOF");
  /* Hard keyword retags to KEYWORD as usual. */
  ASSERT(tokens.items[0].kind == KAVAK_TOK_KEYWORD, "[0] hard => KEYWORD");
  ASSERT(kavak_token_keyword_id(&tokens.items[0]) == 1, "[0] keyword id");
  /* Soft keyword stays IDENT but carries its id for contextual parsing. */
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] soft => IDENT");
  ASSERT(kavak_token_soft_keyword_id(&tokens.items[1]) == 2, "[1] soft id carried");
  /* A plain identifier is IDENT with no soft id. */
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT, "[2] other => IDENT");
  ASSERT(kavak_token_soft_keyword_id(&tokens.items[2]) == 0, "[2] no soft id");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_keyword_prefix(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* "lettuce" must NOT match keyword "let" — lookup is on the whole
   * maximal ident span, not a prefix. */
  ASSERT(run_lex_with(&KW_CONFIG, "lettuce", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "IDENT, not KEYWORD");
  ASSERT(tokens.items[0].span.len == 7, "spans all 7 bytes");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_custom_ident_start(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* '$' is an ident char only because DOLLAR_CONFIG says so. "$x" is one
   * IDENT; under the ASCII default '$' would be INVALID. */
  ASSERT(run_lex_with(&DOLLAR_CONFIG, "$x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "IDENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 2,
         "IDENT spans \"$x\"");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_unicode_ident_hooks(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  const char *src = "\xC3\xB1" "ame a" "\xCC\x81" " " "\xE4\xB8\xAD" "_1";
  ASSERT(run_lex_with(&UNICODE_IDENT_CONFIG, src, &tokens, &diags) == 0,
         "lex Unicode idents: rc 0");
  ASSERT(tokens.count == 4, "three Unicode IDENT tokens + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] ñame IDENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] spans full UTF-8 ñame");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] a + combining mark IDENT");
  ASSERT(tokens.items[1].span.start == 6 && tokens.items[1].span.len == 3,
         "[1] combining mark continues identifier");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT, "[2] CJK + underscore + digit IDENT");
  ASSERT(tokens.items[2].span.start == 10 && tokens.items[2].span.len == 5,
         "[2] spans full UTF-8 CJK identifier");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "Unicode identifiers produce no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_ident_then_unknown(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* The ident ends at the first non-ident byte; the '@' still reports as
   * INVALID. Proves the maximal-munch boundary and recovery coexist. */
  ASSERT(run_lex("foo@", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "IDENT + INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] IDENT covers \"foo\"");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INVALID, "[1] INVALID (@)");
  ASSERT(tokens.items[1].span.start == 3 && tokens.items[1].span.len == 1,
         "[1] INVALID over the '@'");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 1, "one diag for the bad byte");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Comment-scanner tests ───────────────────────────────────────────── */

static int test_line_comment_skipped(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* The "//" comment runs to the newline and is skipped; the '\n' it
   * leaves behind is plain whitespace here, so only "foo" survives. */
  ASSERT(run_lex_with(&CMT_CONFIG, "// comment\nfoo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF (comment skipped)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT");
  ASSERT(tokens.items[0].span.start == 11 && tokens.items[0].span.len == 3,
         "[0] IDENT is \"foo\", after the comment + newline");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "comments produce no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_line_comment_eof(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* A line comment terminated by EOF (no trailing newline) is fine — only
   * block comments error at EOF. */
  ASSERT(run_lex_with(&CMT_CONFIG, "// trailing", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 1, "just EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_EOF, "EOF");
  ASSERT(diags.count == 0, "no diag for an EOF-terminated line comment");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_line_comment_cr_terminated(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&CMT_CONFIG, "// comment\rfoo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF after CR-terminated comment");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT");
  ASSERT(tokens.items[0].span.start == 11 && tokens.items[0].span.len == 3,
         "[0] IDENT is after the CR");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diag for CR-terminated line comment");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_line_comment_unicode_newline_terminated(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&CMT_UNICODE_NEWLINE_CONFIG,
                      "// comment" "\xE2\x80\xA8" "foo",
                      &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF after Unicode-terminated comment");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT");
  ASSERT(tokens.items[0].span.start == 13 && tokens.items[0].span.len == 3,
         "[0] IDENT is after U+2028");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diag for Unicode-terminated line comment");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_block_comment_skipped(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&CMT_CONFIG, "/* c */foo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF (block skipped)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT");
  ASSERT(tokens.items[0].span.start == 7 && tokens.items[0].span.len == 3,
         "[0] IDENT \"foo\" right after the close delimiter");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_lone_slash_invalid(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* A single '/' opens no comment rule (both rules want two bytes), so it
   * falls through to INVALID — comment-open matches only the full delim. */
  ASSERT(run_lex_with(&CMT_CONFIG, "/", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] INVALID over the lone slash");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one diag");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_block_comment_unterminated(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Block comment that never closes → INVALID over the whole run + one
   * diag, then EOF. */
  ASSERT(run_lex_with(&CMT_CONFIG, "/* abc", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 6,
         "[0] INVALID spans the unterminated comment");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one diag for the unterminated comment");
  ASSERT(diags.items[0].severity == KAVAK_SEV_ERROR, "diag is error");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_block_comment_non_nested(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Non-nesting: an inner open delimiter is plain body text, and the first
   * close delimiter ends the comment. The 8-byte comment closes cleanly,
   * leaving "x". (Contrast test_block_comment_nested — same bytes.) */
  ASSERT(run_lex_with(&CMT_CONFIG, "/* /* */x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT \"x\"");
  ASSERT(tokens.items[0].span.start == 8 && tokens.items[0].span.len == 1,
         "[0] IDENT right after the first close");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "closed cleanly, no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_block_comment_nested(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Same bytes, NESTING on: the inner open bumps depth to 2, the first
   * close drops it to 1, then "x" runs to EOF still inside the comment →
   * unterminated. */
  ASSERT(run_lex_with(&CMT_NEST_CONFIG, "/* /* */x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID (still open at EOF)");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 9,
         "[0] INVALID spans the whole unterminated run");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one diag for the unterminated nested comment");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_doc_comment_kept(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* "///" (is_doc) beats "//" by longest-open; with keep_doc_comments on it
   * emits a COMMENT spanning the doc comment. The "//" rule would not emit
   * (is_doc=0), so the COMMENT proves the doc rule won. */
  ASSERT(run_lex_with(&CMT_DOC_KEEP, "/// doc\nfoo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "COMMENT + IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_COMMENT, "[0] COMMENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 7,
         "[0] COMMENT spans \"/// doc\", not the newline");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] IDENT \"foo\"");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_doc_comment_skipped(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Same input + rules, keep_doc_comments off → the doc comment is skipped
   * like any other; no COMMENT token. */
  ASSERT(run_lex_with(&CMT_DOC_SKIP, "/// doc\nfoo", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF (COMMENT suppressed)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT \"foo\"");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_keep_comments(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* keep_comments retains non-doc line AND block comments as COMMENT
   * tokens; the line comment still excludes its terminating newline. */
  ASSERT(run_lex_with(&CMT_KEEP_ALL, "// c\nx /* b */", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "COMMENT IDENT COMMENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_COMMENT, "[0] line COMMENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] spans \"// c\", not the newline");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] IDENT x");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 1,
         "[1] x after the newline");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_COMMENT, "[2] block COMMENT");
  ASSERT(tokens.items[2].span.start == 7 && tokens.items[2].span.len == 7,
         "[2] spans open through close delimiter");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "kept comments produce no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* An unterminated block comment is still an error, not trivia. */
  ASSERT(run_lex_with(&CMT_KEEP_ALL, "/* x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID,
         "[0] unterminated block stays INVALID under keep_comments");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] INVALID spans the run");
  ASSERT(diags.count == 1, "one diag for the unterminated comment");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Operator + punctuation tests ────────────────────────────────────── */

static int test_operator_single(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&OP_CONFIG, "+", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "OP + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_OP, "[0] OP");
  ASSERT(tokens.items[0].v == OP_PLUS, "[0] v = op_id");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] OP spans the operator");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_operator_longest_eq(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* "=" alone is OP_ASSIGN; "==" must be ONE OP_EQ, not two OP_ASSIGN. */
  ASSERT(run_lex_with(&OP_CONFIG, "= ==", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "OP OP EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_OP && tokens.items[0].v == OP_ASSIGN,
         "[0] '=' is OP_ASSIGN");
  ASSERT(tokens.items[0].span.len == 1, "[0] one byte");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_OP && tokens.items[1].v == OP_EQ,
         "[1] '==' is OP_EQ (longest-match)");
  ASSERT(tokens.items[1].span.start == 2 && tokens.items[1].span.len == 2,
         "[1] '==' spans two bytes");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_operator_longest_three(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* All three lengths in one stream: "<<=" (3) > "<<" (2) > "<" (1). */
  ASSERT(run_lex_with(&OP_CONFIG, "<<= << <", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "OP OP OP EOF");
  ASSERT(tokens.items[0].v == OP_SHL_EQ, "[0] '<<=' wins at len 3");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3, "[0] 3 bytes");
  ASSERT(tokens.items[1].v == OP_SHL, "[1] '<<' wins at len 2");
  ASSERT(tokens.items[1].span.start == 4 && tokens.items[1].span.len == 2, "[1] 2 bytes");
  ASSERT(tokens.items[2].v == OP_LT, "[2] '<' alone");
  ASSERT(tokens.items[2].span.start == 7 && tokens.items[2].span.len == 1, "[2] 1 byte");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_operator_utf8(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* "≠" (U+2260) is a 3-byte UTF-8 operator. match_lit compares raw
   * bytes, so multi-byte spellings need no special handling. */
  ASSERT(run_lex_with(&OP_CONFIG, "\xE2\x89\xA0", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "OP + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_OP && tokens.items[0].v == OP_NE,
         "[0] '≠' is OP_NE");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] OP spans all 3 UTF-8 bytes");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_operator_then_ident(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* No spaces: the ident scanner stops at '+', the operator scanner takes
   * it, then the next ident resumes. */
  ASSERT(run_lex_with(&OP_CONFIG, "a+b", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "IDENT OP IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT && tokens.items[0].span.len == 1,
         "[0] IDENT 'a'");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_OP && tokens.items[1].v == OP_PLUS,
         "[1] OP '+'");
  ASSERT(tokens.items[1].span.start == 1 && tokens.items[1].span.len == 1, "[1] at byte 1");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT && tokens.items[2].span.start == 2,
         "[2] IDENT 'b'");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_structural_punct_path(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* The 10 structural marks are not configured as operators here, so each
   * emits PUNCT with the source byte in v. */
  ASSERT(run_lex_with(&OP_CONFIG, "()[]{},;:.", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 11, "10 PUNCT + EOF");
  static const unsigned char punct_v[] = {
    '(', ')', '[', ']', '{', '}', ',', ';', ':', '.',
  };
  for (uint32_t i = 0; i < 10; ++i) {
    ASSERT(tokens.items[i].kind == KAVAK_TOK_PUNCT, "PUNCT");
    ASSERT(tokens.items[i].v == punct_v[i], "PUNCT v = source byte");
    ASSERT(tokens.items[i].span.start == i && tokens.items[i].span.len == 1,
           "one byte each, in order");
  }
  ASSERT(tokens.items[10].kind == KAVAK_TOK_EOF, "[10] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_punct_no_operators(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Structural punctuation is recognized even with an empty config (no
   * operators). EMPTY_CONFIG previously saw these as
   * INVALID; now they are PUNCT. */
  ASSERT(run_lex("(;)", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "PUNCT PUNCT PUNCT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_PUNCT, "[0] '('");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_PUNCT, "[1] ';'");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_PUNCT, "[2] ')'");
  ASSERT(tokens.items[0].v == '(' && tokens.items[1].v == ';' &&
         tokens.items[2].v == ')', "punct v stores source byte");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Number-scanner tests ───────────────────────────────────────────── */

static int test_number_decimal_baseline(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex("123 0 42abc", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 5, "INT INT INT IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT 123");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans 123");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] INT 0");
  ASSERT(tokens.items[1].span.start == 4 && tokens.items[1].span.len == 1,
         "[1] spans 0");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INT, "[2] INT 42");
  ASSERT(tokens.items[2].span.start == 6 && tokens.items[2].span.len == 2,
         "[2] number stops before ident chars");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_IDENT, "[3] IDENT abc");
  ASSERT(tokens.items[3].span.start == 8 && tokens.items[3].span.len == 3,
         "[3] IDENT spans abc");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_EOF, "[4] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_base_prefixes(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_BASE_CONFIG, "0x1f 0b1010 0o77 0x1_f", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 5, "4 INT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] hex INT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] spans 0x1f");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] binary INT");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 6,
         "[1] spans 0b1010");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INT, "[2] octal INT");
  ASSERT(tokens.items[2].span.start == 12 && tokens.items[2].span.len == 4,
         "[2] spans 0o77");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_INT, "[3] hex with underscore");
  ASSERT(tokens.items[3].span.start == 17 && tokens.items[3].span.len == 5,
         "[3] spans 0x1_f");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_EOF, "[4] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_prefix_flag_off(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex("0x10", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "INT IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT 0");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] spans only 0 because hex flag is off");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] IDENT x10");
  ASSERT(tokens.items[1].span.start == 1 && tokens.items[1].span.len == 3,
         "[1] spans x10");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "flag-off prefix is not an error");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_underscore_boundaries(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Underscore runs between digits are legal Java/Kotlin separators, so
   * `1__2` is ONE literal; only a trailing run stops the number. */
  ASSERT(run_lex_with(&NUM_BASE_CONFIG, "1_234 1__2", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "INT INT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT 1_234");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] underscore between digits is accepted");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] INT 1__2");
  ASSERT(tokens.items[1].span.start == 6 && tokens.items[1].span.len == 4,
         "[1] double underscore between digits is one literal");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "underscore runs between digits are not errors");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_float_and_exponent(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_FLOAT_CONFIG, "1.2 3e+4 5.6e-7 8e", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 6, "FLOAT FLOAT FLOAT INT IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_FLOAT, "[0] FLOAT 1.2");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans 1.2");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_FLOAT, "[1] FLOAT 3e+4");
  ASSERT(tokens.items[1].span.start == 4 && tokens.items[1].span.len == 4,
         "[1] spans 3e+4");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_FLOAT, "[2] FLOAT 5.6e-7");
  ASSERT(tokens.items[2].span.start == 9 && tokens.items[2].span.len == 6,
         "[2] spans 5.6e-7");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_INT, "[3] INT 8");
  ASSERT(tokens.items[3].span.start == 16 && tokens.items[3].span.len == 1,
         "[3] malformed exponent resyncs before e");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_IDENT, "[4] IDENT e");
  ASSERT(tokens.items[4].span.start == 17 && tokens.items[4].span.len == 1,
         "[4] e is a normal ident after resync");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_EOF, "[5] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_malformed_resync(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_FLOAT_CONFIG, "1.2.3 0x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 6, "FLOAT PUNCT INT INT IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_FLOAT, "[0] FLOAT 1.2");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans 1.2");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_PUNCT, "[1] PUNCT dot");
  ASSERT(tokens.items[1].span.start == 3 && tokens.items[1].span.len == 1,
         "[1] second dot is left for punctuation");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INT, "[2] INT 3");
  ASSERT(tokens.items[2].span.start == 4 && tokens.items[2].span.len == 1,
         "[2] resumes at 3");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_INT, "[3] INT 0");
  ASSERT(tokens.items[3].span.start == 6 && tokens.items[3].span.len == 1,
         "[3] hex flag absent in NUM_FLOAT_CONFIG");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_IDENT, "[4] IDENT x");
  ASSERT(tokens.items[4].span.start == 7 && tokens.items[4].span.len == 1,
         "[4] x is a normal ident when hex is disabled");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_EOF, "[5] EOF");
  ASSERT(diags.count == 0, "no diag when the base prefix flag is off");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_strict_invalid_digits(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Strict: "0x1g" is one malformed token, not INT 0x1 + IDENT g. */
  ASSERT(run_lex_with(&NUM_STRICT_CONFIG, "0x1g", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] malformed token spans the whole '0x1g'");
  ASSERT(diags.count == 1 &&
         strcmp(diags.items[0].message, "malformed number literal") == 0,
         "malformed number literal diagnostic");

  kavak_token_vec_free(&tokens); kavak_token_vec_init(&tokens);
  kavak_diag_vec_free(&diags);   kavak_diag_vec_init(&diags);

  /* Strict: a decimal that runs into letters is malformed too. */
  ASSERT(run_lex_with(&NUM_STRICT_CONFIG, "12abc", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID &&
         tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "'12abc' is one malformed token");

  kavak_token_vec_free(&tokens); kavak_token_vec_init(&tokens);
  kavak_diag_vec_free(&diags);   kavak_diag_vec_init(&diags);

  /* Clean literals are still fine; whitespace is a valid boundary. */
  ASSERT(run_lex_with(&NUM_STRICT_CONFIG, "0x1f 12", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT &&
         tokens.items[0].span.len == 4, "[0] 0x1f INT");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT &&
         tokens.items[1].span.start == 5 && tokens.items[1].span.len == 2,
         "[1] 12 INT");
  ASSERT(diags.count == 0, "no diags for clean literals under strict mode");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_hex_escape_helper(void) {
  uint32_t cp = 0;
#define HX(str, form) kavak_scan_hex_escape((str), (str) + strlen(str), (form), &cp)

  /* \xHH — exactly two hex digits, a raw byte. */
  ASSERT(HX("41", 'x') == 2 && cp == 0x41, "\\x41 => 'A', 2 bytes");
  ASSERT(HX("ff", 'x') == 2 && cp == 0xFF, "\\xff => 0xFF");
  ASSERT(HX("4", 'x') == 0, "\\x with one digit is malformed");
  ASSERT(HX("4g", 'x') == 0, "\\x with a non-hex digit is malformed");

  /* \uHHHH — exactly four hex digits, a Unicode scalar. */
  ASSERT(HX("0041", 'u') == 4 && cp == 0x41, "\\u0041 => 'A'");
  ASSERT(HX("00G0", 'u') == 0, "\\u with bad hex is malformed");
  ASSERT(HX("004", 'u') == 0, "\\u with three digits is malformed");
  ASSERT(HX("d800", 'u') == 0, "\\u surrogate is rejected");

  /* \u{H+} — 1..6 hex digits in braces. */
  ASSERT(HX("{1F600}", 'u') == 7 && cp == 0x1F600, "\\u{1F600} => emoji, 7 bytes");
  ASSERT(HX("{41}", 'u') == 4 && cp == 0x41, "\\u{41} => 'A', 4 bytes");
  ASSERT(HX("{10FFFF}", 'u') == 8 && cp == 0x10FFFF, "\\u{10FFFF} max scalar");
  ASSERT(HX("{}", 'u') == 0, "\\u{} empty is malformed");
  ASSERT(HX("{110000}", 'u') == 0, "\\u{110000} is out of range");
  ASSERT(HX("{D800}", 'u') == 0, "\\u{D800} surrogate is rejected");
  ASSERT(HX("{1F600", 'u') == 0, "\\u{ unclosed brace is malformed");
  ASSERT(HX("{1234567}", 'u') == 0, "\\u{ too many digits is malformed");

  /* \UHHHHHHHH — exactly eight hex digits, a Unicode scalar. */
  ASSERT(HX("0001F600", 'U') == 8 && cp == 0x1F600, "\\U0001F600 => emoji");
  ASSERT(HX("00110000", 'U') == 0, "\\U is out of range");
  ASSERT(HX("0000D800", 'U') == 0, "\\U surrogate is rejected");

  /* Unrecognized form and degenerate inputs. */
  ASSERT(HX("41", 'z') == 0, "unknown form => 0");
  ASSERT(kavak_scan_hex_escape("", "", 'x', &cp) == 0, "empty payload => 0");
  ASSERT(kavak_scan_hex_escape(NULL, NULL, 'x', &cp) == 0, "NULL payload => 0");

#undef HX
  return 0;
}

static int test_number_invalid_prefixed_literal(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_BASE_CONFIG, "0x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 2,
         "[0] invalid spans the whole empty hex literal");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one diag for empty prefixed literal");
  ASSERT(diags.items[0].span.start == 0 && diags.items[0].span.len == 2,
         "diag span matches INVALID");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_suffixes(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_SUFFIX_CONFIG, "12u8 3u", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "INT INT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT with u8 suffix");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] suffix is part of the numeric span");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] INT with u suffix");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 2,
         "[1] suffix is part of the numeric span");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_suffix_boundary(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Flag OFF (default longest-match): "123usize" splits into 123u + size. */
  ASSERT(run_lex_with(&NUM_SUFFIX_BOUNDARY_CONFIG_OFF, "123usize", &tokens, &diags) == 0,
         "rc 0 (boundary off)");
  ASSERT(tokens.count == 3, "INT IDENT EOF (off)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT &&
         tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] 123u — suffix consumed without boundary (off)");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT &&
         tokens.items[1].span.start == 4 && tokens.items[1].span.len == 4,
         "[1] 'size' (off)");

  kavak_token_vec_free(&tokens); kavak_token_vec_init(&tokens);
  kavak_diag_vec_free(&diags);   kavak_diag_vec_init(&diags);

  /* Flag ON: "123usize" stays 123 + usize (suffix needs a boundary). */
  ASSERT(run_lex_with(&NUM_SUFFIX_BOUNDARY_CONFIG, "123usize", &tokens, &diags) == 0,
         "rc 0 (boundary on)");
  ASSERT(tokens.count == 3, "INT IDENT EOF (on)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT &&
         tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] 123 — suffix rejected at identifier boundary (on)");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT &&
         tokens.items[1].span.start == 3 && tokens.items[1].span.len == 5,
         "[1] 'usize' (on)");

  kavak_token_vec_free(&tokens); kavak_token_vec_init(&tokens);
  kavak_diag_vec_free(&diags);   kavak_diag_vec_init(&diags);

  /* Flag ON, real boundary: "123u;" still takes the suffix (';' ends it). */
  ASSERT(run_lex_with(&NUM_SUFFIX_BOUNDARY_CONFIG, "123u", &tokens, &diags) == 0,
         "rc 0 (on, clean)");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT &&
         tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] 123u — suffix consumed at EOF boundary (on)");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_suffix_longest_unsorted(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&NUM_SUFFIX_UNSORTED_CONFIG, "1u64", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 2, "INT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "longest suffix wins even when table is unsorted");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_leading_dot(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* `.5` becomes a FLOAT starting at the dot; a dot NOT followed by a
   * digit is untouched, so `..` stays an operator and `.x` stays
   * punct + ident. One fraction per literal: `.5.6` is two FLOATs. */
  ASSERT(run_lex_with(&NUM_LEADING_DOT_CONFIG, ".5 1..2 .x .5.6 .5e3",
                      &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 10, "FLOAT INT OP INT PUNCT IDENT FLOAT FLOAT FLOAT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_FLOAT, "[0] FLOAT .5");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 2,
         "[0] spans .5 including the dot");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] INT 1");
  ASSERT(tokens.items[1].span.start == 3 && tokens.items[1].span.len == 1,
         "[1] no fraction: next byte after . is another .");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_OP && tokens.items[2].v == OP_RANGE,
         "[2] .. claimed by the operator table");
  ASSERT(tokens.items[2].span.start == 4 && tokens.items[2].span.len == 2,
         "[2] spans both dots");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_INT, "[3] INT 2");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_PUNCT && tokens.items[4].v == '.',
         "[4] .x keeps the dot as punct");
  ASSERT(tokens.items[4].span.start == 8 && tokens.items[4].span.len == 1,
         "[4] one byte");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_IDENT, "[5] IDENT x");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_FLOAT, "[6] FLOAT .5");
  ASSERT(tokens.items[6].span.start == 11 && tokens.items[6].span.len == 2,
         "[6] first fraction stops before the second dot");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_FLOAT, "[7] FLOAT .6");
  ASSERT(tokens.items[7].span.start == 13 && tokens.items[7].span.len == 2,
         "[7] second leading-dot float");
  ASSERT(tokens.items[8].kind == KAVAK_TOK_FLOAT, "[8] FLOAT .5e3");
  ASSERT(tokens.items[8].span.start == 16 && tokens.items[8].span.len == 4,
         "[8] exponent rides on a leading-dot float");
  ASSERT(tokens.items[9].kind == KAVAK_TOK_EOF, "[9] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_number_underscore_runs(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* A `_` run between digits is consumed whole (any length, any base);
   * a trailing run stops the number before the first underscore. */
  ASSERT(run_lex_with(&NUM_BASE_CONFIG, "1__2 1_ 1_000 0x1__f", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 6, "INT INT IDENT INT INT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INT, "[0] INT 1__2");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] double underscore run is one literal");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] INT 1");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 1,
         "[1] trailing underscore is not consumed");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT, "[2] IDENT _");
  ASSERT(tokens.items[2].span.start == 6 && tokens.items[2].span.len == 1,
         "[2] the trailing underscore resyncs as an ident");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_INT, "[3] INT 1_000");
  ASSERT(tokens.items[3].span.start == 8 && tokens.items[3].span.len == 5,
         "[3] single separators unchanged");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_INT, "[4] INT 0x1__f");
  ASSERT(tokens.items[4].span.start == 14 && tokens.items[4].span.len == 6,
         "[4] runs work for non-decimal bases too");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_EOF, "[5] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── String/char-scanner tests ──────────────────────────────────────── */

static int test_string_basic_escape(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"a\\n\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] STRING");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] string span includes delimiters and escape bytes");
  ASSERT(tokens.items[0].v == 0, "[0] STRING v = 0");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "valid escape, no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_single_quote_char(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "'x'", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "CHAR + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_CHAR, "[0] CHAR");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] char span includes delimiters");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_triple_longest_open(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"\"\"a\"\"\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] STRING");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 7,
         "[0] triple string spans all delimiters");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "triple close found");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_raw_beats_ident_and_skips_escapes(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "r\"\\q\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] raw STRING");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] raw opener beats IDENT r");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "raw string does not validate \\q");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_invalid_escape_keeps_token(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"a\\q\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] STRING still emitted");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] whole string span survives invalid escape");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one invalid-escape diag");
  ASSERT(diags.items[0].span.start == 2 && diags.items[0].span.len == 2,
         "diag spans \\q");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_invalid_utf8_escape_skips_sequence(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"\\\xE2\x28\xA1\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] STRING still emitted");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 6,
         "[0] string span includes recovered escape");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 2, "UTF-8 escape reports bounded diagnostics");
  ASSERT(diags.items[0].span.start == 2 && diags.items[0].span.len == 3,
         "UTF-8 diag spans recovered sequence");
  ASSERT(diags.items[1].span.start == 1 && diags.items[1].span.len == 4,
         "escape diag spans full escaped sequence");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_unterminated_eof(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"abc", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] spans the unterminated string");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 1, "one diag");
  ASSERT(diags.items[0].span.start == 0 && diags.items[0].span.len == 4,
         "diag span matches INVALID");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_unterminated_newline_recovers(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&STR_CONFIG, "\"abc\nx", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "INVALID IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] stops before newline");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] IDENT x after recovery");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 1,
         "[1] x after skipped newline");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 1, "one diag");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_string_quoted_identifier(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* The IDENT flag emits a quoted identifier spanning both backticks.
   * "if" is in the keyword table, but quoted names never retag. */
  ASSERT(run_lex_with(&QUOTED_IDENT_CONFIG, "`if`", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT, not KEYWORD");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] spans both delimiters");
  ASSERT(tokens.items[0].v == 0, "[0] IDENT v = 0");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* Empty backticks stay a legal 2-byte IDENT — validation is the
   * consumer's job. */
  ASSERT(run_lex_with(&QUOTED_IDENT_CONFIG, "``", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] empty quoted IDENT");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 2,
         "[0] just the two backticks");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* Unterminated quoted identifier reuses string recovery. */
  ASSERT(run_lex_with(&QUOTED_IDENT_CONFIG, "`a b", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "INVALID + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_INVALID, "[0] INVALID");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 4,
         "[0] spans the unterminated identifier");
  ASSERT(diags.count == 1, "one diag");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── String interpolation tests ─────────────────────────────────────── */

static int test_template_bare_and_braced_interpolation(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"a $name ${b + 1} c\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 8, "STRING IDENT STRING IDENT OP INT STRING EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] leading string fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans opening quote through text before $name");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] bare interpolation ident");
  ASSERT(tokens.items[1].span.start == 4 && tokens.items[1].span.len == 4,
         "[1] spans name without the $ marker");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_STRING, "[2] middle string fragment");
  ASSERT(tokens.items[2].span.start == 8 && tokens.items[2].span.len == 1,
         "[2] spans the space between interpolations");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_IDENT, "[3] braced ident");
  ASSERT(tokens.items[3].span.start == 11 && tokens.items[3].span.len == 1,
         "[3] spans b");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_OP && tokens.items[4].v == OP_PLUS,
         "[4] plus inside interpolation");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_INT, "[5] int inside interpolation");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_STRING, "[6] trailing string fragment");
  ASSERT(tokens.items[6].span.start == 17 && tokens.items[6].span.len == 3,
         "[6] spans text after } through closing quote");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_EOF, "[7] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&AUTO_SEMI_CONFIG, ")\n+", &tokens, &diags) == 0,
         "rc 0 for punctuation terminator");
  ASSERT(tokens.count == 4, "PUNCT ; OP EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_PUNCT && tokens.items[0].v == ')',
         "[0] closing punct");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_PUNCT &&
         tokens.items[1].v == PUNCT_VIRTUAL_SEMI, "[1] virtual semi after )");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_OP && tokens.items[2].v == OP_PLUS,
         "[2] plus after newline");
  ASSERT(diags.count == 0, "no punct terminator diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_balances_inner_braces(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"${bar({x: 1})}\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 11, "fragment + balanced expression + fragment + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] opening fragment");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] bar");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_PUNCT, "[2] (");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_PUNCT, "[3] inner {");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_IDENT, "[4] x");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_PUNCT, "[5] :");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_INT, "[6] 1");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_PUNCT, "[7] inner }");
  ASSERT(tokens.items[8].kind == KAVAK_TOK_PUNCT, "[8] )");
  ASSERT(tokens.items[9].kind == KAVAK_TOK_STRING, "[9] closing fragment");
  ASSERT(tokens.items[10].kind == KAVAK_TOK_EOF, "[10] EOF");
  ASSERT(diags.count == 0, "inner } does not close the interpolation");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_unbalanced_interpolation(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"a ${x", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "STRING IDENT INVALID EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] fragment before ${");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] x before EOF");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INVALID, "[2] invalid interpolation");
  ASSERT(tokens.items[2].span.start == 3 && tokens.items[2].span.len == 3,
         "[2] INVALID spans ${x");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 1, "one diag for unterminated interpolation");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_lone_dollar_literal(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Dual-form rules (`$` / NULL close): a marker followed by neither
   * `{` nor an ident start is literal text (Kotlin semantics). */
  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"price: $\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] one plain STRING");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 10,
         "[0] trailing $ stays inside the literal");
  ASSERT(diags.count == 0, "lone $ before the close quote is not an error");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"$ x\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] one plain STRING");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] $ before whitespace stays inside the literal");
  ASSERT(diags.count == 0, "lone $ mid-string is not an error");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* `$` before an ident start still interpolates. */
  ASSERT(run_lex_with(&TEMPLATE_CONFIG, "\"$x\"", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 4, "STRING IDENT STRING EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] head fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] just the open quote");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] interpolated x");
  ASSERT(tokens.items[1].span.start == 2 && tokens.items[1].span.len == 1,
         "[1] spans x without the $");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_STRING, "[2] tail fragment");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_explicit_close_always_enters(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Explicit-close rules keep the always-enter behavior: whitespace
   * right after `\(` still opens the (empty) expression. */
  ASSERT(run_lex_with(&SWIFT_TEMPLATE_CONFIG, "\"\\( )\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 3, "STRING STRING EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] head fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] open quote before the marker");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_STRING, "[1] tail fragment");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 1,
         "[1] close quote after the interpolation");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF");
  ASSERT(diags.count == 0, "empty explicit-close interpolation is fine");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&SWIFT_TEMPLATE_CONFIG, "\"a \\(1)b\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 4, "STRING INT STRING EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] head fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans quote + text before the marker");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_INT, "[1] interpolated 1");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 1,
         "[1] expression token at its byte offset");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_STRING, "[2] tail fragment");
  ASSERT(tokens.items[2].span.start == 7 && tokens.items[2].span.len == 2,
         "[2] text after ) through close quote");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Interpolation marker-token tests ───────────────────────────────── */

static int test_template_marker_tokens(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Bare `$x`: fragments retag to fragment_kind with v = head(1)/tail(3)
   * and the `$ident` entry is ONE token including the `$`. No open
   * marker for the bare form. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"a $x b\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 4, "FRAGMENT BARE_IDENT FRAGMENT EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT, "[0] head fragment kind");
  ASSERT(tokens.items[0].v == 1, "[0] v = 1 (head)");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 3,
         "[0] spans quote through text before $x");
  ASSERT(tokens.items[1].kind == MARK_BARE_IDENT, "[1] bare-ident kind");
  ASSERT(tokens.items[1].v == 0, "[1] v = 0");
  ASSERT(tokens.items[1].span.start == 3 && tokens.items[1].span.len == 2,
         "[1] spans $x including the $");
  ASSERT(tokens.items[2].kind == MARK_FRAGMENT, "[2] tail fragment kind");
  ASSERT(tokens.items[2].v == 3, "[2] v = 3 (tail)");
  ASSERT(tokens.items[2].span.start == 5 && tokens.items[2].span.len == 3,
         "[2] spans text after x through close quote");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_EOF, "[3] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* No interpolation → ONE normal STRING with v = 0, even though
   * fragment_kind is configured. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"plain\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 2, "STRING + EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] plain STRING kind");
  ASSERT(tokens.items[0].v == 0, "[0] v = 0");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 7,
         "[0] whole literal");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_marker_interior_fragment(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* A fragment BETWEEN two interpolations is interior: v = 2, not another
   * head. Regression: a survived mutant once tagged every pre-interp
   * fragment v = 1, which only multi-interpolation literals can catch. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"a $x m $y b\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 6, "FRAG BARE FRAG BARE FRAG EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT && tokens.items[0].v == 1,
         "[0] head fragment v = 1");
  ASSERT(tokens.items[1].kind == MARK_BARE_IDENT, "[1] $x");
  ASSERT(tokens.items[2].kind == MARK_FRAGMENT, "[2] interior fragment kind");
  ASSERT(tokens.items[2].v == 2, "[2] v = 2 (interior, NOT head)");
  ASSERT(tokens.items[2].span.start == 5 && tokens.items[2].span.len == 3,
         "[2] spans ` m ` between the interpolations");
  ASSERT(tokens.items[3].kind == MARK_BARE_IDENT, "[3] $y");
  ASSERT(tokens.items[4].kind == MARK_FRAGMENT && tokens.items[4].v == 3,
         "[4] tail fragment v = 3");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_EOF, "[5] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* Same for the braced form: `-` between `${a}` and `${b}` is interior. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"${a}-${b}\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 10, "FRAG OPEN a CLOSE FRAG OPEN b CLOSE FRAG EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT && tokens.items[0].v == 1,
         "[0] head is just the open quote, v = 1");
  ASSERT(tokens.items[4].kind == MARK_FRAGMENT, "[4] interior fragment kind");
  ASSERT(tokens.items[4].v == 2, "[4] v = 2 (interior)");
  ASSERT(tokens.items[4].span.start == 5 && tokens.items[4].span.len == 1,
         "[4] spans the `-`");
  ASSERT(tokens.items[8].kind == MARK_FRAGMENT && tokens.items[8].v == 3,
         "[8] tail fragment v = 3");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_marker_braced_and_adjacent(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* `${...}`: OPEN marker over `${`, CLOSE marker over `}`, expression
   * tokens unchanged in between. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"pre ${1 + 2} post\"",
                      &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 8, "FRAG OPEN INT OP INT CLOSE FRAG EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT && tokens.items[0].v == 1,
         "[0] head fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 5,
         "[0] spans \"pre ");
  ASSERT(tokens.items[1].kind == MARK_INTERP_OPEN, "[1] open marker");
  ASSERT(tokens.items[1].v == 0, "[1] marker v = 0");
  ASSERT(tokens.items[1].span.start == 5 && tokens.items[1].span.len == 2,
         "[1] spans exactly ${");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_INT, "[2] INT 1");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_OP && tokens.items[3].v == OP_PLUS,
         "[3] + inside the hole");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_INT, "[4] INT 2");
  ASSERT(tokens.items[5].kind == MARK_INTERP_CLOSE, "[5] close marker");
  ASSERT(tokens.items[5].v == 0, "[5] marker v = 0");
  ASSERT(tokens.items[5].span.start == 12 && tokens.items[5].span.len == 1,
         "[5] spans exactly }");
  ASSERT(tokens.items[6].kind == MARK_FRAGMENT && tokens.items[6].v == 3,
         "[6] tail fragment");
  ASSERT(tokens.items[6].span.start == 13 && tokens.items[6].span.len == 6,
         "[6] spans  post\"");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_EOF, "[7] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* Adjacent interpolations: no empty interior fragment between them,
   * but head and tail always exist (open/close delimiters). */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"$a$b\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 5, "FRAG BARE BARE FRAG EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT && tokens.items[0].v == 1,
         "[0] head fragment");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 1,
         "[0] just the open quote");
  ASSERT(tokens.items[1].kind == MARK_BARE_IDENT, "[1] $a entry");
  ASSERT(tokens.items[1].span.start == 1 && tokens.items[1].span.len == 2,
         "[1] spans $a");
  ASSERT(tokens.items[2].kind == MARK_BARE_IDENT, "[2] $b entry, no fragment between");
  ASSERT(tokens.items[2].span.start == 3 && tokens.items[2].span.len == 2,
         "[2] spans $b");
  ASSERT(tokens.items[3].kind == MARK_FRAGMENT && tokens.items[3].v == 3,
         "[3] tail fragment");
  ASSERT(tokens.items[3].span.start == 5 && tokens.items[3].span.len == 1,
         "[3] just the close quote");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_EOF, "[4] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_template_marker_nested(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* Nested template inside `${...}`: inner fragments carry their own
   * head/tail v-tags, so the parser can rebuild nesting from kinds. */
  ASSERT(run_lex_with(&MARKER_TEMPLATE_CONFIG, "\"x${\"in $y\"}z\"",
                      &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 8, "FRAG OPEN FRAG BARE FRAG CLOSE FRAG EOF");
  ASSERT(tokens.items[0].kind == MARK_FRAGMENT && tokens.items[0].v == 1,
         "[0] outer head");
  ASSERT(tokens.items[0].span.start == 0 && tokens.items[0].span.len == 2,
         "[0] spans \"x");
  ASSERT(tokens.items[1].kind == MARK_INTERP_OPEN, "[1] outer ${ marker");
  ASSERT(tokens.items[1].span.start == 2 && tokens.items[1].span.len == 2,
         "[1] spans ${");
  ASSERT(tokens.items[2].kind == MARK_FRAGMENT && tokens.items[2].v == 1,
         "[2] inner head");
  ASSERT(tokens.items[2].span.start == 4 && tokens.items[2].span.len == 4,
         "[2] spans \"in ");
  ASSERT(tokens.items[3].kind == MARK_BARE_IDENT, "[3] inner $y entry");
  ASSERT(tokens.items[3].span.start == 8 && tokens.items[3].span.len == 2,
         "[3] spans $y");
  ASSERT(tokens.items[4].kind == MARK_FRAGMENT && tokens.items[4].v == 3,
         "[4] inner tail");
  ASSERT(tokens.items[4].span.start == 10 && tokens.items[4].span.len == 1,
         "[4] just the inner close quote");
  ASSERT(tokens.items[5].kind == MARK_INTERP_CLOSE, "[5] outer } marker");
  ASSERT(tokens.items[5].span.start == 11 && tokens.items[5].span.len == 1,
         "[5] spans }");
  ASSERT(tokens.items[6].kind == MARK_FRAGMENT && tokens.items[6].v == 3,
         "[6] outer tail");
  ASSERT(tokens.items[6].span.start == 12 && tokens.items[6].span.len == 2,
         "[6] spans z\"");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_EOF, "[7] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── emit_newlines tests ────────────────────────────────────────────── */

static int test_emit_newlines_every_line(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* One NEWLINE per physical newline, blank lines included, nothing
   * extra at EOF. */
  ASSERT(run_lex_with(&EMIT_NEWLINES_CONFIG, "a\nb\n\nc", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 7, "IDENT NL IDENT NL NL IDENT EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] a");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_NEWLINE, "[1] NEWLINE after a");
  ASSERT(tokens.items[1].span.start == 1 && tokens.items[1].span.len == 1,
         "[1] spans the newline byte");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_IDENT, "[2] b");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_NEWLINE, "[3] NEWLINE after b");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_NEWLINE, "[4] NEWLINE for the blank line");
  ASSERT(tokens.items[4].span.start == 4 && tokens.items[4].span.len == 1,
         "[4] blank line newline byte");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_IDENT, "[5] c");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_EOF, "[6] EOF, no virtual NEWLINE");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* A trailing newline emits its NEWLINE; EOF adds nothing on top. */
  ASSERT(run_lex_with(&EMIT_NEWLINES_CONFIG, "a\n", &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == 3, "IDENT NEWLINE EOF");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_NEWLINE, "[1] trailing NEWLINE");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_EOF, "[2] EOF right after");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

static int test_emit_newlines_interpolation_and_comments(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  /* A newline inside a `${...}` hole still emits its NEWLINE token. */
  ASSERT(run_lex_with(&EMIT_NEWLINES_CONFIG, "\"x${a\n+b}y\"", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 7, "STRING IDENT NEWLINE OP IDENT STRING EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_STRING, "[0] head fragment");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_IDENT, "[1] a");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_NEWLINE, "[2] NEWLINE inside the hole");
  ASSERT(tokens.items[2].span.start == 5 && tokens.items[2].span.len == 1,
         "[2] spans the newline byte");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_OP && tokens.items[3].v == OP_PLUS,
         "[3] + after the newline");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_IDENT, "[4] b");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_STRING, "[5] tail fragment");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_EOF, "[6] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  kavak_token_vec_init(&tokens);
  kavak_diag_vec_init(&diags);

  /* Newlines swallowed by a block comment never emit. */
  ASSERT(run_lex_with(&EMIT_NEWLINES_CONFIG, "/* a\nb */x", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 2, "IDENT + EOF, no NEWLINE from the comment");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] IDENT x");
  ASSERT(tokens.items[0].span.start == 9 && tokens.items[0].span.len == 1,
         "[0] x right after the comment");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_EOF, "[1] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── Auto-semicolon tests ───────────────────────────────────────────── */

static int test_auto_semicolon_newline_and_eof(void) {
  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);

  ASSERT(run_lex_with(&AUTO_SEMI_CONFIG, "a\n+\nb\n42", &tokens, &diags) == 0,
         "rc 0");
  ASSERT(tokens.count == 8, "IDENT ; OP IDENT ; INT ; EOF");
  ASSERT(tokens.items[0].kind == KAVAK_TOK_IDENT, "[0] a");
  ASSERT(tokens.items[1].kind == KAVAK_TOK_PUNCT &&
         tokens.items[1].v == PUNCT_VIRTUAL_SEMI, "[1] virtual semi after a");
  ASSERT(tokens.items[1].span.start == 1 && tokens.items[1].span.len == 0,
         "[1] semi is before first newline");
  ASSERT(tokens.items[2].kind == KAVAK_TOK_OP && tokens.items[2].v == OP_PLUS,
         "[2] plus line does not terminate");
  ASSERT(tokens.items[3].kind == KAVAK_TOK_IDENT, "[3] b");
  ASSERT(tokens.items[4].kind == KAVAK_TOK_PUNCT &&
         tokens.items[4].v == PUNCT_VIRTUAL_SEMI, "[4] virtual semi after b");
  ASSERT(tokens.items[5].kind == KAVAK_TOK_INT, "[5] 42");
  ASSERT(tokens.items[6].kind == KAVAK_TOK_PUNCT &&
         tokens.items[6].v == PUNCT_VIRTUAL_SEMI, "[6] final EOF semi");
  ASSERT(tokens.items[6].span.start == 8 && tokens.items[6].span.len == 0,
         "[6] EOF semi anchors at len");
  ASSERT(tokens.items[7].kind == KAVAK_TOK_EOF, "[7] EOF");
  ASSERT(diags.count == 0, "no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

/* ── TinyLang smoke test ────────────────────────────────────────────── */

static int test_tinylang_smoke(void) {
  static const uint32_t expected_kinds[] = {
    KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_PUNCT, KAVAK_TOK_PUNCT,
    KAVAK_TOK_PUNCT, KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT, KAVAK_TOK_OP,
    KAVAK_TOK_INT, KAVAK_TOK_OP, KAVAK_TOK_FLOAT, KAVAK_TOK_KEYWORD,
    KAVAK_TOK_IDENT, KAVAK_TOK_OP, KAVAK_TOK_INT, KAVAK_TOK_OP,
    KAVAK_TOK_IDENT, KAVAK_TOK_PUNCT, KAVAK_TOK_KEYWORD, KAVAK_TOK_IDENT,
    KAVAK_TOK_OP, KAVAK_TOK_STRING, KAVAK_TOK_PUNCT, KAVAK_TOK_PUNCT,
    KAVAK_TOK_EOF,
  };

  KavakTokenVec tokens; kavak_token_vec_init(&tokens);
  KavakDiagVec  diags;  kavak_diag_vec_init(&diags);
  const char *src =
    "fn main() { let x = 1 + 2.5 // skip\n"
    " if x == 3 && x { let s = \"a\\n\" } }";

  ASSERT(run_lex_with(&TINY_CONFIG, src, &tokens, &diags) == 0, "rc 0");
  ASSERT(tokens.count == sizeof(expected_kinds) / sizeof(*expected_kinds),
         "full TinyLang token count");
  for (uint32_t i = 0; i < tokens.count; ++i) {
    ASSERT(tokens.items[i].kind == expected_kinds[i], "TinyLang kind sequence");
  }
  ASSERT(tokens.items[0].v == TINY_KW_FN, "fn keyword id");
  ASSERT(tokens.items[7].v == TINY_OP_ASSIGN, "= op id");
  ASSERT(tokens.items[9].v == TINY_OP_PLUS, "+ op id");
  ASSERT(tokens.items[13].v == TINY_OP_EQ, "== op id, not two = tokens");
  ASSERT(tokens.items[15].v == TINY_OP_AND, "&& op id");
  ASSERT(tokens.items[tokens.count - 1].span.start == strlen(src),
         "EOF anchored at len");
  ASSERT(diags.count == 0, "comment skipped and no diags");

  kavak_diag_vec_free(&diags);
  kavak_token_vec_free(&tokens);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_empty_source();
  fails += test_invalid_config_rejected();
  fails += test_whitespace_only();
  fails += test_one_unknown_byte();
  fails += test_recovery_no_desync();
  fails += test_null_diags_no_crash();
  fails += test_identifier_basic();
  fails += test_keyword_match();
  fails += test_soft_keyword();
  fails += test_keyword_prefix();
  fails += test_custom_ident_start();
  fails += test_unicode_ident_hooks();
  fails += test_ident_then_unknown();
  fails += test_line_comment_skipped();
  fails += test_line_comment_eof();
  fails += test_line_comment_cr_terminated();
  fails += test_line_comment_unicode_newline_terminated();
  fails += test_block_comment_skipped();
  fails += test_lone_slash_invalid();
  fails += test_block_comment_unterminated();
  fails += test_block_comment_non_nested();
  fails += test_block_comment_nested();
  fails += test_doc_comment_kept();
  fails += test_doc_comment_skipped();
  fails += test_keep_comments();
  fails += test_operator_single();
  fails += test_operator_longest_eq();
  fails += test_operator_longest_three();
  fails += test_operator_utf8();
  fails += test_operator_then_ident();
  fails += test_structural_punct_path();
  fails += test_punct_no_operators();
  fails += test_number_decimal_baseline();
  fails += test_number_base_prefixes();
  fails += test_number_prefix_flag_off();
  fails += test_number_underscore_boundaries();
  fails += test_number_float_and_exponent();
  fails += test_number_malformed_resync();
  fails += test_number_invalid_prefixed_literal();
  fails += test_number_strict_invalid_digits();
  fails += test_hex_escape_helper();
  fails += test_number_suffixes();
  fails += test_number_suffix_boundary();
  fails += test_number_suffix_longest_unsorted();
  fails += test_number_leading_dot();
  fails += test_number_underscore_runs();
  fails += test_string_basic_escape();
  fails += test_string_single_quote_char();
  fails += test_string_triple_longest_open();
  fails += test_string_raw_beats_ident_and_skips_escapes();
  fails += test_string_invalid_escape_keeps_token();
  fails += test_string_invalid_utf8_escape_skips_sequence();
  fails += test_string_unterminated_eof();
  fails += test_string_unterminated_newline_recovers();
  fails += test_string_quoted_identifier();
  fails += test_template_bare_and_braced_interpolation();
  fails += test_template_balances_inner_braces();
  fails += test_template_unbalanced_interpolation();
  fails += test_template_lone_dollar_literal();
  fails += test_template_explicit_close_always_enters();
  fails += test_template_marker_tokens();
  fails += test_template_marker_interior_fragment();
  fails += test_template_marker_braced_and_adjacent();
  fails += test_template_marker_nested();
  fails += test_emit_newlines_every_line();
  fails += test_emit_newlines_interpolation_and_comments();
  fails += test_auto_semicolon_newline_and_eof();
  fails += test_tinylang_smoke();

  if (fails == 0) {
    printf("  ✓ test_lexer: 68/68 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_lexer: %d failure(s)\n", fails);
  return 1;
}
