// SPDX-License-Identifier: MIT
/**
 * @file kavak.h
 * @brief kavak — embeddable frontend toolkit for language implementations.
 * @version 0.9.0
 *
 * DESIGN PRINCIPLES
 *
 *   - One descriptor per language. KavakLanguage is link-time `const`;
 *     all runtime-mutable state lives in KavakSession (operator tables,
 *     diagnostic vec, type arena, per-analysis caches).
 *   - Public structs with direct field access for descriptor/backend code.
 *     Consumer-facing stability comes from the accessor APIs.
 *   - Layered. Use only the levels you need: a lightweight config DSL can
 *     stop at source/lexer primitives; a full language frontend uses parser,
 *     type, and sema.
 *   - Pratt parser with a per-language operator table. Recursive-descent
 *     helpers carry decls and statements. Speculative parsing through
 *     kavak_parser_checkpoint / kavak_parser_rewind for grammars that need it.
 *   - Diagnostics carry KavakSpan; line/col are derived on demand from
 *     KavakSource's line-offset table. No redundant cached coords.
 *   - Arena allocation for AST and TypeInfo (bulk-free per session). No
 *     per-node cleanup. Hot paths after init are allocation-free.
 *
 * AUDIENCES
 *
 *   This header is organized by role. Read the tier(s) you implement
 *   against and skip the rest.
 *
 *     CONSUMER API     (Sections 1-5)
 *       An IDE / linter / compiler-backend that analyzes source via a
 *       kavak-built frontend.
 *       Calls kavak_analyze, walks the AST, formats errors. Stops here.
 *
 *     DESCRIPTOR API   (Sections 6-13)
 *       A language author implementing a kavak frontend. Fills in
 *       KavakLanguage with keyword + operator tables, parser hooks,
 *       and sema rules. Reads everything above plus this tier.
 *
 *     BACKEND ABI      (Sections 14-15)
 *       A code generator turning analyzed AST into IR / WASM / native.
 *       Reads modifier bitmask layout and the type-arena shape only.
 *
 *     PRIMITIVES       (Sections 16-19)
 *       The substrate — arena, span, raw diagnostic vec, UTF-8.
 *       Used by descriptor and backend audiences directly. Consumers
 *       see them only through the accessor surface in Section 4.
 *
 * VERSIONING + BREAKING CHANGE POLICY
 *
 *   KAVAK_VERSION_*       Library version (this header).
 *   KavakLanguage.version Per-language descriptor version. Orthogonal
 *                         to the library version.
 *
 *   0.x releases are pre-1.0: API and ABI may change between minor
 *   versions while the descriptor and runtime surfaces settle. The tier
 *   notes below describe the intended stability target once the public
 *   API reaches 1.0.
 *
 *     CONSUMER API     Stable accessor surface.
 *     DESCRIPTOR API   Source-stable across compatible library versions.
 *     BACKEND ABI      Layout-sensitive; codegen tools should pin a
 *                      kavak version.
 *     PRIMITIVES       Public structs are descriptor-facing, not a binary
 *                      compatibility promise before 1.0.
 *
 * QUICK START (consumer)
 *
 *   #include <kavak.h>
 *   #include <my_language.h>
 *
 *   extern const KavakLanguage MY_LANGUAGE;
 *
 *   KavakSession *s = kavak_session_new(&MY_LANGUAGE);
 *   KavakResult  *r = kavak_analyze(s, source, "main.kv");
 *
 *   for (uint32_t i = 0; i < kavak_error_count(r); ++i) {
 *     const char *msg; uint32_t line, col;
 *     kavak_error_at(r, i, &msg, &line, &col);
 *     fprintf(stderr, "main.kv:%u:%u: %s\n", line, col, msg);
 *   }
 *
 *   const KavakASTNode *root = kavak_root(r);
 *   // walk root with first_child / next_sibling, lower to your IR
 *
 *   kavak_result_free(r);
 *   kavak_session_free(s);
 *
 * SECTIONS
 *
 *   ╔═══ CONSUMER API ═══════════════════════════════════════════╗
 *      1.  Source              — input
 *      2.  Analyze             — one-call entry
 *      3.  Read result         — root, tokens, source
 *      4.  Errors              — count, message, line, col
 *      5.  Dump                — text / JSON / S-expr
 *   ╠═══ DESCRIPTOR API ═════════════════════════════════════════╣
 *      6.  Tokens              — KavakToken, kinds, vec
 *      7.  Lexer config        — KavakLexerConfig
 *      8.  Parser toolkit      — Pratt + RD helpers
 *      9.  AST                 — KavakASTNode + payload
 *     10.  Types               — KavakTypeInfo, builtins
 *     11.  Sema toolkit        — scope, narrowing
 *     12.  Session             — KavakSession lifecycle
 *     13.  KavakLanguage       — the descriptor
 *   ╠═══ BACKEND ABI ════════════════════════════════════════════╣
 *     14.  Modifier bits       — per-language ASTNode bitmask
 *     15.  Type arena          — TypeArena
 *   ╠═══ PRIMITIVES ═════════════════════════════════════════════╣
 *     16.  Diagnostics raw     — KavakDiagVec, severity, format
 *     17.  Span                — half-open byte ranges
 *     18.  Arena               — generic byte bump allocator
 *     18b. Intern pool         — pointer-identity strings
 *     19.  UTF-8               — decode, advance, Unicode hooks
 *   ╚════════════════════════════════════════════════════════════╝
 *
 * SOURCE OF TRUTH
 *
 *   This header is the public API contract. README.md tracks package status
 *   and release gates. When prose conflicts with this header, this header
 *   wins.
 */

#ifndef KAVAK_H
#define KAVAK_H

#define KAVAK_VERSION_MAJOR 0
#define KAVAK_VERSION_MINOR 9
#define KAVAK_VERSION_PATCH 0
#define KAVAK_VERSION_STRING "0.9.0"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define KAVAK_INLINE static inline __attribute__((unused))
#else
#define KAVAK_INLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ──────────────────────────────────────────────────
 * Definitions land in their respective sections below. Listing them up
 * front lets sections reference each other without ordering constraints.
 *
 * Two substrate types (KavakSpan, KavakSeverity) get their full
 * definition here rather than just a forward decl — they're embedded
 * by-value into types defined in higher tiers (KavakToken, KavakDiag,
 * KavakASTNode), and a forward decl alone won't satisfy the C
 * "complete type required for a struct field" rule. The helpers
 * (KAVAK_SPAN_NONE, kavak_span_make, kavak_span_union, …) still live
 * in the Primitives tier where they belong (Section 17). */

typedef struct KavakArena       KavakArena;
typedef struct KavakArenaChunk  KavakArenaChunk;   /* opaque */
typedef struct KavakInternPool  KavakInternPool;   /* opaque */
typedef struct KavakSource      KavakSource;
typedef struct KavakDiag        KavakDiag;
typedef struct KavakDiagVec     KavakDiagVec;
typedef struct KavakToken       KavakToken;
typedef struct KavakTokenVec    KavakTokenVec;
typedef union  KavakASTPayload  KavakASTPayload;
typedef struct KavakASTNode     KavakASTNode;
typedef struct KavakTypeInfo    KavakTypeInfo;
typedef struct KavakTypeArena   KavakTypeArena;
typedef struct KavakTypeArenaChunk KavakTypeArenaChunk; /* opaque */
typedef struct KavakRecordField KavakRecordField;
typedef struct KavakTypeSubst   KavakTypeSubst;
typedef struct KavakParser      KavakParser;
typedef struct KavakParserConfig KavakParserConfig;
typedef struct KavakParserCheckpoint KavakParserCheckpoint;
typedef struct KavakSema        KavakSema;
typedef struct KavakSymbol      KavakSymbol;
typedef struct KavakNarrowing   KavakNarrowing;
typedef struct KavakSession     KavakSession;
typedef struct KavakResult      KavakResult;
typedef struct KavakLanguage    KavakLanguage;
typedef struct KavakLexerConfig KavakLexerConfig;
typedef struct KavakKeyword     KavakKeyword;
typedef struct KavakOperator    KavakOperator;
typedef struct KavakScope       KavakScope;

/* Substrate types (full definition early, see comment above). */
typedef struct KavakSpan {
  uint32_t start;
  uint32_t len;
} KavakSpan;

typedef enum {
  KAVAK_SEV_ERROR   = 0,
  KAVAK_SEV_WARNING = 1,
  KAVAK_SEV_NOTE    = 2,
} KavakSeverity;

/** Library version string. */
const char *kavak_version(void);


/* ════════════════════════════════════════════════════════════════════════════
 *
 *                          C O N S U M E R A P I
 *
 *   Sections 1-5. What you call to analyze a source file via a kavak-built
 *   frontend. The other tiers are not needed for this audience.
 *
 * ════════════════════════════════════════════════════════════════════════════ */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  1. Source — input bytes and line mapping                              │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 *   KavakSource src;
 *   if (kavak_source_init(&src, code, len, "main.kv") != 0) {
 *     // allocation failure
 *   }
 *   ...
 *   kavak_source_free(&src);
 *
 * The bytes and filename are borrowed — kavak just stores the pointers,
 * the caller keeps them alive. Source init maps bytes and line starts;
 * UTF-8 validity is a lexer diagnostic. The line-offset table is owned,
 * freed by kavak_source_free. */

struct KavakSource {
  const char *bytes;            /**< Source text. Not necessarily NUL-terminated. */
  size_t      len;              /**< Byte count.                                  */
  const char *filename;         /**< For diagnostics. Borrowed.                   */
  uint32_t    newline_flags;    /**< KAVAK_NEWLINE_* physical-line policy.        */
  uint32_t   *line_offsets;     /**< line_offsets[i] = byte offset where line
                                 *   (i+1) begins. [0] = 0; trailing entry
                                 *   = len, the EOF sentinel. Built lazily on
                                 *   the first kavak_source_pos(); NULL until
                                 *   then — use the accessor, not this field.    */
  uint32_t    line_count;       /**< Physical line count (≥ 1), 0 until the
                                 *   line table is built. See kavak_source_pos.  */
};

#define KAVAK_NEWLINE_LF       (1u << 0)  /* U+000A */
#define KAVAK_NEWLINE_CRLF     (1u << 1)  /* U+000D U+000A */
#define KAVAK_NEWLINE_CR       (1u << 2)  /* U+000D */
#define KAVAK_NEWLINE_UNICODE  (1u << 3)  /* U+0085, U+2028, U+2029 */
#define KAVAK_NEWLINE_DEFAULT  (KAVAK_NEWLINE_LF | KAVAK_NEWLINE_CRLF | \
                                KAVAK_NEWLINE_CR)

