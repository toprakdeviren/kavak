// SPDX-License-Identifier: MIT
/**
 * @file src/kavak.c
 * @brief Top-level kavak entry points.
 *
 * Owns session/result lifetime and coordinates source setup, descriptor
 * hooks, token storage, diagnostics, AST arena, and analysis cleanup.
 */

#include "kavak.h"
#include "kavak_internal.h"

#include <stdlib.h>
#include <string.h>

const char *kavak_version(void) { return KAVAK_VERSION_STRING; }

/* Session is opaque in kavak.h; the internal struct carries the
 * descriptor pointer and per-session type arena through the analysis
 * lifetime while the public surface stays stable. */
struct KavakSession {
  const KavakLanguage *lang;
  KavakTypeArena       type_arena;
};

struct KavakResult {
  KavakSession  *session;
  KavakSource    source;
  KavakTokenVec  tokens;
  KavakDiagVec   diags;
  KavakArena     ast_arena;
  KavakASTNode  *root;
};

static int result_init(KavakResult *result, KavakSession *session,
                       const char *bytes, const size_t len,
                       const char *filename,
                       const uint32_t newline_flags) {
  memset(result, 0, sizeof(*result));
  result->session = session;
  kavak_token_vec_init(&result->tokens);
  kavak_diag_vec_init(&result->diags);
  /* 64 KiB keeps common ASTs in a small number of chunks without making
   * empty or tiny analyses reserve much memory. */
  kavak_arena_init(&result->ast_arena, 64u * 1024u);
  if (!result->ast_arena.tail) return -1;
  if (kavak_source_init_with_newlines(&result->source, bytes, len,
                                      filename ? filename : "<memory>",
                                      newline_flags) != 0) {
    return -1;
  }
  return 0;
}

static void result_cleanup(KavakResult *result) {
  if (!result) return;
  kavak_arena_free(&result->ast_arena);
  kavak_diag_vec_free(&result->diags);
  kavak_token_vec_free(&result->tokens);
  kavak_source_free(&result->source);
}

static void result_diag(KavakResult *result, const char *message) {
  if (!result) return;
  (void)kavak_diag_vec_push(&result->diags, (KavakDiag){
    .severity = KAVAK_SEV_ERROR,
    .message = message,
    .span = KAVAK_SPAN_NONE,
  });
}

KavakSession *kavak_session_new(const KavakLanguage *lang) {
  if (!lang) return NULL;
  kavak_utf8_init();
  KavakSession *session = calloc(1, sizeof(*session));
  if (!session) return NULL;
  session->lang = lang;
  kavak_type_arena_init(&session->type_arena);
  if (!session->type_arena.aux || !session->type_arena.aux->tail) {
    kavak_type_arena_free(&session->type_arena);
    free(session);
    return NULL;
  }
  return session;
}

void kavak_session_free(KavakSession *session) {
  if (!session) return;
  kavak_type_arena_free(&session->type_arena);
  free(session);
}

KavakTypeArena *kavak_session_type_arena(KavakSession *session) {
  return session ? &session->type_arena : NULL;
}

int kavak_session_reset_types(KavakSession *session) {
  return session ? kavak_type_arena_reset(&session->type_arena) : -1;
}

const KavakLanguage *kavak_session_language(const KavakSession *session) {
  return session ? session->lang : NULL;
}

KavakResult *kavak_analyze(KavakSession *session,
                            const char   *source,
                            const char   *filename) {
  if (!session || !source) return NULL;
  return kavak_analyze_bytes(session, source, strlen(source), filename);
}

