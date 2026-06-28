// SPDX-License-Identifier: MIT
/**
 * @file src/lexer.c
 * @brief kavak_lex driver and token scanners.
 *
 * The lexer walks a byte cursor and applies scanners in fixed precedence:
 * comments, strings/chars, numbers, identifiers, operators, then punctuation.
 * Unmatched bytes become KAVAK_TOK_INVALID with a diagnostic.
 */

#include "kavak.h"

#include <stdint.h>
#include <string.h>

#define KAVAK_MAX_INDENT_DEPTH 128u
#define KAVAK_MAX_INTERP_DEPTH 64u

typedef struct Lexer {
  const KavakSource      *source;
  const KavakLexerConfig *config;       /* shared scanner configuration  */
  KavakTokenVec          *tokens;
  KavakDiagVec           *diags;        /* may be NULL — diags discarded */
  uint32_t                pos;
  uint32_t                indent_stack[KAVAK_MAX_INDENT_DEPTH];
  uint32_t                indent_top;
  uint32_t                last_significant_index;
  uint32_t                interp_depth;
  int                     at_line_start;
  int                     line_has_significant;
} Lexer;

static int lexer_config_valid(const KavakLexerConfig *config) {
  if (!config) return 0;
  if (config->keyword_count != 0 && !config->keywords) return 0;
  for (uint32_t i = 0; i < config->keyword_count; ++i) {
    const char *text = config->keywords[i].text;
    if (!text || text[0] == '\0') return 0;
  }

  /* Soft keywords reference keyword ids and are dereferenced during identifier
   * retagging; a non-zero count with a NULL table is a NULL-deref waiting to
   * happen. */
  if (config->soft_keyword_count != 0 && !config->soft_keywords) return 0;

  if (config->operator_count != 0 && !config->operators) return 0;
  for (uint32_t i = 0; i < config->operator_count; ++i) {
    const char *text = config->operators[i].text;
    if (!text || text[0] == '\0') return 0;
  }

  if (config->comment_count != 0 && !config->comments) return 0;
  for (uint32_t i = 0; i < config->comment_count; ++i) {
    const KavakCommentRule *rule = &config->comments[i];
    if (!rule->open || rule->open[0] == '\0') return 0;
    if (!rule->close || rule->close[0] == '\0') return 0;
  }

  if (config->string_count != 0 && !config->strings) return 0;
  for (uint32_t i = 0; i < config->string_count; ++i) {
    const KavakStringRule *rule = &config->strings[i];
    if (!rule->open || rule->open[0] == '\0') return 0;
    if (!rule->close || rule->close[0] == '\0') return 0;
  }

  if (config->numbers.suffix_count != 0 && !config->numbers.suffixes) return 0;
  return 1;
}

static int at_eof(const Lexer *lex) {
  return lex->pos >= (uint32_t)lex->source->len;
}

static unsigned char peek(const Lexer *lex) {
  return at_eof(lex) ? 0 : (unsigned char)lex->source->bytes[lex->pos];
}

static uint32_t newline_flags(const Lexer *lex) {
  if (lex->config && lex->config->newline_flags) return lex->config->newline_flags;
  return lex->source->newline_flags ? lex->source->newline_flags
                                    : KAVAK_NEWLINE_DEFAULT;
}

static uint32_t newline_len_at(const Lexer *lex, const uint32_t pos) {
  return kavak_source_newline_len(lex->source, pos, newline_flags(lex));
}

/* ASCII whitespace only. Newline-aware layout and auto-semicolon handling
 * wrap this primitive rather than replacing it. */