/** Borrows `bytes` and records the newline policy (KAVAK_NEWLINE_DEFAULT).
 *  The line-offset table is built lazily on the first kavak_source_pos().
 *  Returns 0 on success, -1 on invalid input (oversize / NULL bytes with
 *  non-zero len), with struct fields zeroed on failure. */
int  kavak_source_init   (KavakSource *source, const char *bytes, size_t len,
                          const char *filename);

/** Like kavak_source_init, but with an explicit physical-line policy.
 *  Pass 0 for KAVAK_NEWLINE_DEFAULT. */
int  kavak_source_init_with_newlines(KavakSource *source,
                                     const char *bytes,
                                     size_t len,
                                     const char *filename,
                                     uint32_t newline_flags);

/** Releases the line-offset table; bytes and filename are not freed. */
void kavak_source_free   (KavakSource *source);

/** Returns the byte length of the newline sequence starting at byte_pos,
 *  or 0 when no configured newline starts there. Pass 0 for
 *  KAVAK_NEWLINE_DEFAULT. */
uint32_t kavak_source_newline_len(const KavakSource *source,
                                  size_t byte_pos,
                                  uint32_t newline_flags);

/** Converts a byte position (0-based, must be ≤ len) to 1-based line and
 *  byte column. Either out param may be NULL. O(log line_count). */
void kavak_source_pos    (const KavakSource *source, size_t byte_pos,
                          uint32_t *out_line, uint32_t *out_col);

/** Borrowed pointer to the bytes a span covers, with *out_len set to the
 *  in-bounds byte length (clamped to the buffer). NULL when the span starts at
 *  or past the end, or the source has no bytes. Not NUL-terminated — pair with
 *  an intern pool (Section 18b) to materialize a stable name string. */
const char *kavak_source_slice(const KavakSource *source, KavakSpan span,
                               size_t *out_len);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  2. Analyze — tokenize, parse, resolve types                            │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Runs the descriptor's lexer, parser, and sema hooks over one source
 * string and returns an opaque result. `kavak_analyze` takes a
 * NUL-terminated string; use `kavak_analyze_bytes` for known-length input.
 *
 *   KavakSession *s = kavak_session_new(&MY_LANGUAGE);
 *   KavakResult  *r = kavak_analyze(s, code, "main.kv");
 *   ...
 *   kavak_result_free(r);
 *
 * Error model: analysis never fails silently. A NULL return means out-of-memory
 * or invalid caller input (NULL session/source). Syntax and semantic errors are
 * reported through the Section 4 accessors, and the AST is still produced as a
 * best-effort partial tree (nodes carry KAVAK_AST_FLAG_ERROR — see Section 9). */

KavakResult *kavak_analyze     (KavakSession *session,
                                const char   *source,
                                const char   *filename);
KavakResult *kavak_analyze_bytes(KavakSession *session,
                                 const char   *bytes,
                                 size_t        len,
                                 const char   *filename);

void         kavak_result_free (KavakResult *r);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  3. Read result — root, tokens, source                                  │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * All accessors return data owned by `r`. Pointers stay valid until
 * kavak_result_free.
 *
 * KavakResult is opaque — its struct shape is intentionally not exposed
 * in this header. Read everything through these accessors and the
 * Section 4 diagnostic surface. The internal layout may change between
 * minor versions; consumer code must use the accessor surface. */

const KavakASTNode *kavak_root         (const KavakResult *r);
const KavakSource  *kavak_result_source(const KavakResult *r);
const KavakToken   *kavak_tokens       (const KavakResult *r);
size_t              kavak_token_count  (const KavakResult *r);
/* The descriptor that produced this result (borrowed). Lets tooling reach the
 * language's optional hooks — e.g. the dump hooks in Section 5. */
const KavakLanguage *kavak_result_language(const KavakResult *r);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  4. Errors — diagnostics for consumers                                  │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Stable accessors over the diagnostic vec inside KavakResult. The raw
 * KavakDiagVec lives in Section 16 (Primitives) — descriptor authors
 * push directly; consumers read through these accessors only. */

uint32_t    kavak_error_count   (const KavakResult *r);
const char *kavak_error_message (const KavakResult *r, uint32_t i);
uint32_t    kavak_error_line    (const KavakResult *r, uint32_t i);
uint32_t    kavak_error_col     (const KavakResult *r, uint32_t i);

/* Resolves the i-th error ONCE and writes its message/line/col (any out-param
 * may be NULL). Returns 1 if the i-th error exists, else 0 (out-params cleared).
 * Prefer this in a count-driven loop: kavak_error_message/line/col each rescan
 * the diagnostic list, so calling all three per index is O(n²); this is a single
 * pass per index. */
int         kavak_error_at      (const KavakResult *r, uint32_t i,
                                 const char **out_message,
                                 uint32_t *out_line, uint32_t *out_col);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  5. Dump — serialize the AST                                            │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Lightweight debug serializers over the generic AST shape. Language
 * frontends may ship richer dumpers for their own user AST kinds. */

void kavak_dump_text  (const KavakResult *r, FILE *out);
void kavak_dump_json  (const KavakResult *r, FILE *out);
void kavak_dump_sexpr (const KavakResult *r, FILE *out);

/* All three dumpers consult the result's descriptor (Section 13): if it sets
 * `ast_kind_name`, kinds print by name; if it sets `ast_payload`, each node's
 * payload (operator, literal text) is appended. Both are optional. */

/* Writes a JSON-escaped copy of s[0..len) into `buf`, NUL-terminated and
 * truncated to buf_len (like kavak_ty_to_string), escaping ", \\, and control
 * characters. Returns the length that would be written excluding the NUL — a
 * reusable primitive for descriptor-authored JSON dumpers. */
size_t kavak_json_escape(const char *s, size_t len, char *buf, size_t buf_len);


/* ════════════════════════════════════════════════════════════════════════════
 *
 *                          D E S C R I P T O R   A P I
 *
 *   Sections 6-13. What you fill in to register a language as a kavak
 *   frontend. A static `KavakLanguage` in your descriptor source file
 *   aggregates everything below.
 *
 * ════════════════════════════════════════════════════════════════════════════ */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  6. Tokens — what the lexer produces                                     │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * KavakToken is a 16-byte struct: kind (4) + span (8) + v (4). Position
 * + length live in `span` directly (Section 17). line/col are not
 * cached — derive on demand from KavakSource via kavak_source_pos().
 *
 * Token kinds 0..255 are reserved for the kernel; languages extend
 * from KAVAK_TOK_USER_BASE upward. */

#define KAVAK_TOK_USER_BASE 256u

typedef enum {
  KAVAK_TOK_INVALID    = 0,
  KAVAK_TOK_EOF        = 1,
  KAVAK_TOK_IDENT      = 2,
  KAVAK_TOK_KEYWORD    = 3,
  KAVAK_TOK_INT        = 4,
  KAVAK_TOK_FLOAT      = 5,
  KAVAK_TOK_STRING     = 6,
  KAVAK_TOK_CHAR       = 7,
  KAVAK_TOK_OP         = 8,
  KAVAK_TOK_PUNCT      = 9,        /* `,` `;` `:` etc.; v = ASCII byte */
  KAVAK_TOK_NEWLINE    = 10,       /* offside layout or emit_newlines      */
  KAVAK_TOK_INDENT     = 11,
  KAVAK_TOK_DEDENT     = 12,
  KAVAK_TOK_COMMENT    = 13,       /* keep_comments / keep_doc_comments    */
  /* 14..255 reserved for kernel token kinds */
} KavakTokenKind;

struct KavakToken {
  uint32_t   kind;       /* KAVAK_TOK_* or KAVAK_TOK_USER_BASE+N           */
  KavakSpan  span;       /* {start, len} — see Section 17                   */
  uint32_t   v;          /* tag-discriminated — read via the accessors
                          * below, never directly. See lifetime + meaning
                          * notes there.                                    */
};

/* Tag-discriminated accessors for KavakToken.v. Each asserts the kind
 * before returning, so debug builds catch accidental cross-kind reads
 * (e.g., reading op_id off an IDENT token). Release builds compile the
 * assert away — pure single-load, no overhead.
 *
 * The descriptor sets v through the same kind discipline: write
 * tok.v = my_keyword_id only when tok.kind == KAVAK_TOK_KEYWORD. */
KAVAK_INLINE uint32_t kavak_token_keyword_id(const KavakToken *token) {
  assert(token->kind == KAVAK_TOK_KEYWORD);
  return token->v;
}
KAVAK_INLINE uint32_t kavak_token_op_id(const KavakToken *token) {
  assert(token->kind == KAVAK_TOK_OP);
  return token->v;
}
/* Soft (contextual) keyword id carried on a KAVAK_TOK_IDENT token: the language
 * enum value when this identifier spells a keyword listed in the lexer config's
 * soft_keywords, else 0 (a plain identifier). Hard keywords lex as
 * KAVAK_TOK_KEYWORD instead — read those with kavak_token_keyword_id. This lets
 * `var`/`async`/`type`-style words be keywords in context yet usable as
 * ordinary names elsewhere. */
KAVAK_INLINE uint32_t kavak_token_soft_keyword_id(const KavakToken *token) {
  assert(token->kind == KAVAK_TOK_IDENT);
  return token->v;
}

struct KavakTokenVec {
  KavakToken *items;
  uint32_t    count;
  uint32_t    cap;
};

void kavak_token_vec_init    (KavakTokenVec *vector);
void kavak_token_vec_free    (KavakTokenVec *vector);

/** Appends `token` (doubling-grow). Returns 0 / -1 on out-of-memory. */
int  kavak_token_vec_push    (KavakTokenVec *vector, KavakToken token);