KavakResult *kavak_analyze_bytes(KavakSession *session,
                                 const char   *bytes,
                                 const size_t  len,
                                 const char   *filename) {
  if (!session || (len != 0 && !bytes)) return NULL;
  const KavakLanguage *lang = session->lang;
  if (!lang) return NULL;
  KavakResult *result = calloc(1, sizeof(*result));
  if (!result) return NULL;
  if (result_init(result, session, bytes, len, filename,
                  lang->lexer.newline_flags) != 0) {
    result_cleanup(result);
    free(result);
    return NULL;
  }

  if (kavak_lex(&result->source, &lang->lexer, &result->tokens,
                &result->diags) != 0) {
    result_cleanup(result);
    free(result);
    return NULL;
  }

  if (!lang->parse_source) {
    result_diag(result, "language descriptor missing parse_source");
    return result;
  }

  KavakParser *parser = kavak_parser_new(&result->source,
                                         result->tokens.items,
                                         result->tokens.count,
                                         &result->ast_arena,
                                         &result->diags,
                                         &lang->parser);
  if (!parser) {
    result_cleanup(result);
    free(result);
    return NULL;
  }
  result->root = lang->parse_source(parser);
  kavak_parser_free(parser);
  if (!result->root) {
    result_diag(result, "parse failed");
    return result;
  }

  KavakSema *sema = kavak_sema_new(session, &result->source,
                                   result->tokens.items,
                                   result->tokens.count,
                                   result->root,
                                   &result->diags);
  if (!sema) {
    result_cleanup(result);
    free(result);
    return NULL;
  }

  if (lang->pre_resolve) lang->pre_resolve(sema, result->root);
  (void)kavak_sema_resolve_names(sema, result->root);
  if (lang->post_sema) lang->post_sema(sema, result->root);
  kavak_sema_free(sema);
  return result;
}

void kavak_result_free(KavakResult *r) {
  if (!r) return;
  result_cleanup(r);
  free(r);
}

const KavakASTNode *kavak_root        (const KavakResult *r) { return r ? r->root : NULL; }
const KavakSource  *kavak_result_source(const KavakResult *r) { return r ? &r->source : NULL; }
const KavakToken   *kavak_tokens      (const KavakResult *r) { return r ? r->tokens.items : NULL; }
size_t              kavak_token_count (const KavakResult *r) { return r ? r->tokens.count : 0; }
const KavakLanguage *kavak_result_language(const KavakResult *r) {
  return r && r->session ? r->session->lang : NULL;
}

uint32_t kavak_error_count(const KavakResult *r) {
  return r ? kavak_diag_error_count(&r->diags) : 0;
}

static const KavakDiag *nth_error(const KavakResult *r, const uint32_t i) {
  if (!r) return NULL;
  uint32_t seen = 0;
  for (uint32_t n = 0; n < r->diags.count; ++n) {
    if (r->diags.items[n].severity != KAVAK_SEV_ERROR) continue;
    if (seen == i) return &r->diags.items[n];
    ++seen;
  }
  return NULL;
}

const char *kavak_error_message(const KavakResult *r, const uint32_t i) {
  const KavakDiag *diag = nth_error(r, i);
  return diag && diag->message ? diag->message : "";
}

uint32_t kavak_error_line(const KavakResult *r, const uint32_t i) {
  const KavakDiag *diag = nth_error(r, i);
  if (!r || !diag) return 0;
  uint32_t line = 0;
  kavak_source_pos(&r->source, diag->span.start, &line, NULL);
  return line;
}

uint32_t kavak_error_col(const KavakResult *r, const uint32_t i) {
  const KavakDiag *diag = nth_error(r, i);
  if (!r || !diag) return 0;
  uint32_t col = 0;
  kavak_source_pos(&r->source, diag->span.start, NULL, &col);
  return col;
}

int kavak_error_at(const KavakResult *r, const uint32_t i,
                   const char **out_message, uint32_t *out_line,
                   uint32_t *out_col) {
  const KavakDiag *diag = nth_error(r, i);
  if (!diag) {
    if (out_message) *out_message = "";
    if (out_line) *out_line = 0;
    if (out_col) *out_col = 0;
    return 0;
  }
  if (out_message) *out_message = diag->message ? diag->message : "";
  if (out_line || out_col) {
    uint32_t line = 0, col = 0;
    kavak_source_pos(&r->source, diag->span.start, &line, &col);
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
  }
  return 1;
}