static int is_inline_ws(const unsigned char c) {
  return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

static int starts_newline(const Lexer *lex) {
  return !at_eof(lex) && newline_len_at(lex, lex->pos) != 0;
}

static uint32_t consume_newline(Lexer *lex) {
  const uint32_t n = newline_len_at(lex, lex->pos);
  if (n == 0) return 0;
  lex->pos += n;
  return n;
}

static void skip_whitespace(Lexer *lex) {
  while (!at_eof(lex)) {
    const uint32_t n = newline_len_at(lex, lex->pos);
    if (n != 0) {
      lex->pos += n;
      continue;
    }
    if (!is_inline_ws(peek(lex))) break;
    lex->pos++;
  }
}

static void skip_inline_whitespace(Lexer *lex) {
  while (!at_eof(lex) && is_inline_ws(peek(lex))) lex->pos++;
}

static int emit_token(Lexer *lex, const uint32_t kind, const KavakSpan span,
                      const uint32_t v) {
  return kavak_token_vec_push(lex->tokens,
                              (KavakToken){ .kind = kind, .span = span, .v = v });
}

static void push_diag(Lexer *lex, const KavakSpan span, const char *message) {
  if (!lex->diags) return;
  (void)kavak_diag_vec_push(lex->diags, (KavakDiag){
    .severity = KAVAK_SEV_ERROR,
    .message  = message,
    .span     = span,
  });
}

/* Records an INVALID token over [start, lex->pos) and pushes a diag.
 * Returns -1 only if the token push fails (critical OOM); a failed
 * diag push is non-fatal — the token is still recorded and the lexer
 * keeps going. */
static int emit_invalid(Lexer *lex, const uint32_t start, const char *message) {
  const KavakSpan span = kavak_span_from_to(start, lex->pos);
  if (emit_token(lex, KAVAK_TOK_INVALID, span, 0) != 0) return -1;
  push_diag(lex, span, message);
  return 0;
}

/* ── Shared scanner substrate ────────────────────────────────────────── */

/* Decode the codepoint at lex->pos without advancing. Returns bytes
 * (1..4) on success, 0 at EOF or on invalid UTF-8 (the kavak_utf8_decode
 * contract). Scanners that walk past ASCII go through this so bad-UTF-8
 * handling is uniform at their own recovery boundary. */
static int peek_cp(const Lexer *lex, uint32_t *out_cp) {
  return kavak_utf8_decode(lex->source->bytes + lex->pos,
                           lex->source->bytes + lex->source->len, out_cp);
}

static uint32_t utf8_recovery_len(const Lexer *lex) {
  const unsigned char lead = peek(lex);
  uint32_t expected = 1;
  if (lead >= 0xC2 && lead <= 0xDF) expected = 2;
  else if (lead >= 0xE0 && lead <= 0xEF) expected = 3;
  else if (lead >= 0xF0 && lead <= 0xF4) expected = 4;
  const uint32_t remaining = (uint32_t)lex->source->len - lex->pos;
  return expected < remaining ? expected : remaining;
}

/* Does the source at lex->pos begin with the NUL-terminated literal
 * `lit`? Pure look-ahead: never advances pos, never reads past the
 * source end. */
static int match_lit(const Lexer *lex, const char *lit) {
  const size_t n = strlen(lit);
  if ((size_t)lex->pos + n > lex->source->len) return 0;
  return memcmp(lex->source->bytes + lex->pos, lit, n) == 0;
}

static int match_lit_at(const Lexer *lex, const uint32_t pos, const char *lit) {
  const size_t n = strlen(lit);
  if ((size_t)pos + n > lex->source->len) return 0;
  return memcmp(lex->source->bytes + pos, lit, n) == 0;
}

/* Identifier predicates: dispatch to the config hook, or the kernel's
 * ASCII default when the hook is NULL. */
static int is_ident_start_cfg(const Lexer *lex, const uint32_t cp) {
  return lex->config->is_ident_start ? lex->config->is_ident_start(cp)
                                     : kavak_ascii_is_ident_start(cp);
}

static int is_ident_cont_cfg(const Lexer *lex, const uint32_t cp) {
  return lex->config->is_ident_cont ? lex->config->is_ident_cont(cp)
                                    : kavak_ascii_is_ident_cont(cp);
}

static int ascii_digit_value(const unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
  return -1;
}

static int is_unicode_scalar(const uint32_t cp) {
  return cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

size_t kavak_scan_hex_escape(const char *s, const char *end, const char form,
                             uint32_t *out_cp) {
  if (!s || !end || s >= end) return 0;

  uint32_t fixed;
  switch (form) {
    case 'x': fixed = 2; break;
    case 'u': fixed = 4; break;  /* fixed \uHHHH, or braced \u{H+} below */
    case 'U': fixed = 8; break;
    default: return 0;
  }

  /* Braced \u{H+}: 1..6 hex digits, any Unicode scalar value. */
  if (form == 'u' && *s == '{') {
    const char *p = s + 1;
    uint32_t cp = 0, digits = 0;
    for (; p < end && *p != '}'; ++p) {
      const int d = ascii_digit_value((unsigned char)*p);
      if (d < 0 || digits == 6u) return 0;
      cp = (cp << 4) | (uint32_t)d;
      ++digits;
    }
    if (digits == 0 || p >= end || *p != '}' || !is_unicode_scalar(cp)) return 0;
    if (out_cp) *out_cp = cp;
    return (size_t)(p - s) + 1u;  /* through the closing brace */
  }

  /* Fixed-width form: exactly `fixed` hex digits. */
  if ((size_t)(end - s) < (size_t)fixed) return 0;
  uint32_t cp = 0;
  for (uint32_t i = 0; i < fixed; ++i) {
    const int d = ascii_digit_value((unsigned char)s[i]);
    if (d < 0) return 0;
    cp = (cp << 4) | (uint32_t)d;
  }
  /* \u / \U must denote a Unicode scalar; \x is a raw byte 0x00..0xFF. */
  if (form != 'x' && !is_unicode_scalar(cp)) return 0;
  if (out_cp) *out_cp = cp;
  return fixed;
}

static int is_digit_for_base(const unsigned char c, const uint32_t base) {
  const int value = ascii_digit_value(c);
  return value >= 0 && (uint32_t)value < base;
}

static int byte_at_is_digit_for_base(const Lexer *lex, const uint32_t pos,
                                     const uint32_t base) {
  return pos < (uint32_t)lex->source->len &&
         is_digit_for_base((unsigned char)lex->source->bytes[pos], base);
}

/* Scanner outcome. NO_MATCH = "not my token, try the next scanner";
 * OOM = critical token push failure, propagate -1 out of kavak_lex.
 * MATCHED = the scanner claimed the input and advanced pos. "Claimed and
 * advanced" is the real contract, not "emitted a token": it usually emits
 * one, but a skipped comment advances without emitting, and a
 * recoverable error emits INVALID + diag. Returning MATCHED — never
 * NO_MATCH — on a recoverable error keeps the bad span from double-
 * reporting through the unknown-char fallthrough. */
typedef enum {
  SCAN_NO_MATCH = 0,
  SCAN_MATCHED  = 1,
  SCAN_OOM      = -1,
} ScanResult;

static ScanResult scan_token(Lexer *lex);

static int token_kind_is_significant(const uint32_t kind) {
  switch (kind) {
    case KAVAK_TOK_INVALID:
    case KAVAK_TOK_EOF:
    case KAVAK_TOK_NEWLINE:
    case KAVAK_TOK_INDENT:
    case KAVAK_TOK_DEDENT:
    case KAVAK_TOK_COMMENT:
      return 0;
    default:
      return 1;
  }
}

static void note_emitted_tokens(Lexer *lex, const uint32_t first_index) {
  for (uint32_t i = first_index; i < lex->tokens->count; ++i) {
    const uint32_t kind = lex->tokens->items[i].kind;
    if (token_kind_is_significant(kind)) {
      lex->last_significant_index = i;
      lex->line_has_significant = 1;
    }
  }
}

/* ── Identifier + keyword scanner ────────────────────────────────────── */

/* Consumes a maximal run of identifier codepoints. On a complete ident,
 * an exact byte-compare against config->keywords retags it to KEYWORD
 * (v = keyword id); otherwise it's IDENT (v = 0). Lookup is on the whole
 * maximal span, so "lettuce" never matches keyword "let".
 *
 * Returns NO_MATCH if the leading codepoint isn't an ident-start, leaving
 * pos untouched for the next scanner in the dispatch chain. */
static ScanResult try_scan_identifier(Lexer *lex) {
  uint32_t cp;
  int n = peek_cp(lex, &cp);
  if (n == 0 || !is_ident_start_cfg(lex, cp)) return SCAN_NO_MATCH;

  const uint32_t start = lex->pos;
  lex->pos += (uint32_t)n;

  /* Maximal munch. A bad-UTF-8 byte (n == 0) ends the ident cleanly. */
  for (;;) {
    n = peek_cp(lex, &cp);
    if (n == 0 || !is_ident_cont_cfg(lex, cp)) break;
    lex->pos += (uint32_t)n;
  }

  const KavakSpan span = kavak_span_from_to(start, lex->pos);

  /* Keyword retag — linear scan; descriptor-owned tables are usually small. */
  const KavakLexerConfig *cfg = lex->config;
  for (uint32_t i = 0; i < cfg->keyword_count; ++i) {
    const char  *kw    = cfg->keywords[i].text;
    const size_t kwlen = strlen(kw);
    if (kwlen == span.len &&
        memcmp(lex->source->bytes + span.start, kw, kwlen) == 0) {
      /* Soft (contextual) keywords stay IDENT tokens so they remain usable as
       * names; the keyword id rides along in v for context-sensitive parsing. */
      const uint32_t id = cfg->keywords[i].id;
      uint32_t kind = KAVAK_TOK_KEYWORD;
      for (uint32_t j = 0; j < cfg->soft_keyword_count; ++j) {
        if (cfg->soft_keywords[j] == id) { kind = KAVAK_TOK_IDENT; break; }
      }
      return emit_token(lex, kind, span, id) == 0 ? SCAN_MATCHED : SCAN_OOM;
    }
  }
  return emit_token(lex, KAVAK_TOK_IDENT, span, 0) == 0 ? SCAN_MATCHED
                                                        : SCAN_OOM;
}

/* ── Number-literal scanner ──────────────────────────────────────────── */

static uint32_t scan_digits(Lexer *lex, const uint32_t base,
                            const uint32_t flags) {
  uint32_t digit_count = 0;
  while (!at_eof(lex)) {
    const unsigned char c = peek(lex);
    if (is_digit_for_base(c, base)) {
      lex->pos++;
      if (digit_count != UINT32_MAX) digit_count++;
      continue;
    }
    /* Underscore separators run between digits, Java/Kotlin style: a
     * whole `_` run is consumed when a digit of the base follows it
     * (`1__2` is one literal); otherwise the number stops before the
     * run (`1_` is INT `1` then IDENT `_`). */
    if (c == '_' && (flags & KAVAK_NUM_UNDERSCORES) && digit_count > 0) {
      uint32_t run_end = lex->pos;
      while (run_end < (uint32_t)lex->source->len &&
             lex->source->bytes[run_end] == '_') {
        run_end++;
      }
      if (!byte_at_is_digit_for_base(lex, run_end, base)) break;
      lex->pos = run_end;
      continue;
    }
    break;
  }
  return digit_count;
}

static int exponent_starts_here(const Lexer *lex) {
  if (at_eof(lex)) return 0;
  const unsigned char c = peek(lex);
  if (c != 'e' && c != 'E') return 0;

  uint32_t p = lex->pos + 1;
  if (p < (uint32_t)lex->source->len) {
    const unsigned char sign = (unsigned char)lex->source->bytes[p];
    if (sign == '+' || sign == '-') p++;
  }
  return byte_at_is_digit_for_base(lex, p, 10);
}

static void consume_number_suffix(Lexer *lex) {
  const KavakNumberStyle *numbers = &lex->config->numbers;
  if (!numbers->suffixes) return;
  const char *best = NULL;
  size_t best_len = 0;
  for (uint32_t i = 0; i < numbers->suffix_count; ++i) {
    const char *suffix = numbers->suffixes[i];
    if (!suffix || suffix[0] == '\0') continue;
    const size_t len = strlen(suffix);
    if (len > best_len && match_lit(lex, suffix)) {
      best = suffix;
      best_len = len;
    }
  }
  if (!best) return;

  /* Optional strictness: reject a suffix that runs straight into more
   * identifier characters, so "123usize" (suffix "u") is not split into
   * 123u + size but kept as 123 + the identifier "usize". */
  if (numbers->flags & KAVAK_NUM_SUFFIX_REQUIRES_BOUNDARY) {
    const size_t after = (size_t)lex->pos + best_len;
    uint32_t cp = 0;
    const int n = kavak_utf8_decode(lex->source->bytes + after,
                                    lex->source->bytes + lex->source->len, &cp);
    if (n != 0 && is_ident_cont_cfg(lex, cp)) return;  /* boundary violated */
  }

  lex->pos += (uint32_t)best_len;
}

static ScanResult emit_number(Lexer *lex, const uint32_t start,
                              const uint32_t kind) {
  consume_number_suffix(lex);

  /* Strict mode: a literal that runs straight into identifier characters
   * (e.g. `0x1g`, `12abc`) is one malformed token, not a number plus an
   * identifier. Any valid suffix was already consumed above, so `123u` is
   * unaffected. Pull the trailing run in so the diagnostic spans the whole
   * bad token. */
  if (lex->config->numbers.flags & KAVAK_NUM_STRICT_INVALID_DIGITS) {
    uint32_t cp = 0;
    int n = peek_cp(lex, &cp);
    if (n != 0 && is_ident_cont_cfg(lex, cp)) {
      do {
        lex->pos += (uint32_t)n;
        n = peek_cp(lex, &cp);
      } while (n != 0 && is_ident_cont_cfg(lex, cp));
      return emit_invalid(lex, start, "malformed number literal") == 0
               ? SCAN_MATCHED : SCAN_OOM;
    }
  }

  return emit_token(lex, kind, kavak_span_from_to(start, lex->pos), 0) == 0
           ? SCAN_MATCHED : SCAN_OOM;
}

/* Decimal integers are the zero-config baseline. Extra bases are opt-in:
 * when the matching base flag is absent, `0x10` is tokenized as INT `0`
 * followed by IDENT `x10` instead of being treated as a malformed hex
 * literal. Floats/exponents are decimal-only; languages that need
 * `p`-style hex floats provide a descriptor-owned numeric scanner. */
static ScanResult try_scan_number(Lexer *lex) {
  const KavakNumberStyle *numbers = &lex->config->numbers;
  const uint32_t flags = numbers->flags;
  const uint32_t start = lex->pos;

  /* Leading-dot floats (`.5`) are opt-in and need a digit right after
   * the dot, so `1..2` and `.x` are untouched — the number scanner runs
   * before the operator/punct scanner, so `.5` wins over punct `.`. */
  const int leading_dot =
      (flags & KAVAK_NUM_LEADING_DOT) && (flags & KAVAK_NUM_FLOAT) &&
      peek(lex) == '.' && byte_at_is_digit_for_base(lex, lex->pos + 1, 10);

  if (!leading_dot && !byte_at_is_digit_for_base(lex, lex->pos, 10)) {
    return SCAN_NO_MATCH;
  }

  if (!leading_dot &&
      peek(lex) == '0' && lex->pos + 1 < (uint32_t)lex->source->len) {
    const unsigned char marker = (unsigned char)lex->source->bytes[lex->pos + 1];
    uint32_t base = 0;
    if ((marker == 'x' || marker == 'X') && (flags & KAVAK_NUM_BASE_HEX)) base = 16;
    if ((marker == 'o' || marker == 'O') && (flags & KAVAK_NUM_BASE_OCT)) base = 8;
    if ((marker == 'b' || marker == 'B') && (flags & KAVAK_NUM_BASE_BIN)) base = 2;

    if (base != 0) {
      lex->pos += 2;
      if (scan_digits(lex, base, flags) == 0) {
        return emit_invalid(lex, start, "malformed number literal") == 0
                 ? SCAN_MATCHED : SCAN_OOM;
      }
      return emit_number(lex, start, KAVAK_TOK_INT);
    }
  }

  uint32_t kind = KAVAK_TOK_INT;
  if (leading_dot) {
    /* One fraction per literal: `.5.6` lexes as FLOAT `.5`, FLOAT `.6`. */
    kind = KAVAK_TOK_FLOAT;
    lex->pos++;
    (void)scan_digits(lex, 10, flags);
  } else {
    (void)scan_digits(lex, 10, flags);
    if ((flags & KAVAK_NUM_FLOAT) && peek(lex) == '.' &&
        byte_at_is_digit_for_base(lex, lex->pos + 1, 10)) {
      kind = KAVAK_TOK_FLOAT;
      lex->pos++;
      (void)scan_digits(lex, 10, flags);
    }
  }

  if ((flags & KAVAK_NUM_EXPONENT) && exponent_starts_here(lex)) {
    kind = KAVAK_TOK_FLOAT;
    lex->pos++;
    if (!at_eof(lex) && (peek(lex) == '+' || peek(lex) == '-')) lex->pos++;
    (void)scan_digits(lex, 10, flags);
  }

  return emit_number(lex, start, kind);
}

/* ── String + char scanner with escape validation ───────────────────── */

static const KavakStringRule *match_longest_string_rule(const Lexer *lex,
                                                        size_t *out_open_len) {
  const KavakLexerConfig *cfg = lex->config;
  if (!cfg->strings) return NULL;
  const KavakStringRule  *best = NULL;
  size_t                  best_len = 0;
  for (uint32_t i = 0; i < cfg->string_count; ++i) {
    const char *open = cfg->strings[i].open;
    if (!open || open[0] == '\0' || !cfg->strings[i].close) continue;
    const size_t len = strlen(open);
    if (len > best_len && match_lit(lex, open)) {
      best     = &cfg->strings[i];
      best_len = len;
    }
  }
  if (out_open_len) *out_open_len = best_len;
  return best;
}

static int escape_is_allowed(const KavakStringRule *rule, const unsigned char esc) {
  if (!rule->escapes) return 0;
  for (uint32_t i = 0; i < rule->escape_count; ++i) {
    if ((unsigned char)rule->escapes[i].esc == esc) return 1;
  }
  return 0;
}

static int string_rule_allows_newline(const KavakStringRule *rule) {
  return (rule->flags & (KAVAK_STR_FLAG_TRIPLE | KAVAK_STR_FLAG_MULTILINE)) != 0;
}

static int string_rule_emits_char(const KavakStringRule *rule) {
  if (rule->flags & KAVAK_STR_FLAG_CHAR) return 1;
  return strcmp(rule->open, "'") == 0 && strcmp(rule->close, "'") == 0 &&
         (rule->flags & (KAVAK_STR_FLAG_TRIPLE | KAVAK_STR_FLAG_MULTILINE |
                         KAVAK_STR_FLAG_TEMPLATE)) == 0;
}

/* Token kind at the close delimiter. IDENT (quoted identifiers) is
 * checked first so an IDENT-flagged `'…'` rule never falls into the
 * CHAR heuristic. */
static uint32_t string_rule_close_kind(const KavakStringRule *rule) {
  if (rule->flags & KAVAK_STR_FLAG_IDENT) return KAVAK_TOK_IDENT;
  return string_rule_emits_char(rule) ? KAVAK_TOK_CHAR : KAVAK_TOK_STRING;
}

/* For the `$`/NULL dual form a marker followed by neither `{` nor an
 * identifier start is literal text (Kotlin's lone-`$` rule), so the
 * fragment keeps going instead of erroring. Rules with an explicit
 * interp_close always enter the interpolation. */
static int interp_marker_starts_interpolation(const Lexer *lex,
                                              const KavakStringRule *rule) {
  if (rule->interp_close) return 1;
  const uint32_t after = lex->pos + (uint32_t)strlen(rule->interp_open);
  if (after >= (uint32_t)lex->source->len) return 0;
  if (lex->source->bytes[after] == '{') return 1;
  uint32_t cp;
  const int n = kavak_utf8_decode(lex->source->bytes + after,
                                  lex->source->bytes + lex->source->len, &cp);
  return n != 0 && is_ident_start_cfg(lex, cp);
}

static const char *paired_open_for_close(const char *close) {
  if (!close || close[0] == '\0' || close[1] != '\0') return NULL;
  switch (close[0]) {
    case '}': return "{";
    case ')': return "(";
    case ']': return "[";
    default:  return NULL;
  }
}

static int emit_interpolation_invalid(Lexer *lex, const uint32_t start,
                                      const char *message) {
  if (lex->pos <= start) lex->pos = start + 1;
  return emit_invalid(lex, start, message);
}

/* Whitespace inside `${...}` swallows newlines — no layout or auto-semi
 * events fire in interpolation — but emit_newlines still wants the
 * physical NEWLINE tokens. Returns 0, or -1 on token-push OOM. */
static int skip_interpolation_whitespace(Lexer *lex) {
  if (!lex->config->emit_newlines) {
    skip_whitespace(lex);
    return 0;
  }
  while (!at_eof(lex)) {
    const uint32_t n = newline_len_at(lex, lex->pos);
    if (n != 0) {
      if (emit_token(lex, KAVAK_TOK_NEWLINE, kavak_span_make(lex->pos, n), 0) != 0) {
        return -1;
      }
      lex->pos += n;
      continue;
    }
    if (!is_inline_ws(peek(lex))) break;
    lex->pos++;
  }
  return 0;
}

static int scan_interpolation_expression(Lexer *lex, const uint32_t interp_start,
                                         const char *close,
                                         const uint32_t close_kind) {
  if (lex->interp_depth >= KAVAK_MAX_INTERP_DEPTH) {
    return emit_interpolation_invalid(lex, interp_start,
                                      "string interpolation nesting too deep") == 0 ? 0 : -1;
  }

  lex->interp_depth++;
  const size_t close_len = strlen(close);
  const char *paired_open = paired_open_for_close(close);
  uint32_t pair_depth = 0;

  for (;;) {
    if (skip_interpolation_whitespace(lex) != 0) {
      lex->interp_depth--;
      return -1;
    }
    if (at_eof(lex)) {
      lex->interp_depth--;
      return emit_invalid(lex, interp_start, "unterminated string interpolation") == 0 ? 0 : -1;
    }

    if (pair_depth == 0 && close_len > 0 && match_lit(lex, close)) {
      const uint32_t close_start = lex->pos;
      lex->pos += (uint32_t)close_len;
      lex->interp_depth--;
      if (close_kind != 0 &&
          emit_token(lex, close_kind,
                     kavak_span_from_to(close_start, lex->pos), 0) != 0) {
        return -1;
      }
      return 1;
    }

    const uint32_t token_start = lex->pos;
    const int was_paired_open = paired_open && match_lit(lex, paired_open);
    const int was_paired_close = pair_depth > 0 && close_len > 0 && match_lit(lex, close);

    const ScanResult r = scan_token(lex);
    if (r == SCAN_OOM) {
      lex->interp_depth--;
      return -1;
    }
    if (r == SCAN_NO_MATCH) {
      lex->pos++;
      if (emit_invalid(lex, token_start, "unknown character") != 0) {
        lex->interp_depth--;
        return -1;
      }
    } else {
      if (was_paired_open) {
        pair_depth++;
      } else if (was_paired_close) {
        pair_depth--;
      }
    }
  }
}

static int scan_bare_interpolation_identifier(Lexer *lex,
                                              const KavakStringRule *rule,
                                              const uint32_t interp_start) {
  const ScanResult r = try_scan_identifier(lex);
  if (r == SCAN_OOM) return -1;
  if (r == SCAN_MATCHED) {
    if (rule->bare_ident_kind != 0) {
      /* Patch the just-pushed ident in place: one token spanning the
       * whole `$ident` entry. Unconditional, so a KEYWORD retag is
       * overridden too — `$it`-style names stay simple entries. */
      KavakToken *token = &lex->tokens->items[lex->tokens->count - 1];
      token->kind = rule->bare_ident_kind;
      token->span = kavak_span_from_to(interp_start, lex->pos);
      token->v    = 0;
    }
    return 1;
  }
  return emit_interpolation_invalid(lex, interp_start,
                                    "expected identifier after interpolation marker") == 0 ? 0 : -1;
}

/* Marker token over the consumed interpolation opener, emitted eagerly
 * before the body so markers stay stream-ordered. Returns 0 / -1 on
 * token-push OOM. */
static int emit_interp_open_marker(Lexer *lex, const KavakStringRule *rule,
                                   const uint32_t interp_start) {
  if (rule->interp_open_kind == 0) return 0;
  return emit_token(lex, rule->interp_open_kind,
                    kavak_span_from_to(interp_start, lex->pos), 0);
}

static int scan_string_interpolation(Lexer *lex, const KavakStringRule *rule,
                                     const uint32_t interp_start) {
  const size_t open_len = strlen(rule->interp_open);
  lex->pos += (uint32_t)open_len;

  if (rule->interp_close) {
    if (emit_interp_open_marker(lex, rule, interp_start) != 0) return -1;
    return scan_interpolation_expression(lex, interp_start, rule->interp_close,
                                         rule->interp_close_kind);
  }

  /* `$` with NULL close covers both bare identifiers and `${...}`
   * expressions. The descriptor-level close is NULL because the
   * opening marker decides which form this occurrence uses. */
  if (open_len == 1 && rule->interp_open[0] == '$' && match_lit(lex, "{")) {
    lex->pos++;
    if (emit_interp_open_marker(lex, rule, interp_start) != 0) return -1;
    return scan_interpolation_expression(lex, interp_start, "}",
                                         rule->interp_close_kind);
  }

  /* The bare `$ident` form has no opener beyond the `$` — no marker. */
  return scan_bare_interpolation_identifier(lex, rule, interp_start);
}

static ScanResult try_scan_string(Lexer *lex) {
  size_t open_len = 0;
  const KavakStringRule *rule = match_longest_string_rule(lex, &open_len);
  if (!rule) return SCAN_NO_MATCH;

  const uint32_t start = lex->pos;
  const size_t close_len = strlen(rule->close);
  const int is_raw = (rule->flags & KAVAK_STR_FLAG_RAW) != 0;
  const int allows_newline = string_rule_allows_newline(rule);
  const int is_template = (rule->flags & KAVAK_STR_FLAG_TEMPLATE) != 0 &&
                          rule->interp_open && rule->interp_open[0] != '\0';
  uint32_t fragment_start = start;
  int had_interp = 0;            /* literal contains >=1 interpolation */
  lex->pos += (uint32_t)open_len;

  for (;;) {
    if (at_eof(lex)) {
      return emit_invalid(lex, fragment_start, "unterminated string literal") == 0
               ? SCAN_MATCHED : SCAN_OOM;
    }

    const unsigned char c = peek(lex);
    if (!allows_newline && (c == '\n' || c == '\r')) {
      return emit_invalid(lex, fragment_start, "unterminated string literal") == 0
               ? SCAN_MATCHED : SCAN_OOM;
    }

    if (close_len > 0 && match_lit(lex, rule->close)) {
      lex->pos += (uint32_t)close_len;
      /* fragment_kind retags every fragment of an interpolated literal;
       * a literal with no interpolation stays one normal token (v=0). */
      const int tagged = had_interp && rule->fragment_kind != 0;
      const uint32_t kind = tagged ? rule->fragment_kind
                                   : string_rule_close_kind(rule);
      const uint32_t v = tagged ? 3u : 0u;            /* 3 = tail */
      return emit_token(lex, kind, kavak_span_from_to(fragment_start, lex->pos), v) == 0
               ? SCAN_MATCHED : SCAN_OOM;
    }

    if (is_template && match_lit(lex, rule->interp_open) &&
        interp_marker_starts_interpolation(lex, rule)) {
      const uint32_t interp_start = lex->pos;
      had_interp = 1;
      if (fragment_start < interp_start) {
        uint32_t fragment_kind = KAVAK_TOK_STRING;
        uint32_t fragment_v = 0;
        if (rule->fragment_kind != 0) {
          fragment_kind = rule->fragment_kind;
          fragment_v = fragment_start == start ? 1u : 2u; /* head : interior */
        }
        if (emit_token(lex, fragment_kind,
                       kavak_span_from_to(fragment_start, interp_start),
                       fragment_v) != 0) {
          return SCAN_OOM;
        }
      }
      const int interp_result = scan_string_interpolation(lex, rule, interp_start);
      if (interp_result < 0) return SCAN_OOM;
      if (interp_result == 0) return SCAN_MATCHED;
      fragment_start = lex->pos;
      continue;
    }

    if (!is_raw && c == '\\') {
      const uint32_t esc_start = lex->pos;
      lex->pos++;
      if (at_eof(lex)) {
        return emit_invalid(lex, fragment_start, "unterminated string literal") == 0
                 ? SCAN_MATCHED : SCAN_OOM;
      }
      const unsigned char esc = peek(lex);
      if (!allows_newline && (esc == '\n' || esc == '\r')) {
        return emit_invalid(lex, fragment_start, "unterminated string literal") == 0
                 ? SCAN_MATCHED : SCAN_OOM;
      }
      uint32_t cp = 0;
      const int esc_len = esc < 0x80 ? 1 : peek_cp(lex, &cp);
      const uint32_t advance = esc_len ? (uint32_t)esc_len : utf8_recovery_len(lex);
      lex->pos += advance;
      if (esc_len == 0 && esc >= 0x80) {
        push_diag(lex, kavak_span_make(esc_start + 1u, advance),
                  "invalid UTF-8 in string escape");
      }
      if (!escape_is_allowed(rule, esc)) {
        push_diag(lex, kavak_span_from_to(esc_start, lex->pos),
                  "invalid escape sequence");
      }
      continue;
    }

    uint32_t cp;
    const int n = peek_cp(lex, &cp);
    lex->pos += (uint32_t)(n ? n : 1);
    if (n == 0 && c >= 0x80) {
      push_diag(lex, kavak_span_make(lex->pos - 1, 1),
                "invalid UTF-8 in string literal");
    }
  }
}

/* ── Line + block comment scanner ────────────────────────────────────── */

/* Dispatch position 1: comment-open is tried before the operator scanner,
 * so `//` and slash-star beat a bare `/` operator.
 *
 * A rule whose `close` is exactly "\n" is a line comment — it runs to the
 * next newline (or EOF) and leaves the newline in the stream, because
 * layout / auto-semi handling needs to see line ends. Any other `close`
 * is a block comment: the close delimiter is consumed as
 * part of the comment, the body nests when `rule.nest` is set, and running
 * off the end without closing is recoverable (INVALID + diag at EOF).
 *
 * With config.keep_comments every comment emits a KAVAK_TOK_COMMENT
 * spanning the whole comment; with config.keep_doc_comments only the
 * `is_doc` rules do. Any other comment is skipped silently (MATCHED, no
 * token). When several rules could open here the longest `open` wins —
 * a "///" doc rule beats a "//" line rule — so config order does not
 * affect matching. */
static ScanResult try_scan_comment(Lexer *lex) {
  const KavakLexerConfig *cfg = lex->config;

  const KavakCommentRule *rule = NULL;
  size_t open_len = 0;
  for (uint32_t i = 0; i < cfg->comment_count; ++i) {
    const size_t olen = strlen(cfg->comments[i].open);
    if (olen > open_len && match_lit(lex, cfg->comments[i].open)) {
      rule     = &cfg->comments[i];
      open_len = olen;
    }
  }
  if (!rule) return SCAN_NO_MATCH;

  const uint32_t start = lex->pos;
  lex->pos += (uint32_t)open_len;          /* consume the open delimiter */

  const int is_line = rule->close[0] == '\n' && rule->close[1] == '\0';
  if (is_line) {
    while (!at_eof(lex) && !starts_newline(lex)) lex->pos++;
  } else {
    const size_t close_len = strlen(rule->close);
    uint32_t     depth     = 1;
    while (!at_eof(lex)) {
      if (rule->nest && match_lit(lex, rule->open)) {
        lex->pos += (uint32_t)open_len;
        depth++;
      } else if (match_lit(lex, rule->close)) {
        lex->pos += (uint32_t)close_len;
        if (--depth == 0) break;
      } else {
        lex->pos++;            /* comment body — content is not validated */
      }
    }
    if (depth != 0) {
      return emit_invalid(lex, start, "unterminated block comment") == 0
                 ? SCAN_MATCHED : SCAN_OOM;
    }
  }

  if (cfg->keep_comments || (rule->is_doc && cfg->keep_doc_comments)) {
    const KavakSpan span = kavak_span_from_to(start, lex->pos);
    return emit_token(lex, KAVAK_TOK_COMMENT, span, 0) == 0 ? SCAN_MATCHED
                                                            : SCAN_OOM;
  }
  return SCAN_MATCHED;
}

/* ── Operator + punctuation scanner (longest-match) ─────────────────── */

/* The longest operator in config.operators matching at lex->pos, or NULL.
 * Scans the whole table, so `<<=` wins over `<<` over `<`, and `==`
 * over `=` regardless of config order. Multi-byte spellings compare as
 * raw bytes. */
static const KavakOperator *match_longest_op(const Lexer *lex) {
  const KavakLexerConfig *cfg = lex->config;
  const KavakOperator    *best = NULL;
  size_t                  best_len = 0;
  for (uint32_t i = 0; i < cfg->operator_count; ++i) {
    const size_t len = strlen(cfg->operators[i].text);
    if (len > best_len && match_lit(lex, cfg->operators[i].text)) {
      best     = &cfg->operators[i];
      best_len = len;
    }
  }
  return best;
}

/* The kernel's structural punctuation — recognized regardless of the
 * operator table. A language that wants one of these to carry
 * precedence (say `.` as member access) lists it in `operators`, and the
 * operator scan above claims it first. */
static int is_kernel_punct(const unsigned char c) {
  switch (c) {
    case '(': case ')': case '[': case ']': case '{': case '}':
    case ',': case ';': case ':': case '.':
      return 1;
    default:
      return 0;
  }
}

/* Operators first (longest-match → OP, v = op_id), then the structural
 * punctuation path (→ PUNCT, v = ASCII byte). Reached only after comments, strings,
 * numbers, and identifiers decline, so the punct test sees real symbols. */
static ScanResult try_scan_operator(Lexer *lex) {
  const KavakOperator *op = match_longest_op(lex);
  if (op) {
    const uint32_t start = lex->pos;
    lex->pos += (uint32_t)strlen(op->text);
    return emit_token(lex, KAVAK_TOK_OP, kavak_span_from_to(start, lex->pos),
                      op->id) == 0 ? SCAN_MATCHED : SCAN_OOM;
  }

  if (is_kernel_punct(peek(lex))) {
    const uint32_t start = lex->pos;
    const uint32_t v = peek(lex);
    lex->pos++;
    return emit_token(lex, KAVAK_TOK_PUNCT, kavak_span_from_to(start, lex->pos),
                      v) == 0 ? SCAN_MATCHED : SCAN_OOM;
  }
  return SCAN_NO_MATCH;
}

/* ── Scanner dispatch ────────────────────────────────────────────────── */

/* Tries each scanner in longest-match precedence order; the first that
 * doesn't return NO_MATCH wins. The precedence order is fixed (see the
 * file header) — comment-open beats operator `/`, string-open beats a
 * bare `r` ident, etc. Any byte still unclaimed after the wired scanners
 * returns NO_MATCH and the caller emits INVALID. */
static ScanResult scan_token(Lexer *lex) {
  ScanResult r;
  if ((r = try_scan_comment(lex))    != SCAN_NO_MATCH) return r;
  if ((r = try_scan_string(lex))     != SCAN_NO_MATCH) return r;
  if ((r = try_scan_number(lex))     != SCAN_NO_MATCH) return r;
  if ((r = try_scan_identifier(lex)) != SCAN_NO_MATCH) return r;
  if ((r = try_scan_operator(lex))   != SCAN_NO_MATCH) return r;
  return SCAN_NO_MATCH;
}

/* ── Newline-aware wrappers ──────────────────────────────────────────── */

static int line_comment_starts_at(const Lexer *lex, const uint32_t pos) {
  const KavakLexerConfig *cfg = lex->config;
  for (uint32_t i = 0; i < cfg->comment_count; ++i) {
    const KavakCommentRule *rule = &cfg->comments[i];
    if (rule->close && rule->close[0] == '\n' && rule->close[1] == '\0' &&
        rule->open && match_lit_at(lex, pos, rule->open)) {
      return 1;
    }
  }
  return 0;
}

static uint32_t indentation_col_after_byte(Lexer *lex, const unsigned char c,
                                           const uint32_t col, int *bad_unit) {
  const KavakOffsideConfig *cfg = lex->config->offside;
  if (c == ' ') {
    if (cfg->unit == KAVAK_INDENT_TABS) *bad_unit = 1;
    return col + 1;
  }
  if (cfg->unit == KAVAK_INDENT_SPACES) *bad_unit = 1;
  if (cfg->unit == KAVAK_INDENT_TABS) return col + 1;
  return ((col / 8u) + 1u) * 8u;
}

static int emit_indent_token(Lexer *lex, const uint32_t kind, const uint32_t pos) {
  return emit_token(lex, kind, kavak_span_make(pos, 0), 0);
}

static int handle_offside_line_start(Lexer *lex) {
  const KavakOffsideConfig *cfg = lex->config->offside;
  if (!cfg || !lex->at_line_start) return 0;

  const uint32_t indent_start = lex->pos;
  uint32_t col = 0;
  int bad_unit = 0;

  while (!at_eof(lex) && (peek(lex) == ' ' || peek(lex) == '\t')) {
    const unsigned char c = peek(lex);
    col = indentation_col_after_byte(lex, c, col, &bad_unit);
    lex->pos++;
  }

  if (bad_unit) {
    push_diag(lex, kavak_span_from_to(indent_start, lex->pos),
              "indentation uses a disallowed whitespace unit");
  }

  if (at_eof(lex) || starts_newline(lex) || line_comment_starts_at(lex, lex->pos)) {
    return 0;
  }

  const uint32_t point = lex->pos;
  uint32_t current = lex->indent_stack[lex->indent_top];

  if (col > current) {
    if (lex->indent_top + 1u >= KAVAK_MAX_INDENT_DEPTH) {
      /* Recoverable: over-deep nesting is a syntax error, not OOM. Record the
       * diagnostic and keep lexing at the current depth (no INDENT pushed) so
       * the rest of the file stays analyzable. -1 stays reserved for OOM /
       * token-push failure, per the kavak_analyze "NULL only on OOM" contract. */
      push_diag(lex, kavak_span_make(point, 0), "indentation nesting too deep");
      return 0;
    }
    lex->indent_stack[++lex->indent_top] = col;
    return emit_indent_token(lex, KAVAK_TOK_INDENT, point);
  }

  /* Strict dedents: pop every level strictly deeper than this column. */
  while (lex->indent_top > 0u && col < lex->indent_stack[lex->indent_top]) {
    lex->indent_top--;
    if (emit_indent_token(lex, KAVAK_TOK_DEDENT, point) != 0) return -1;
  }
  /* Haskell-style equal-column close: pop the matching level exactly once. This
   * is a deliberate dedent, not a misalignment, so it must not trip the
   * misaligned-dedent diagnostic below (which compares against the *enclosing*
   * level after the pop). */
  int closed_equal = 0;
  if (cfg->dedent_on_lower_or_equal && lex->indent_top > 0u &&
      col == lex->indent_stack[lex->indent_top]) {
    lex->indent_top--;
    closed_equal = 1;
    if (emit_indent_token(lex, KAVAK_TOK_DEDENT, point) != 0) return -1;
  }

  current = lex->indent_stack[lex->indent_top];
  if (!closed_equal && col != current) {
    push_diag(lex, kavak_span_make(point, 0), "misaligned dedent");
  }
  return 0;
}

static int maybe_emit_auto_semicolon(Lexer *lex, const uint32_t pos) {
  const KavakAutoSemicolon *auto_semi = lex->config->auto_semi;
  if (!auto_semi || !auto_semi->is_terminating || !lex->line_has_significant) return 0;
  const KavakToken *token = &lex->tokens->items[lex->last_significant_index];
  if (!auto_semi->is_terminating(token)) return 0;
  return emit_token(lex, auto_semi->emit_kind, kavak_span_make(pos, 0),
                    auto_semi->emit_v);
}

static int finish_physical_line(Lexer *lex) {
  const uint32_t start = lex->pos;
  const uint32_t newline_len = newline_len_at(lex, start);

  if (maybe_emit_auto_semicolon(lex, start) != 0) return -1;
  /* Offside NEWLINEs are gated on the line having a significant token;
   * emit_newlines is ungated — every physical newline emits, blank and
   * comment-only lines included. */
  if (lex->config->emit_newlines ||
      (lex->config->offside && lex->line_has_significant)) {
    if (emit_token(lex, KAVAK_TOK_NEWLINE, kavak_span_make(start, newline_len), 0) != 0) {
      return -1;
    }
  }

  (void)consume_newline(lex);
  lex->at_line_start = 1;
  lex->line_has_significant = 0;
  return 0;
}

static int drain_indentation(Lexer *lex, const uint32_t pos) {
  while (lex->indent_top > 0u) {
    lex->indent_top--;
    if (emit_indent_token(lex, KAVAK_TOK_DEDENT, pos) != 0) return -1;
  }
  return 0;
}

static int finish_eof_layout(Lexer *lex) {
  const uint32_t pos = lex->pos;
  if (maybe_emit_auto_semicolon(lex, pos) != 0) return -1;
  /* The virtual len-0 NEWLINE at EOF is offside-only; emit_newlines
   * covers physical newlines and adds nothing here. */
  if (lex->config->offside && lex->line_has_significant) {
    if (emit_token(lex, KAVAK_TOK_NEWLINE, kavak_span_make(pos, 0), 0) != 0) return -1;
  }
  if (lex->config->offside && drain_indentation(lex, pos) != 0) return -1;
  lex->line_has_significant = 0;
  return 0;
}

int kavak_lex(const KavakSource      *source,
              const KavakLexerConfig *config,
              KavakTokenVec          *out_tokens,
              KavakDiagVec           *out_diags) {
  if (!source || !out_tokens || !lexer_config_valid(config)) return -1;
  if (source->len > UINT32_MAX) return -1;
  if (source->len != 0 && !source->bytes) return -1;

  Lexer lex = {
    .source = source,
    .config = config,
    .tokens = out_tokens,
    .diags  = out_diags,
    .pos    = 0,
    .indent_stack = { 0 },
    .indent_top = 0,
    .last_significant_index = 0,
    .interp_depth = 0,
    .at_line_start = 1,
    .line_has_significant = 0,
  };

  const int newline_aware = config->offside || config->auto_semi ||
                            config->emit_newlines;

  for (;;) {
    /* Reserve headroom at the token-count ceiling so the closing layout
     * (drained DEDENTs + NEWLINE) and the final EOF token always fit; otherwise
     * a pathological >4G-token source could fail the EOF push like an OOM. */
    if (out_tokens->count >= UINT32_MAX - (KAVAK_MAX_INDENT_DEPTH + 4u)) {
      push_diag(&lex, kavak_span_make(lex.pos, 0),
                "source exceeds the token limit; truncated");
      break;
    }
    if (newline_aware) {
      if (lex.at_line_start && handle_offside_line_start(&lex) != 0) return -1;
      skip_inline_whitespace(&lex);
      if (at_eof(&lex)) break;
      if (starts_newline(&lex)) {
        if (finish_physical_line(&lex) != 0) return -1;
        continue;
      }
    } else {
      skip_whitespace(&lex);
      if (at_eof(&lex)) break;
    }

    lex.at_line_start = 0;
    const uint32_t first_new_token = out_tokens->count;
    const ScanResult r = scan_token(&lex);
    if (r == SCAN_OOM) return -1;
    if (r == SCAN_NO_MATCH) {
      /* No scanner claimed this byte. Single-byte advance = recovery
       * doesn't lose the rest of the file on one bad codepoint. */
      const uint32_t start = lex.pos;
      lex.pos++;
      if (emit_invalid(&lex, start, "unknown character") != 0) return -1;
    }
    note_emitted_tokens(&lex, first_new_token);
  }

  if (newline_aware && finish_eof_layout(&lex) != 0) return -1;

  /* Final EOF anchor — always emitted, even after errors. Span is
   * a zero-length point at end-of-source so consumers can use it as
   * a stable end-of-stream sentinel. */
  if (emit_token(&lex, KAVAK_TOK_EOF, kavak_span_make(lex.pos, 0), 0) != 0) return -1;
  return 0;
}