/** Reserves capacity for at least `n` tokens. Returns 0 / -1 on out-of-memory. */
int  kavak_token_vec_reserve (KavakTokenVec *vector, uint32_t n);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  7. Lexer config — keywords, operators, comments, strings               │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * KavakLexerConfig is what a descriptor fills in to say "this is how my
 * language tokenizes". The kernel's lexer (kavak_lex) reads the config
 * and emits a token stream — KavakToken values from Section 6 — into a vec.
 * Descriptor authors who need more control bypass kavak_lex entirely
 * and roll their own using KavakSource / KavakSpan / KavakArena
 * primitives.
 *
 * QUICK START (descriptor side):
 *
 *   static const KavakKeyword TINY_KEYWORDS[] = {
 *     { "fn",   1 }, { "let",  2 }, { "if", 3 }, { "else", 4 },
 *   };
 *   static const KavakOperator TINY_OPS[] = {
 *     { "+",  10, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX,  1 },
 *     { "*",  20, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX,  2 },
 *     { "==",  5, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX,  3 },
 *     { "&&",  3, KAVAK_ASSOC_LEFT,  KAVAK_OP_FLAG_INFIX
 *                                  | KAVAK_OP_FLAG_SHORT_CIRCUIT, 4 },
 *   };
 *   static const KavakCommentRule TINY_COMMENTS[] = {
 *     { .open = "//", .close = "\n", .nest = 0, .is_doc = 0 },
 *   };
 *   static const KavakLexerConfig TINY_LEXER = {
 *     .keywords       = TINY_KEYWORDS,
 *     .keyword_count  = sizeof(TINY_KEYWORDS)/sizeof(*TINY_KEYWORDS),
 *     .operators      = TINY_OPS,
 *     .operator_count = sizeof(TINY_OPS)/sizeof(*TINY_OPS),
 *     .comments       = TINY_COMMENTS,
 *     .comment_count  = 1,
 *     .numbers.flags  = KAVAK_NUM_BASE_DEC,
 *     // strings = NULL/0 → no string literals
 *     // is_ident_start = NULL → ASCII default (kavak_ascii_is_ident_start)
 *     // offside = NULL, auto_semi = NULL → free-form whitespace
 *   };
 *
 *   KavakTokenVec tokens; kavak_token_vec_init(&tokens);
 *   KavakDiagVec  diags;  kavak_diag_vec_init(&diags);
 *   kavak_lex(&source, &TINY_LEXER, &tokens, &diags);
 *
 * The lexer always emits a final KAVAK_TOK_EOF. Recoverable errors
 * (bad UTF-8, unterminated string, unknown character) push to `diags`
 * and the lexer emits a KAVAK_TOK_INVALID token over the bad span,
 * then keeps going. Returns 0 on success, non-zero on allocation failure. */

/* ── Operator associativity ─────────────────────────────────────────── */

typedef enum {
  KAVAK_ASSOC_LEFT  = 0,
  KAVAK_ASSOC_RIGHT = 1,
  KAVAK_ASSOC_NONE  = 2,
} KavakAssoc;

/* Operator role flags. An operator with multiple flags can be parsed in
 * any of the named positions — `-` is INFIX | PREFIX, `!` is PREFIX
 * only, `++` is PREFIX | POSTFIX, etc. SHORT_CIRCUIT marks `&&` / `||`
 * / `??` for flow-sensitive narrowing. */
#define KAVAK_OP_FLAG_INFIX           (1u << 0)
#define KAVAK_OP_FLAG_PREFIX          (1u << 1)
#define KAVAK_OP_FLAG_POSTFIX         (1u << 2)
#define KAVAK_OP_FLAG_SHORT_CIRCUIT   (1u << 3)
#define KAVAK_OP_FLAG_SELECTOR        (1u << 7)  /* LHS . RHS selector */

/* ── Keyword + operator tables ──────────────────────────────────────── */

struct KavakKeyword {
  const char *text;     /* NUL-terminated source spelling, e.g. "let" */
  uint32_t    id;       /* language enum value (LANG_KW_LET, ...)     */
};

struct KavakOperator {
  const char *text;     /* source spelling, e.g. "+", "??", "===" */
  uint16_t    prec;     /* binding power (higher = tighter)        */
  uint8_t     assoc;    /* KavakAssoc                              */
  uint8_t     flags;    /* KAVAK_OP_FLAG_*                         */
  uint32_t    id;       /* language enum value (LANG_OP_ADD, ...)  */
};

/* ── Comment rules ──────────────────────────────────────────────────── */

typedef struct KavakCommentRule {
  const char *open;     /* line: "//" / "#" ; block: slash-star, "(*"  */
  const char *close;    /* line: "\n" ; block: star-slash, "*)"        */
  uint8_t     nest;     /* 1 if block comments nest (Rust-style)       */
  uint8_t     is_doc;   /* 1 if this style is a doc comment            */
} KavakCommentRule;

/* ── Number literal config ──────────────────────────────────────────── */

#define KAVAK_NUM_BASE_DEC      (1u << 0)   /* decimal — the implied baseline */
#define KAVAK_NUM_BASE_HEX      (1u << 1)   /* `0x` prefix             */
#define KAVAK_NUM_BASE_OCT      (1u << 2)   /* `0o` prefix             */
#define KAVAK_NUM_BASE_BIN      (1u << 3)   /* `0b` prefix             */
#define KAVAK_NUM_UNDERSCORES   (1u << 4)   /* allow `_` separator     */
#define KAVAK_NUM_FLOAT         (1u << 5)   /* allow `.` decimal point */
#define KAVAK_NUM_EXPONENT      (1u << 6)   /* allow `e` / `E` form    */
#define KAVAK_NUM_LEADING_DOT   (1u << 7)   /* `.5` floats (with FLOAT) */
#define KAVAK_NUM_SUFFIX_REQUIRES_BOUNDARY (1u << 8)
                                            /* a matched suffix is only
                                             * accepted when the next character
                                             * is NOT identifier-continue, so
                                             * `123usize` (suffix "u") stays
                                             * 123 + `usize` instead of splitting
                                             * into 123u + size. Off by default
                                             * (longest match wins, no boundary). */
#define KAVAK_NUM_STRICT_INVALID_DIGITS (1u << 9)
                                            /* a number that runs straight into
                                             * identifier characters (e.g.
                                             * `0x1g`, `12abc`) is a single
                                             * "malformed number literal" token
                                             * rather than a number followed by
                                             * an identifier. Off by default
                                             * (the trailing run lexes as IDENT).
                                             * Any matched suffix is consumed
                                             * first, so a valid `123u` is not
                                             * affected. */

typedef struct KavakNumberStyle {
  uint32_t           flags;          /* KAVAK_NUM_* bits */
  /* Optional suffix table — "L", "u", "f32", "_u8", etc. The lexer
   * tries each suffix after the digits; longest match wins. With
   * KAVAK_NUM_SUFFIX_REQUIRES_BOUNDARY the match must also end at a
   * non-identifier boundary. NULL/0 means no suffixes are allowed. */
  const char *const *suffixes;
  uint32_t           suffix_count;
} KavakNumberStyle;

/* ── String literal config ──────────────────────────────────────────── */

/* A single escape mapping inside a string literal. The lexer reads
 * `\` + `esc` and emits the codepoint `replacement`. For escapes
 * that produce multiple codepoints (rare), the descriptor handles
 * them via a custom string scanner; this table is the simple case. */
typedef struct KavakEscape {
  char     esc;          /* the char after backslash, e.g. 'n', 't' */
  uint32_t replacement;  /* the codepoint produced, e.g. 0x0A       */
} KavakEscape;

/* String-rule flags. A rule with no flags is a plain "C-style" string.
 * IDENT marks a quoted-identifier rule (Kotlin backticks): the token is
 * emitted as KAVAK_TOK_IDENT spanning both delimiters, with no keyword
 * retag — `` `if` `` stays an identifier. */
#define KAVAK_STR_FLAG_RAW        (1u << 0)  /* don't process escapes      */
#define KAVAK_STR_FLAG_TEMPLATE   (1u << 1)  /* allow $/${ interpolation   */
#define KAVAK_STR_FLAG_TRIPLE     (1u << 2)  /* triple-quoted (multiline)  */
#define KAVAK_STR_FLAG_MULTILINE  (1u << 3)  /* allow raw newlines inside  */
#define KAVAK_STR_FLAG_CHAR       (1u << 4)  /* emit KAVAK_TOK_CHAR        */
#define KAVAK_STR_FLAG_IDENT      (1u << 5)  /* emit KAVAK_TOK_IDENT       */

typedef struct KavakStringRule {
  const char        *open;            /* `"`, `'`, `r"`, `"""`, `<<<`     */
  const char        *close;           /* matching close                    */
  uint32_t           flags;           /* KAVAK_STR_FLAG_*                  */
  /* Interpolation markers (TEMPLATE flag). NULL/NULL = no interpolation
   * even if TEMPLATE is set. Examples: `$` / NULL for balanced `${...}`,
   * or `\(` / `)` for parenthesized interpolation. */
  const char        *interp_open;
  const char        *interp_close;
  /* Escape table — NULL/0 means no escapes (raw-by-default). */
  const KavakEscape *escapes;
  uint32_t           escape_count;
  /* Template token remapping — all 0 = plain STRING fragments and
   * unmarked expression tokens. Kinds are typically
   * KAVAK_TOK_USER_BASE + N. Only meaningful with TEMPLATE.
   *
   * fragment_kind: when the literal contains at least one interpolation,
   * its fragments are emitted with this kind and v = 1 (head, starts
   * with the open delimiter), 2 (interior), or 3 (tail, ends with the
   * close delimiter). A literal with NO interpolation stays one normal
   * STRING/CHAR/IDENT token with v = 0 even when fragment_kind is set. */
  uint32_t           fragment_kind;
  uint32_t           interp_open_kind;   /* marker (v=0) over the consumed
                                          * opener — `${` for the dual `$`
                                          * form (bare `$ident` emits no
                                          * open marker), interp_open for
                                          * explicit-close rules          */
  uint32_t           interp_close_kind;  /* marker (v=0) over the consumed
                                          * closer span (`}`)             */
  uint32_t           bare_ident_kind;    /* bare `$ident` becomes ONE token
                                          * of this kind spanning the `$`
                                          * through the identifier (v=0),
                                          * overriding any KEYWORD retag  */
} KavakStringRule;

/* ── Whitespace strategy: three orthogonal axes ─────────────────────── */

/* Axis 1 — offside-rule (Python / Haskell / F# light). The lexer tracks
 * the column of the first non-whitespace char on each line and emits
 * virtual KAVAK_TOK_INDENT / DEDENT / NEWLINE tokens. */

typedef enum {
  KAVAK_INDENT_SPACES = 0,   /* require spaces only, count chars        */
  KAVAK_INDENT_TABS   = 1,   /* require tabs only, count tabs           */
  KAVAK_INDENT_BOTH   = 2,   /* mix allowed; tabs expand at col 8       */
} KavakIndentUnit;

typedef struct KavakOffsideConfig {
  KavakIndentUnit unit;
  /* If 1, a line at the same column as the enclosing block dedents
   * (Haskell-ish). If 0, only strictly-shallower lines dedent
   * (Python-ish). */
  uint8_t         dedent_on_lower_or_equal;
} KavakOffsideConfig;

/* Axis 2 — Go-style auto-semicolon. After tokenizing line N, if the
 * last significant token matches `is_terminating`, emit a virtual `;` token
 * (kind = KAVAK_TOK_PUNCT, v = punct_id_for_semicolon) before the
 * newline. The callback receives the token, so languages can inspect
 * kind, v, and span. */

typedef struct KavakAutoSemicolon {
  /* Returns 1 if `token` ends a statement (identifier, literal, `)`,
   * `]`, `}`, certain keywords). */
  int (*is_terminating)(const KavakToken *token);
  /* Token kind to emit for the virtual semicolon. Languages set this
   * to whatever they use for `;` — e.g., a KAVAK_TOK_PUNCT with
   * v=PUNCT_SEMICOLON. */
  uint32_t emit_kind;
  uint32_t emit_v;
} KavakAutoSemicolon;

/* Axis 3 — parser-level automatic semicolon insertion. This is the
 * descriptor's job in its own parser (Section 8 helpers / the parse_source
 * hook), not a lexer feature; it is listed here only so the three whitespace
 * strategies sit together. The lexer itself handles axes 1 and 2. */

/* ── KavakLexerConfig — the aggregate ───────────────────────────────── */

struct KavakLexerConfig {
  /* Char predicates. NULL = use the kernel's ASCII defaults from Section 19.
   * Languages with Unicode identifiers can wire kavak_unicode_is_ident_start
   * / kavak_unicode_is_ident_cont here. */
  int (*is_ident_start)(uint32_t cp);
  int (*is_ident_cont) (uint32_t cp);

  /* Number literals. Zero flags = decimal-integer-only baseline. */
  KavakNumberStyle numbers;

  /* String rules — NULL/0 = no string literals in this language. */
  const KavakStringRule *strings;
  uint32_t               string_count;

  /* Comment styles — NULL/0 = no comments. */
  const KavakCommentRule *comments;
  uint32_t                comment_count;

  /* Operators — longest-match table. NULL/0 = no operators (the lexer
   * still emits KAVAK_TOK_PUNCT for the brackets / commas it
   * recognizes structurally; operators are the language-defined ones
   * here). */
  const KavakOperator *operators;
  uint32_t             operator_count;

  /* Keywords — looked up after each ident scan; matches retag the
   * token kind to KAVAK_TOK_KEYWORD with v = keyword id. */
  const KavakKeyword *keywords;
  uint32_t            keyword_count;

  /* Soft (contextual) keywords — keyword ids that should stay
   * KAVAK_TOK_IDENT (with v = id) instead of retagging to KAVAK_TOK_KEYWORD,
   * so they remain usable as ordinary names and the parser decides by
   * context. NULL/0 = every matched keyword is hard. Read the carried id on
   * the IDENT token with kavak_token_soft_keyword_id. */
  const uint32_t     *soft_keywords;
  uint32_t            soft_keyword_count;

  /* Whitespace — both NULL = free-form C-style. */
  const KavakOffsideConfig  *offside;
  const KavakAutoSemicolon  *auto_semi;
  /* Physical newline policy. 0 = KAVAK_NEWLINE_DEFAULT. Set
   * KAVAK_NEWLINE_UNICODE when a language treats NEL/LS/PS as source
   * line breaks. */
  uint32_t                   newline_flags;

  /* If 1, doc comments (KavakCommentRule.is_doc=1) emit
   * KAVAK_TOK_COMMENT tokens; otherwise comments are silently skipped. */
  int keep_doc_comments;

  /* If 1, ALL comment rules emit KAVAK_TOK_COMMENT tokens, doc or not.
   * Unterminated block comments still emit KAVAK_TOK_INVALID. */
  int keep_comments;

  /* If 1, emit a KAVAK_TOK_NEWLINE token for EVERY physical newline in
   * code context — ungated by line significance (blank and comment-only
   * lines included), and also inside `${...}` interpolation expressions.
   * No virtual newline is added at EOF. Newlines inside block comments
   * or string literals never emit. Independent of offside/auto_semi
   * (offside + emit_newlines inside interpolation is unsupported). */
  int emit_newlines;
};

/* ── kavak_lex driver ───────────────────────────────────────────────── */

/** Tokenizes `source` per `config` into `out_tokens`. Recoverable
 *  diagnostics (bad UTF-8, unterminated string, unknown character)
 *  go to `out_diags`; pass NULL to discard them. The token vec must
 *  be pre-initialized via kavak_token_vec_init.
 *
 *  The lexer always emits a final KAVAK_TOK_EOF, even on error.
 *  Bad bytes produce a KAVAK_TOK_INVALID token over the offending
 *  span and a diag, then the lexer advances and keeps going — a
 *  single bad byte doesn't desync the rest of the file.
 *
 *  Returns 0 on success, -1 on invalid arguments (NULL source/out_tokens,
 *  invalid config, oversize source, or len != 0 with NULL bytes) or a token-vec
 *  allocation failure. Recoverable errors do not trigger -1; check `out_diags`
 *  for them. */
int kavak_lex(const KavakSource      *source,
              const KavakLexerConfig *config,
              KavakTokenVec          *out_tokens,
              KavakDiagVec           *out_diags);

/* Validate and decode a hex/Unicode escape PAYLOAD — the part after `\x`,
 * `\u`, or `\U`. The kernel's KavakEscape model is single-char-after-backslash
 * (`\n`, `\t`, …) by design; variable-length hex escapes are a descriptor's
 * job, and this is the shared building block for a descriptor-owned string
 * scanner (or a post-lex pass) so each language need not re-implement the hex
 * grammar and range checks.
 *
 *   `s`      points at the first byte of the payload (right after the form
 *            character); `end` is one past the last readable byte.
 *   `form`   is 'x', 'u', or 'U'.
 *
 * Grammar per form:
 *   'x' — exactly 2 hex digits; result is a byte 0x00..0xFF.
 *   'u' — exactly 4 hex digits (\uHHHH) OR braces `{` 1..6 hex `}` (\u{H+});
 *         result must be a Unicode scalar value.
 *   'U' — exactly 8 hex digits; result must be a Unicode scalar value.
 * "Unicode scalar value" excludes > U+10FFFF and the surrogate range
 * U+D800..U+DFFF.
 *
 * Returns the number of bytes consumed FROM `s` (2/4/8, or `{...}` length for
 * the braced form), writing the codepoint to *out_cp when non-NULL. Returns 0
 * — consuming nothing — if the escape is malformed (bad/short hex, out of
 * range, surrogate, unclosed brace) or `form` is unrecognized. */
size_t kavak_scan_hex_escape(const char *s, const char *end, char form,
                             uint32_t *out_cp);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  8. Parser toolkit — Pratt + recursive-descent helpers                   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Opaque parser over a token vec. The parser does not own the
 * source, token array, arena, or diagnostic vec; callers keep them alive
 * for the parser lifetime. AST nodes are allocated from `arena` and bulk-
 * freed with it.
 *
 * Style: table-driven Pratt by default. Languages preferring tier-functions
 * can write their own expression parser using the RD helpers and bypass
 * kavak_parse_expression entirely. */

/* Optional Pratt extension hooks, consulted only AFTER the kernel's built-in
 * handling so they never override it.
 *
 *   parse_primary  contributes expression-position atoms the built-in set lacks
 *                  (if / when / lambda / match …). Called when no built-in atom
 *                  matches; return a node having consumed its tokens, or NULL
 *                  (consuming nothing) to fall through to "expected expression".
 *   parse_postfix  contributes trailing forms the operator table cannot express
 *                  (call (args), index [..]). Tried at the top of each Pratt
 *                  turn so it binds tighter than any operator — exactly like a
 *                  real call/index. Return a new node having consumed tokens,
 *                  or NULL (consuming nothing) when it does not apply. */
typedef KavakASTNode *(*KavakParsePrimaryFn)(KavakParser *parser);
typedef KavakASTNode *(*KavakParsePostfixFn)(KavakParser *parser, KavakASTNode *lhs);

struct KavakParserConfig {
  const KavakOperator *operators;      /* Pratt operator table */
  uint32_t             operator_count;
  /* Optional; NULL => kernel built-ins only. Appended for ABI stability. */
  KavakParsePrimaryFn  parse_primary;
  KavakParsePostfixFn  parse_postfix;
};

struct KavakParserCheckpoint {
  uint32_t pos;                        /* token cursor */
  uint32_t diag_count;                 /* lets rewind drop speculative diags */
};

KavakParser *kavak_parser_new(const KavakSource       *source,
                              const KavakToken        *tokens,
                              uint32_t                 token_count,
                              KavakArena              *arena,
                              KavakDiagVec            *diags,
                              const KavakParserConfig *config);
void         kavak_parser_free  (KavakParser *parser);

const KavakToken *kavak_parser_peek     (const KavakParser *parser);
/* Non-destructive lookahead: the token `offset` positions ahead of the cursor
 * (offset 0 == kavak_parser_peek). Past the end it clamps to the EOF/last
 * token, so a bounded scan never reads out of range or mutates position — the
 * basis for speculative disambiguation (generic-args vs comparison, etc.). */
const KavakToken *kavak_parser_peek_at  (const KavakParser *parser,
                                         uint32_t offset);
const KavakToken *kavak_parser_previous (const KavakParser *parser);
uint32_t          kavak_parser_pos      (const KavakParser *parser);
int               kavak_parser_at_end   (const KavakParser *parser);
/* Forward-progress guard for hand-written descent loops: 1 if the cursor has
 * advanced past `prev_pos`, 0 if not. Capture kavak_parser_pos() before an
 * iteration's work and break when this returns 0 to make stalls impossible. */
int               kavak_parser_progressed(const KavakParser *parser,
                                          uint32_t prev_pos);

int               kavak_parser_check    (const KavakParser *parser,
                                          uint32_t kind);
int               kavak_parser_check_v  (const KavakParser *parser,
                                          uint32_t kind, uint32_t v);
int               kavak_parser_check_text(const KavakParser *parser,
                                           uint32_t kind, const char *text);
const KavakToken *kavak_parser_eat      (KavakParser *parser, uint32_t kind);
const KavakToken *kavak_parser_eat_v    (KavakParser *parser,
                                          uint32_t kind, uint32_t v);
const KavakToken *kavak_parser_eat_text (KavakParser *parser,
                                          uint32_t kind, const char *text);
const KavakToken *kavak_parser_expect   (KavakParser *parser,
                                          uint32_t kind, const char *message);
const KavakToken *kavak_parser_expect_text(KavakParser *parser,
                                            uint32_t kind, const char *text,
                                            const char *message);

/* Skips tokens until one matches a recovery target, staying bracket-depth aware
 * (nested (), [], {} are skipped over) and never stopping on an OPENING bracket.
 * Kind-only form: */
void              kavak_parser_recover_to(KavakParser *parser,
                                           const uint32_t *kinds,
                                           uint32_t kind_count);

/* A recovery target: match by token kind, and — when match_v is non-zero — also
 * by the token's value `v`. This lets a grammar recover to a SPECIFIC
 * punctuation (e.g. {KAVAK_TOK_PUNCT, ';', 1}) instead of any punctuation,
 * which kind-only recovery cannot express since ';' '}' ')' share
 * KAVAK_TOK_PUNCT. */
typedef struct KavakRecoveryTarget {
  uint32_t kind;
  uint32_t v;        /* token value to match when match_v != 0 */
  uint8_t  match_v;  /* 0: match kind only; non-0: match kind AND v */
} KavakRecoveryTarget;

/* Value-aware form of kavak_parser_recover_to. Same bracket-depth and
 * opening-bracket rules; stops at the first depth-0 token matching any target. */
void              kavak_parser_recover_to_targets(KavakParser *parser,
                                                  const KavakRecoveryTarget *targets,
                                                  uint32_t count);
KavakASTNode     *kavak_parser_make_node (KavakParser *parser, uint32_t kind);
void              kavak_parser_finish_node(KavakParser *parser,
                                            KavakASTNode *node);

KavakParserCheckpoint kavak_parser_checkpoint(const KavakParser *parser);
void                  kavak_parser_rewind    (KavakParser *parser,
                                              KavakParserCheckpoint checkpoint);

KavakASTNode *kavak_parse_expression(KavakParser *parser, uint16_t min_prec);

/* Parse "open expr (sep expr)* close", appending each parsed expression as a
 * child of `node`; the delimiters are matched as single-character KAVAK_TOK_PUNCT
 * tokens. Tolerates an empty list and a trailing separator, guards against
 * non-progress hangs, and reports a diagnostic on a missing close. Returns the
 * item count. Does nothing and returns 0 if not positioned at `open`. */
uint32_t      kavak_parser_parse_delimited(KavakParser *parser, KavakASTNode *node,
                                           char open, char sep, char close);

/* Postfix-call builder for parse_postfix hooks: if positioned at `open`, wraps
 * `callee` in a KAVAK_AST_CALL whose first child is the callee and whose
 * remaining children are the parsed arguments. Returns NULL (consuming nothing)
 * when not at `open`, so it slots straight in as a parse_postfix result. */
KavakASTNode *kavak_parser_parse_call    (KavakParser *parser, KavakASTNode *callee,
                                          char open, char sep, char close);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  9. AST — the syntax tree                                             │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * KavakASTNode keeps the common traversal fields directly in the node:
 * kind, modifiers, span, inferred type, first/last child, next sibling,
 * and a kind-specific payload union. Parent pointers are intentionally
 * absent; visitors that need parent context pass it down during traversal.
 *
 * AST kinds 0..255 are reserved for the kernel (AST_INVALID, AST_ROOT,
 * AST_BLOCK, AST_ERROR, …). Languages extend from KAVAK_AST_USER_BASE. */

#define KAVAK_AST_USER_BASE  256u
#define KAVAK_AST_FLAG_ERROR (1u << 31)  /* in modifiers — partial-parse marker */

typedef enum {
  KAVAK_AST_INVALID      = 0,
  KAVAK_AST_ROOT         = 1,
  KAVAK_AST_ERROR        = 2,
  KAVAK_AST_IDENT        = 3,
  KAVAK_AST_LITERAL      = 4,
  KAVAK_AST_UNARY        = 5,
  KAVAK_AST_BINARY       = 6,
  KAVAK_AST_GROUP        = 7,
  KAVAK_AST_SELECTOR     = 8,
  KAVAK_AST_CALL         = 9,
  KAVAK_AST_BLOCK        = 10,
  KAVAK_AST_IF           = 11,
  KAVAK_AST_FOR          = 12,
  KAVAK_AST_RETURN       = 13,
  KAVAK_AST_VAR          = 14,
  KAVAK_AST_CONST        = 15,
  KAVAK_AST_FUNC         = 16,
  KAVAK_AST_PARAM        = 17,
  KAVAK_AST_EXPR_STMT    = 18,
  KAVAK_AST_IMPORT       = 19,
  KAVAK_AST_TYPE_DECL    = 20,
  KAVAK_AST_STRUCT       = 21,
  KAVAK_AST_STRUCT_FIELD = 22,
} KavakASTKind;

typedef enum {
  KAVAK_LIT_INT    = 1,
  KAVAK_LIT_FLOAT  = 2,
  KAVAK_LIT_STRING = 3,
  KAVAK_LIT_CHAR   = 4,
} KavakLiteralKind;

union KavakASTPayload {
  struct {
    uint32_t first_token;
    uint32_t last_token;
  } range;
  struct {
    uint32_t token_index;
    uint32_t literal_kind;
  } literal;
  struct {
    uint32_t token_index;
  } ident;
  struct {
    uint32_t op_token;
    uint32_t op_id;
  } op;
  struct {
    uint64_t a;
    uint64_t b;
  } user;
};

/* Field order matters here: kind + modifiers (both u32) sit consecutively
 * up front so the 8-byte alignment of the trailing pointers doesn't force a
 * 4-byte pad. The seven fields before `payload` are 4+4+8+8+8+8+8 = 48 bytes
 * on LP64 with no padding, so `payload` lands at offset 48; the union adds 16,
 * for sizeof == 64. Reordering this group (e.g., span before modifiers) injects
 * a pad and pushes the prefix to 56 — see test_ast.c, which gates both the
 * offset and the total size. */
struct KavakASTNode {
  uint32_t       kind;          /* AST_* or KAVAK_AST_USER_BASE+N       */
  uint32_t       modifiers;     /* language-defined bitmask;
                                 * top bit = KAVAK_AST_FLAG_ERROR        */
  KavakSpan      span;          /* token-range or byte-range             */
  KavakTypeInfo *type;          /* set by sema; NULL pre-resolution      */
  KavakASTNode  *first_child;
  KavakASTNode  *last_child;    /* O(1) append during parse              */
  KavakASTNode  *next_sibling;
  KavakASTPayload payload;       /* kind-specific payload; header above
                                  * remains the first 48 bytes on LP64. */
};

void kavak_ast_append_child(KavakASTNode *parent, KavakASTNode *child);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  10. Types — KavakTypeInfo, builtins                                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * TypeInfo is extensible the same way ASTNode is: kernel kinds occupy
 * 0..255, languages extend from KAVAK_TY_USER_BASE upward. Builtin
 * primitive types are per-arena singletons; constructed types live in
 * the TypeArena from Section 15 and borrow names / decl pointers from the
 * descriptor or AST that allocated them. */

#define KAVAK_TY_USER_BASE      256u
#define KAVAK_TY_BUILTIN_COUNT   17u

typedef enum {
  KAVAK_TY_INVALID = 0,
  KAVAK_TY_VOID    = 1,
  KAVAK_TY_NEVER   = 2,
  KAVAK_TY_ANY     = 3,
  KAVAK_TY_BYTE    = 4,
  KAVAK_TY_UBYTE   = 5,
  KAVAK_TY_SHORT   = 6,
  KAVAK_TY_USHORT  = 7,
  KAVAK_TY_INT     = 8,
  KAVAK_TY_UINT    = 9,
  KAVAK_TY_LONG    = 10,
  KAVAK_TY_ULONG   = 11,
  KAVAK_TY_FLOAT   = 12,
  KAVAK_TY_DOUBLE  = 13,
  KAVAK_TY_BOOL    = 14,
  KAVAK_TY_CHAR    = 15,
  KAVAK_TY_STRING  = 16,

  KAVAK_TY_NULLABLE = 32,
  KAVAK_TY_FUNCTION = 33,
  KAVAK_TY_NAMED    = 34,
  KAVAK_TY_PARAM    = 35,
  KAVAK_TY_RECORD   = 36,  /* structural record / tuple type */
} KavakTypeKind;

#define KAVAK_TY_FUNC_SUSPEND (1u << 0)
#define KAVAK_TY_FUNC_ASYNC   (1u << 1)

struct KavakRecordField {
  const char    *name;   /* borrowed, NUL-terminated */
  KavakTypeInfo *type;
};

struct KavakTypeInfo {
  uint32_t kind;         /* KAVAK_TY_* or KAVAK_TY_USER_BASE+N */
  uint32_t flags;        /* function flags; user bits for user kinds */
  union {
    struct {
      KavakTypeInfo *inner;
    } nullable;
    struct {
      KavakTypeInfo  *receiver;      /* NULL for free functions */
      KavakTypeInfo **params;        /* arena-owned array; may be NULL */
      uint32_t        param_count;
      KavakTypeInfo  *ret;
    } function;
    struct {
      const char       *name;        /* borrowed */
      KavakTypeInfo   **args;        /* arena-owned array; may be NULL */
      uint32_t          arg_count;
      const KavakASTNode *decl;      /* borrowed, optional */
      KavakTypeInfo    *underlying;  /* underlying representation, e.g. record for struct or aliased primitive type */
    } named;
    struct {
      const char       *name;        /* borrowed */
      uint32_t          index;       /* generic param index at decl site */
      const KavakASTNode *decl;      /* borrowed, optional */
    } param;
    struct {
      KavakTypeInfo    **positional; /* arena-owned array; may be NULL */
      uint32_t           positional_count;
      KavakRecordField  *named;      /* arena-owned array; may be NULL */
      uint32_t           named_count;
    } record;
    struct {
      uint64_t a;
      uint64_t b;
    } user;
  } payload;
};

struct KavakTypeSubst {
  const KavakTypeInfo *param;       /* usually KAVAK_TY_PARAM pointer */
  KavakTypeInfo       *replacement;
};

KavakTypeInfo *kavak_ty_alloc       (KavakTypeArena *arena, uint32_t kind);
KavakTypeInfo *kavak_ty_builtin     (KavakTypeArena *arena, uint32_t kind);
KavakTypeInfo *kavak_ty_nullable    (KavakTypeArena *arena,
                                     KavakTypeInfo *inner);
KavakTypeInfo *kavak_ty_function    (KavakTypeArena *arena,
                                     KavakTypeInfo *receiver,
                                     KavakTypeInfo *const *params,
                                     uint32_t param_count,
                                     KavakTypeInfo *ret,
                                     uint32_t flags);
KavakTypeInfo *kavak_ty_named       (KavakTypeArena *arena,
                                     const char *name,
                                     KavakTypeInfo *const *args,
                                     uint32_t arg_count,
                                     const KavakASTNode *decl);
KavakTypeInfo *kavak_ty_param       (KavakTypeArena *arena,
                                     const char *name,
                                     uint32_t index,
                                     const KavakASTNode *decl);
KavakTypeInfo *kavak_ty_record      (KavakTypeArena *arena,
                                     KavakTypeInfo *const *positional,
                                     uint32_t positional_count,
                                     const KavakRecordField *named,
                                     uint32_t named_count);

/* Two notions of type equality, both cycle-guarded.
 *
 * kavak_ty_equal_nominal — SHALLOW, identity-shaped equality: it compares the
 * naming/shape "skeleton" of a type, deliberately NOT its component types. Use
 * it to ask "are these the same named/shaped type?", not "are they structurally
 * identical?". Per kind it compares ONLY:
 *   - builtin : the kind (Int == Int).
 *   - named   : the name and the type-argument COUNT — NOT the arguments
 *               themselves, and NOT the decl pointer (List<Int> == List<String>).
 *   - function: flags, whether a receiver is present (not its type), and the
 *               parameter COUNT — NOT the parameter types and NOT the return type.
 *   - record  : the positional and named-field COUNTS and the named-field NAMES
 *               — NOT any field's type (neither positional nor named).
 *   - param   : the index and name.
 *   - nullable: recurses into the inner type (which is then itself compared
 *               nominally).
 * Because component types are ignored, distinct types can compare equal here;
 * that is intended (it is the cheap "same shape" check), not a bug. The name is
 * "nominal" in the by-name/by-identity sense, NOT "fully structural".
 *
 * kavak_ty_equal_deep — FULL structural equality: compares everything
 * equal_nominal skips (every type argument, parameter type, return type, and
 * field type, recursively). Use this when you need real type identity. */
int            kavak_ty_equal_nominal(const KavakTypeInfo *a,
                                      const KavakTypeInfo *b);
int            kavak_ty_equal_deep   (const KavakTypeInfo *a,
                                      const KavakTypeInfo *b);
KavakTypeInfo *kavak_ty_substitute   (KavakTypeArena *arena,
                                      const KavakTypeInfo *type,
                                      const KavakTypeSubst *subst,
                                      uint32_t subst_count);
size_t         kavak_ty_to_string    (const KavakTypeInfo *type,
                                      char *buf,
                                      size_t buf_len);

/* Type relations: nullability, assignability, join (LUB).
 *
 * Generic, language-agnostic type-graph operations. The kernel decides the
 * structural and universal cases from its own builtin vocabulary — identity,
 * the bottom type (KAVAK_TY_NEVER), the non-null top type (KAVAK_TY_ANY), and
 * nullability (KAVAK_TY_NULLABLE). Nominal and numeric policy is delegated to
 * the optional descriptor hooks in Section 13 (`is_subtype`, `numeric_rank`,
 * `type_join`). With no hooks supplied these degrade to sound structural-only
 * defaults, so a language opts in to exactly the relations it needs.
 *
 * Kernel convention: KAVAK_TY_ANY is the non-null top — `T?` is assignable to
 * `Any?` but not to `Any`. A language that never constructs KAVAK_TY_NULLABLE
 * nodes is unaffected by any nullability rule. */

int                  kavak_ty_is_nullable    (const KavakTypeInfo *type);
const KavakTypeInfo *kavak_ty_unwrap_nullable(const KavakTypeInfo *type);

/* Is `sub` assignable to `super`? Structural cases are decided by the kernel;
 * nominal/user relations are deferred to lang->is_subtype (NULL => no nominal
 * relation beyond identity). `lang` may be NULL for a structural-only check. */
int            kavak_ty_assignable   (const KavakTypeInfo *sub,
                                      const KavakTypeInfo *super,
                                      const KavakLanguage *lang);

/* The wider of two numerically-ranked builtin types per lang->numeric_rank,
 * or NULL if `lang` supplies no ranks or either type is not numeric. Returns a
 * borrowed builtin pointer; no allocation. */
const KavakTypeInfo *kavak_ty_numeric_common(const KavakTypeInfo *a,
                                             const KavakTypeInfo *b,
                                             const KavakLanguage *lang);

/* Least-upper-bound (branch join) of two types, for if/when/ternary merge.
 * Kernel handles NEVER absorption, identity, and nullability union; numeric
 * widening and nominal joins come from lang's hooks; the universal fallback is
 * KAVAK_TY_ANY. Needs `arena` to allocate the nullable wrap / Any fallback. */
KavakTypeInfo *kavak_ty_join         (KavakTypeArena *arena,
                                      const KavakTypeInfo *a,
                                      const KavakTypeInfo *b,
                                      const KavakLanguage *lang);

/* Structural unification: collect the substitutions that make `pattern` (a type
 * containing KAVAK_TY_PARAM leaves) match `concrete`. Lenient by design —
 * mismatched shapes are skipped rather than failing, the first binding for a
 * given param wins, and there is no occurs-check or variance. New bindings are
 * appended at subst[*subst_count .. subst_cap) and *subst_count is advanced.
 * Returns the number of new bindings added. Pair with kavak_ty_substitute to
 * instantiate a generic signature's return type at a call site. */
uint32_t       kavak_ty_unify        (const KavakTypeInfo *pattern,
                                      const KavakTypeInfo *concrete,
                                      KavakTypeSubst *subst,
                                      uint32_t subst_cap,
                                      uint32_t *subst_count);

/* Outcome of kavak_ty_unify_strict. */
typedef enum {
  KAVAK_UNIFY_OK = 0,          /* fully unified; bindings appended            */
  KAVAK_UNIFY_MISMATCH,        /* shapes/arities/heads don't line up          */
  KAVAK_UNIFY_CONFLICT,        /* a param was forced to two unequal types     */
  KAVAK_UNIFY_OCCURS,          /* a param occurs inside its own binding        */
  KAVAK_UNIFY_CAP_EXCEEDED,    /* ran out of subst slots / recursion budget   */
} KavakUnifyResult;

/* Strict counterpart of kavak_ty_unify, for overload/generic-call inference
 * that must REJECT bad matches instead of silently absorbing them. Unlike the
 * lenient version it: fails on mismatched shapes/arities (and on differing
 * named heads, e.g. List<T> vs Map<Int>); detects when one param is forced to
 * two structurally-unequal types (KAVAK_UNIFY_CONFLICT — e.g. unifying the
 * pattern (T, T) with (Int, String)); and performs an occurs-check. New
 * bindings are appended at subst[*subst_count .. subst_cap); on any non-OK
 * result *subst_count may be left partially advanced, so the caller should
 * discard the bindings unless KAVAK_UNIFY_OK is returned. Returns
 * KAVAK_UNIFY_MISMATCH for NULL subst/subst_count. */
KavakUnifyResult kavak_ty_unify_strict(const KavakTypeInfo *pattern,
                                       const KavakTypeInfo *concrete,
                                       KavakTypeSubst *subst,
                                       uint32_t subst_cap,
                                       uint32_t *subst_count);

/* Nominal conformance over a type hierarchy: 1 if `sub` is `super` or reaches
 * it through direct-supertype edges. The kernel does the transitive, cycle-
 * guarded walk; the language supplies only a node's DIRECT supertypes via
 * lang->supertypes (Section 13). Returns identity-only (no walk) when that hook
 * is unset. kavak_ty_assignable falls back to this when no is_subtype is set. */
int            kavak_ty_conforms     (const KavakTypeInfo *sub,
                                      const KavakTypeInfo *super,
                                      const KavakLanguage *lang);

/* Find a member named `name` on `type` or any of its supertypes (walk up the
 * chain via lang->supertypes, asking lang->find_own_member at each node).
 * Returns the member's type, or NULL if unresolved / hooks unset. name_len 0
 * means strlen(name). */
const KavakTypeInfo *kavak_ty_find_member(const KavakTypeInfo *type,
                                          const char *name, uint32_t name_len,
                                          const KavakLanguage *lang);

/* Closed-set exhaustiveness: 1 if every variant of `sealed` (from
 * lang->variants) is covered by some entry in covered[0..count) — where a
 * variant is "covered" when it conforms to a covered type (so matching a base
 * covers its cases). On a gap, sets *missing (if non-NULL) to the first
 * uncovered variant and returns 0. Returns 0 when no variants hook is set. */
int            kavak_ty_exhaustive   (const KavakTypeInfo *sealed,
                                      const KavakTypeInfo *const *covered,
                                      uint32_t count,
                                      const KavakTypeInfo **missing,
                                      const KavakLanguage *lang);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  11. Sema toolkit — scope, narrowing, name resolution                   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Opaque resolver context plus explicit scope stack. Flow-sensitive
 * narrowing reuses lexical scopes via shadowing: bind a narrowed symbol
 * in an inner scope and normal lookup returns the narrowed type until the
 * scope is popped. */

#define KAVAK_SYM_VALUE    1u
#define KAVAK_SYM_TYPE     2u
#define KAVAK_SYM_FUNCTION 3u

#define KAVAK_SYM_FLAG_NARROWED (1u << 0)

struct KavakSymbol {
  const char         *name;      /* borrowed; may point into source */
  uint32_t            name_len;  /* 0 means strlen(name) */
  uint32_t            kind;      /* KAVAK_SYM_* or language-defined */
  uint32_t            flags;     /* KAVAK_SYM_FLAG_* */
  const KavakASTNode *decl;      /* borrowed, optional */
  KavakTypeInfo      *type;
  void               *user;
};

struct KavakNarrowing {
  KavakSymbol symbol;            /* symbol.name + symbol.type are required */
};

struct KavakScope {
  KavakScope *parent;
  KavakSymbol *symbols;
  uint32_t count;
  uint32_t cap;
  uint32_t flags;
};

typedef int (*KavakSemaBodyFn)(KavakSema *sema, void *user);

KavakSema *kavak_sema_new(KavakSession       *session,
                          const KavakSource  *source,
                          const KavakToken   *tokens,
                          uint32_t            token_count,
                          KavakASTNode       *root,
                          KavakDiagVec       *diags);
void        kavak_sema_free  (KavakSema *sema);

KavakScope     *kavak_sema_scope     (const KavakSema *sema);
KavakTypeArena *kavak_sema_type_arena(KavakSema *sema);
const KavakSource *kavak_sema_source (const KavakSema *sema);
const KavakToken  *kavak_sema_tokens (const KavakSema *sema);
uint32_t           kavak_sema_token_count(const KavakSema *sema);
KavakASTNode      *kavak_sema_root   (const KavakSema *sema);

int  kavak_sema_push_scope     (KavakSema *sema);
void kavak_sema_pop_scope      (KavakSema *sema);
int  kavak_sema_bind           (KavakSema *sema, KavakSymbol symbol);
int  kavak_sema_bind_narrowing (KavakSema *sema,
                                const char *name,
                                uint32_t name_len,
                                KavakTypeInfo *narrowed_type);
const KavakSymbol *kavak_sema_lookup(const KavakSema *sema,
                                     const char *name,
                                     uint32_t name_len);

/* Visit every symbol named `name` across the scope chain, in lookup order
 * (newest scope first, newest binding first within a scope). The visitor
 * returning non-zero stops the walk early. Returns the count visited. Where
 * kavak_sema_lookup returns only the newest match, this surfaces every
 * shadowed / overloaded binding — the basis for overload resolution, since
 * scopes keep duplicate names rather than collapsing them. */
typedef int (*KavakSymbolVisitFn)(const KavakSymbol *symbol, void *user);
uint32_t kavak_sema_lookup_all(const KavakSema *sema,
                               const char *name, uint32_t name_len,
                               KavakSymbolVisitFn visit, void *user);

/* Score a candidate against a call's argument types: higher is better, < 0
 * rejects the candidate. Scoring (exact vs widened vs subtype match) is the
 * language's policy; the kernel only iterates candidates and keeps the best. */
typedef int (*KavakScoreOverloadFn)(const KavakSymbol *candidate,
                                    const KavakTypeInfo *const *arg_types,
                                    uint32_t arg_count, void *user);

/* Pick the best-scoring symbol named `name` for the given argument types using
 * `score`. Returns NULL if no scorer is supplied or no candidate scores >= 0.
 * On ties the first candidate in lookup order (newest) wins. */
const KavakSymbol *kavak_sema_pick_overload(const KavakSema *sema,
                                            const char *name, uint32_t name_len,
                                            KavakScoreOverloadFn score, void *user,
                                            const KavakTypeInfo *const *arg_types,
                                            uint32_t arg_count);

int kavak_sema_apply_narrowings(KavakSema *sema,
                                const KavakNarrowing *narrowings,
                                uint32_t narrowing_count);
int kavak_sema_with_narrowings (KavakSema *sema,
                                const KavakNarrowing *narrowings,
                                uint32_t narrowing_count,
                                KavakSemaBodyFn body,
                                void *user);
int kavak_sema_with_branch_narrowing(KavakSema *sema,
                                     KavakASTNode *condition,
                                     int branch,
                                     KavakSemaBodyFn body,
                                     void *user);
int kavak_sema_apply_early_exit_narrowing(KavakSema *sema,
                                          KavakASTNode *condition,
                                          int branch,
                                          const KavakTypeInfo *terminator_type);

int  kavak_sema_resolve_names(KavakSema *sema, KavakASTNode *root);
void kavak_sema_diag         (KavakSema *sema,
                              KavakSpan span,
                              const char *message);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  12. Session — runtime state holder                                    │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * The descriptor (KavakLanguage, Section 13) is link-time `const`; mutable
 * state per analysis lives in KavakSession. One session can run many
 * kavak_analyze calls — natural for multi-file programs that share
 * cross-file name resolution and type interning.
 *
 *   KavakSession *s = kavak_session_new(&MY_LANGUAGE);
 *   ...
 *   kavak_session_free(s);
 */

/** Allocates a session bound to `lang`. Returns NULL on out-of-memory or
 *  if `lang` is NULL. The descriptor pointer is stored as-is — the
 *  caller keeps `lang` alive (it's a static const in practice). */
KavakSession *kavak_session_new (const KavakLanguage *lang);

/** Frees the session and all per-session arena allocations. After this
 *  call, every KavakResult produced from `session` is invalid — free
 *  results before freeing the session that owns them. NULL is a no-op. */
void          kavak_session_free   (KavakSession *session);

/** Returns the per-session type arena. The pointer stays valid until
 *  kavak_session_free. NULL input returns NULL. */
KavakTypeArena *kavak_session_type_arena(KavakSession *session);

/** Reclaims every type interned in the session's type arena while keeping the
 *  session alive for further analyses. The per-session type arena otherwise
 *  grows for the session's whole lifetime (results free their own AST arena but
 *  never the shared type arena), so a long-lived session — e.g. an IDE running
 *  many analyses — would accumulate types without bound; this is the release
 *  valve. CONTRACT: every KavakTypeInfo* produced before the call (including
 *  those reachable through prior KavakResult roots) is invalidated, so free the
 *  results that reference them first. Returns 0, or -1 on NULL input or OOM
 *  (the arena's aux store could not be rebuilt). */
int kavak_session_reset_types(KavakSession *session);

/** Returns the descriptor pointer passed to kavak_session_new. */
const KavakLanguage *kavak_session_language(const KavakSession *session);

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  13. KavakLanguage — the descriptor                                      │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Minimal descriptor surface needed for `kavak_analyze`: lex, parse,
 * optional pre/post sema hooks, a per-node resolver hook, and the
 * narrowing hook from Section 11. Language frontends fill this out with real
 * grammar and language-specific resolver logic. */

typedef KavakASTNode *(*KavakParseSourceFn)(KavakParser *parser);
typedef void (*KavakSemaHookFn)(KavakSema *sema, KavakASTNode *root);
typedef int  (*KavakResolveNodeFn)(KavakSema *sema, KavakASTNode *node);
typedef uint32_t (*KavakNarrowForBranchFn)(KavakSema *sema,
                                           KavakASTNode *condition,
                                           int branch,
                                           KavakNarrowing *out,
                                           uint32_t out_cap);

/* Post-order (bottom-up) type synthesis: called during the resolve walk after
 * a node's children are resolved, so it can compute the node's type from theirs
 * (operand types -> result, branch arms -> join, etc.). Return the synthesized
 * type (the kernel assigns it to node->type) or NULL to leave node->type as-is.
 * The resolve_node / resolve_node_post pair is also the natural place to push a
 * lexical scope before a node's children and pop it after. */
typedef KavakTypeInfo *(*KavakSynthTypeFn)(KavakSema *sema, KavakASTNode *node);

/* Optional debug-dump hooks (Section 5). `ast_kind_name` maps an AST kind to a
 * stable human name ("Binary", or a user kind's name) for readable dumps —
 * return NULL for kinds it does not name. `ast_payload` writes a node's
 * kind-specific payload (operator spelling, literal text) as a plain string
 * into buf, returning its length like kavak_ty_to_string — return 0 for none.
 * `source` is the analyzed source (may be NULL), so the hook can recover a
 * node's spelling via kavak_source_slice(source, node->span, ...). */
typedef const char *(*KavakAstKindNameFn)(uint32_t kind);
typedef size_t (*KavakAstPayloadFn)(const KavakASTNode *node,
                                    const KavakSource *source,
                                    char *buf, size_t buf_len);

/* Optional type-hierarchy hooks (Section 10 conformance helpers). The language
 * supplies only LOCAL edges of its nominal graph; the kernel does the walks.
 *   supertypes      — a type's DIRECT supertypes (one level). Writes up to cap
 *                     into out[], returns the total count.
 *   find_own_member — the type of a member `type` declares ITSELF (not
 *                     inherited), or NULL. The kernel inherits up the chain.
 *   variants        — the cases of a closed/sealed type (its direct subtypes),
 *                     for exhaustiveness. Writes up to cap, returns the total. */
typedef uint32_t (*KavakSupertypesFn)(const KavakLanguage *lang,
                                      const KavakTypeInfo *type,
                                      const KavakTypeInfo **out, uint32_t cap);
typedef const KavakTypeInfo *(*KavakOwnMemberFn)(const KavakLanguage *lang,
                                                 const KavakTypeInfo *type,
                                                 const char *name,
                                                 uint32_t name_len);
typedef uint32_t (*KavakVariantsFn)(const KavakLanguage *lang,
                                    const KavakTypeInfo *type,
                                    const KavakTypeInfo **out, uint32_t cap);

/* Optional type-relation policy hooks consumed by the Section 10 helpers
 * (kavak_ty_assignable / kavak_ty_numeric_common / kavak_ty_join). All are
 * optional: a NULL hook means the kernel uses its structural-only default for
 * that relation. They express the policy a generic kernel cannot know — the
 * language's nominal hierarchy and numeric promotion ladder — while the
 * traversal stays in the kernel. */

/* Nominal/user subtype relation: 1 if `sub` is a subtype of `super`. Consulted
 * only for cases the structural kernel rules do not already decide (i.e. two
 * distinct non-null types). The hierarchy is read from the types themselves
 * (e.g. named.decl / named.underlying), so this stays a pure relation. */
typedef int (*KavakIsSubtypeFn)(const KavakLanguage *lang,
                                const KavakTypeInfo *sub,
                                const KavakTypeInfo *super);

/* Widening rank of a numeric type `kind` (higher = wider, e.g. Int < Long <
 * Double). Return 0 for any kind the language does not treat as numeric. */
typedef uint32_t (*KavakNumericRankFn)(const KavakLanguage *lang,
                                       uint32_t kind);

/* Language join of two distinct non-null, non-numeric types (consulted after
 * the kernel's structural rules and before the KAVAK_TY_ANY fallback). Return
 * the common supertype, or NULL to accept the Any fallback. */
typedef KavakTypeInfo *(*KavakTypeJoinFn)(KavakTypeArena *arena,
                                          const KavakTypeInfo *a,
                                          const KavakTypeInfo *b,
                                          const KavakLanguage *lang);

struct KavakLanguage {
  const char *name;
  const char *file_extension;
  uint32_t    version;

  KavakLexerConfig lexer;
  KavakParserConfig parser;

  KavakParseSourceFn      parse_source;
  KavakSemaHookFn         pre_resolve;
  KavakResolveNodeFn      resolve_node;
  KavakNarrowForBranchFn  narrow_for_branch;
  KavakSemaHookFn         post_sema;

  /* Type-relation policy hooks (Section 10). All optional; NULL => kernel
   * structural defaults. Placed at the end of the descriptor so existing
   * designated initializers keep working unchanged. */
  KavakIsSubtypeFn        is_subtype;
  KavakNumericRankFn      numeric_rank;
  KavakTypeJoinFn         type_join;

  /* Two-phase resolution & bottom-up synthesis (Section 11 resolve walk).
   * Optional. `declare_node` runs in a first traversal over the whole tree
   * before resolution so a language can forward-register names (functions,
   * types, globals) and make resolution order-independent; bind via
   * kavak_sema_bind, return non-zero to flag the node. `resolve_node_post`
   * runs post-order during resolution (see KavakSynthTypeFn). */
  KavakResolveNodeFn      declare_node;
  KavakSynthTypeFn        resolve_node_post;

  /* Debug-dump hooks (Section 5). Optional; NULL => generic kind=N dumps. */
  KavakAstKindNameFn      ast_kind_name;
  KavakAstPayloadFn       ast_payload;

  /* Type-hierarchy hooks (Section 10 conformance helpers). All optional. */
  KavakSupertypesFn       supertypes;
  KavakOwnMemberFn        find_own_member;
  KavakVariantsFn         variants;
};


/* ════════════════════════════════════════════════════════════════════════════
 *
 *                          B A C K E N D   A B I
 *
 *   Sections 14-15. Read this only if you're writing a code generator
 *   that turns analyzed AST into IR / WASM / native. Stability:
 *   layout-level changes may happen on minor versions — pin exact kavak.
 *
 * ════════════════════════════════════════════════════════════════════════════ */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  14. Modifier bits — per-language ASTNode bitmask                       │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * KavakASTNode.modifiers is a 32-bit bitmask. The kernel claims the
 * top bit (KAVAK_AST_FLAG_ERROR, see Section 9). Language descriptors define
 * the rest.
 *
 * No kernel-level enum — each language ships its own MOD_* macros in
 * its descriptor header. */


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  15. Type arena — TypeArena allocator                                  │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Specialized arena for KavakTypeInfo. Type records are stored in fixed-size
 * chunks and bulk-freed with the session. Auxiliary arrays inside types
 * (function params, generic args, record fields) are copied into the owned
 * generic byte arena. */

struct KavakTypeArena {
  KavakTypeArenaChunk *head;
  KavakTypeArenaChunk *tail;
  KavakArena          *aux;
  KavakTypeInfo        builtins[KAVAK_TY_BUILTIN_COUNT];
};

void kavak_type_arena_init (KavakTypeArena *arena);
void kavak_type_arena_free (KavakTypeArena *arena);
/** Releases every interned type but keeps `arena` ready for reuse (chunks freed,
 *  aux store rebuilt, builtins re-seeded). Invalidates all prior KavakTypeInfo*.
 *  Returns 0, or -1 on NULL input or OOM. */
int  kavak_type_arena_reset(KavakTypeArena *arena);


/* ════════════════════════════════════════════════════════════════════════════
 *
 *                              P R I M I T I V E S
 *
 *   Sections 16-19. The substrate types — used by descriptor and backend
 *   audiences directly, seen by consumers only through the accessor
 *   surface in Section 4. Stable across minor versions; structs may grow
 *   at the end of the layout but never reorder existing fields.
 *
 * ════════════════════════════════════════════════════════════════════════════ */

/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  16. Diagnostics raw — KavakDiagVec, severity, format                   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Vec of { severity, message, span } records — every token, AST node,
 * or sema rule that finds something wrong pushes one. The formatter
 * renders `path:line:col: severity: msg\n` against a KavakSource. Rich
 * diagnostics (carets, fix-its, multi-span notes) can be layered above this.
 *
 * `KavakSeverity` is defined upfront with the substrate types; the
 * struct definitions and the API live below. */

struct KavakDiag {
  KavakSeverity severity;
  const char   *message;        /**< Caller-owned lifetime.        */
  KavakSpan     span;
};

struct KavakDiagVec {
  KavakDiag *items;
  uint32_t   count;
  uint32_t   cap;
};

void     kavak_diag_vec_init    (KavakDiagVec *vector);
void     kavak_diag_vec_free    (KavakDiagVec *vector);

/** Appends a copy of `diag`. Returns 0 on success, -1 on out-of-memory. The vec
 *  stores `diag.message` as-is (the pointer); caller keeps the string
 *  alive while the vec holds it. */
int      kavak_diag_vec_push    (KavakDiagVec *vector, KavakDiag diag);

/** Number of KAVAK_SEV_ERROR entries currently in the vec. */
uint32_t kavak_diag_error_count (const KavakDiagVec *vector);

/** Renders `filename:line:col: severity: message\n` to `buf`.
 *  snprintf-style: returns the number of bytes that would be written
 *  (excluding NUL). Truncation NUL-terminates within `buf_len`. `source`
 *  may be NULL — line / col render as 1 / 1 then.
 *
 *  Sizing mode: pass `buf == NULL && buf_len == 0` to compute the
 *  required length without writing — useful when the caller wants to
 *  size up an arena allocation before formatting. */
size_t   kavak_diag_format      (const KavakDiag *diag, const KavakSource *source,
                                 char *buf, size_t buf_len);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  17. Span — half-open byte range                                        │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Half-open [start, start+len). uint32_t fields cap a single span at
 * 4 GiB per source file. Most of the API is
 * `static inline` — span_make / from_to / end / is_empty / contains
 * are single ops the compiler folds. Only span_union has enough
 * branching to live as a real function.
 *
 * The struct itself is defined upfront with the substrate types
 * (see the forward-decl comment near the top); only the helpers
 * live here. */

/* Sentinel for "no span". Span helpers treat any len==0 range as empty. */
#define KAVAK_SPAN_NONE ((KavakSpan){ 0, 0 })

KAVAK_INLINE KavakSpan kavak_span_make    (uint32_t start, uint32_t len)      { return (KavakSpan){ start, len }; }
KAVAK_INLINE KavakSpan kavak_span_from_to (uint32_t start, uint32_t end_excl) { return (KavakSpan){ start, end_excl > start ? end_excl - start : 0 }; }
KAVAK_INLINE uint32_t  kavak_span_end     (KavakSpan span)                    { return UINT32_MAX - span.start < span.len ? UINT32_MAX : span.start + span.len; }
KAVAK_INLINE int       kavak_span_is_empty(KavakSpan span)                    { return span.len == 0; }
KAVAK_INLINE int       kavak_span_contains(KavakSpan span, uint32_t pos)      { return pos >= span.start && pos < kavak_span_end(span); }

/** Smallest span covering both inputs. Empty span absorbed —
 *  union(empty, x) == x. */
KavakSpan kavak_span_union(KavakSpan a, KavakSpan b);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  18. Arena — generic byte bump allocator                                │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Growable-chunk bump allocator. Allocations are calloc-zeroed and
 * 8-byte aligned by default; alloc_aligned takes caller-specified
 * alignment up to whatever fits. Allocations larger than chunk_size
 * get a dedicated chunk sized to fit; oversize asks reserve extra
 * room when alignment > 8 so data[0] mis-alignment in fresh chunks
 * doesn't leak into the user's pointer. */

struct KavakArena {
  KavakArenaChunk *head;
  KavakArenaChunk *tail;
  size_t           chunk_size;     /* default chunk capacity, bytes */
  size_t           total_used;     /* sum of bytes handed out       */
};

/** Initializes `arena` with a fresh chunk of `chunk_size` bytes. Pass 0
 *  for the default (4096). On out-of-memory, head=tail=NULL — alloc returns NULL,
 *  free is a no-op. */
void   kavak_arena_init           (KavakArena *arena, size_t chunk_size);

/** Allocates `size` bytes, 8-byte aligned, zeroed. NULL on out-of-memory. */
void  *kavak_arena_alloc          (KavakArena *arena, size_t size);

/** alloc with caller-specified alignment (power of two, ≥ 1, or 0
 *  for the 8-byte default). */
void  *kavak_arena_alloc_aligned  (KavakArena *arena, size_t size, size_t align);

/** Frees all chunks and zeroes the arena struct. Idempotent. */
void   kavak_arena_free           (KavakArena *arena);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  18b. Intern pool — pointer-identity strings                            │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Deduplicating string store: equal byte sequences map to one stable,
 * NUL-terminated, pool-owned pointer, so a symbol table can compare names by
 * pointer (`a == b`) instead of strcmp. Names live until kavak_intern_pool_free.
 * Pair with kavak_source_slice to materialize interned identifiers / qualified
 * names from token spans. Single-threaded, like the rest of a kavak session. */

KavakInternPool *kavak_intern_pool_new (void);
void             kavak_intern_pool_free(KavakInternPool *pool);

/** Returns the canonical pointer for s[0..len): identical for equal byte
 *  sequences, so callers can compare interned strings by identity. The result
 *  is NUL-terminated and owned by the pool. NULL on out-of-memory or a NULL
 *  pool. A NULL `s` is treated as the empty string. */
const char      *kavak_intern          (KavakInternPool *pool,
                                        const char *s, size_t len);


/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  19. UTF-8 — decode, advance, identifier predicates                     │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Self-contained: kavak_utf8_decode is a small standards-conformant scalar
 * decoder with no external dependency. The default lexer predicate stays ASCII
 * for portability, while the Unicode predicates below classify identifiers from
 * kavak's own XID tables (generated from the Unicode Character Database), so a
 * language opts into Unicode identifiers without pulling in an ICU-class
 * library. */

/** Decodes one codepoint from [p, end). Returns bytes consumed (1..4)
 *  on success, 0 on EOF / invalid sequence. On failure `*out_cp` is
 *  left untouched. */
int kavak_utf8_decode (const char *p, const char *end, uint32_t *out_cp);

/** Like kavak_utf8_decode but skips the codepoint output. */
int kavak_utf8_advance(const char *p, const char *end);

/** ASCII default for KavakLexerConfig.is_ident_start. A-Z a-z _ . */
int kavak_ascii_is_ident_start(uint32_t cp);

/** ASCII default for KavakLexerConfig.is_ident_cont. Above + 0-9. */
int kavak_ascii_is_ident_cont (uint32_t cp);

/** Unicode identifier start predicate (kavak's generated XID tables), with `_`
 *  allowed as a language-friendly start character. */
int kavak_unicode_is_ident_start(uint32_t cp);

/** Unicode identifier continue predicate (kavak's generated XID tables), with
 *  `_` allowed. */
int kavak_unicode_is_ident_cont (uint32_t cp);


#ifdef __cplusplus
}
#endif
#endif /* KAVAK_H */
