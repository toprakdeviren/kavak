// SPDX-License-Identifier: MIT
/**
 * @file tests/bench_lexer.c
 * @brief Front-end byte-scanning throughput benchmark.
 *
 * Answers one question: how much of kavak's front-end byte cost is spent
 * *scanning* (newline counting in source init, identifier/whitespace runs in
 * the lexer) versus per-token bookkeeping? That ratio decides whether SIMD
 * byte-scan kernels (platform/) are worth their build complexity.
 *
 * Each shape is a large synthetic buffer stressing one scanner:
 *   ident-run   — long ASCII identifiers (scan-bound: max bytes/token)
 *   newline     — short lines (stresses source-init's two-pass newline scan)
 *   whitespace  — sparse tokens in a sea of spaces (whitespace-skip bound)
 *   tokens      — many tiny tokens (per-token dispatch bound; scan-light)
 *   unicode     — 2-byte UTF-8 identifiers (UTF-8 decode + XID lookup path)
 *
 * For each we report MB/s for source-init alone and for a full lex, so the
 * scan share is visible directly. Informational, not a regression gate.
 * Run via `make bench`.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

#define BUF_BYTES (8u * 1024u * 1024u) /* 8 MiB per shape */
#define ITERS     8u

typedef enum { SH_IDENT, SH_NEWLINE, SH_WS, SH_TOKENS, SH_UNICODE } Shape;

/* Fill `buf` (capacity BUF_BYTES) with a shape; returns bytes written. */
static size_t fill_shape(char *buf, Shape shape) {
  size_t n = 0;
  switch (shape) {
    case SH_IDENT: {
      /* 47-char identifier + space → ~48 bytes/token, scan-dominated. */
      while (n + 48 < BUF_BYTES) {
        for (int i = 0; i < 47; ++i) buf[n++] = (char)('a' + (i % 26));
        buf[n++] = ' ';
      }
      break;
    }
    case SH_NEWLINE: {
      /* "abc\n" → one short line, maximal newline density. */
      while (n + 4 < BUF_BYTES) {
        buf[n++] = 'a'; buf[n++] = 'b'; buf[n++] = 'c'; buf[n++] = '\n';
      }
      break;
    }
    case SH_WS: {
      /* one token in ~16 spaces → whitespace-skip dominated. */
      while (n + 16 < BUF_BYTES) {
        buf[n++] = 'x';
        for (int i = 0; i < 15; ++i) buf[n++] = ' ';
      }
      break;
    }
    case SH_TOKENS: {
      /* "a " → shortest identifier + space, per-token-dispatch dominated. */
      while (n + 2 < BUF_BYTES) { buf[n++] = 'a'; buf[n++] = ' '; }
      break;
    }
    case SH_UNICODE: {
      /* 23×(2-byte U+00F1 'ñ') + space → UTF-8 decode + XID lookup per char. */
      while (n + 48 < BUF_BYTES) {
        for (int i = 0; i < 23; ++i) { buf[n++] = (char)0xC3; buf[n++] = (char)0xB1; }
        buf[n++] = ' ';
      }
      break;
    }
  }
  return n;
}

static const char *shape_name(Shape s) {
  switch (s) {
    case SH_IDENT:   return "ident-run  ";
    case SH_NEWLINE: return "newline    ";
    case SH_WS:      return "whitespace ";
    case SH_TOKENS:  return "tokens     ";
    case SH_UNICODE: return "unicode    ";
  }
  return "?";
}

/* Time building the line-offset table: init + the first kavak_source_pos that
 * forces the (now single-pass, lazy) newline scan. This is the cost paid only
 * when a consumer asks for a diagnostic's line/col; a clean parse pays zero. */
static double bench_source(const char *buf, size_t len) {
  double best = 1e30;
  for (uint32_t it = 0; it < ITERS; ++it) {
    KavakSource src;
    double t0 = now_ms();
    if (kavak_source_init(&src, buf, len, "bench") != 0) { fprintf(stderr, "source_init failed\n"); exit(1); }
    uint32_t line = 0, col = 0;
    kavak_source_pos(&src, len / 2, &line, &col);  /* force the lazy build */
    double dt = now_ms() - t0;
    if (dt < best) best = dt;
    kavak_source_free(&src);
  }
  return best;
}

/* Time a full `kavak_lex` over the buffer; reports token count via *out_tok. */
static double bench_lex(const char *buf, size_t len, const KavakLexerConfig *cfg,
                        uint32_t *out_tok) {
  double best = 1e30;
  uint32_t tok = 0;
  for (uint32_t it = 0; it < ITERS; ++it) {
    KavakSource src;
    kavak_source_init(&src, buf, len, "bench");
    KavakTokenVec tv; kavak_token_vec_init(&tv);
    double t0 = now_ms();
    kavak_lex(&src, cfg, &tv, NULL);
    double dt = now_ms() - t0;
    if (dt < best) best = dt;
    tok = tv.count;
    kavak_token_vec_free(&tv);
    kavak_source_free(&src);
  }
  if (out_tok) *out_tok = tok;
  return best;
}

static double mbps(size_t bytes, double ms) {
  return (double)bytes / 1.0e6 / (ms / 1000.0);
}

int main(void) {
  char *buf = malloc(BUF_BYTES);
  if (!buf) { fprintf(stderr, "OOM\n"); return 1; }

  /* Minimal ASCII config: exercises identifier / number / whitespace / newline
   * scanners — exactly the paths a SIMD byte kernel would target. */
  KavakLexerConfig ascii_cfg;
  memset(&ascii_cfg, 0, sizeof(ascii_cfg));

  /* Same, but routing identifier predicates through the Unicode/XID path. */
  KavakLexerConfig uni_cfg = ascii_cfg;
  uni_cfg.is_ident_start = kavak_unicode_is_ident_start;
  uni_cfg.is_ident_cont  = kavak_unicode_is_ident_cont;

  printf("kavak front-end scanning benchmark  (%u MiB/shape, best of %u)\n\n",
         BUF_BYTES / (1024u * 1024u), ITERS);
  printf("  %-11s %10s %12s %12s %10s\n",
         "shape", "src MB/s", "lex MB/s", "tokens", "src/lex %");

  const Shape shapes[] = { SH_IDENT, SH_NEWLINE, SH_WS, SH_TOKENS, SH_UNICODE };
  for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
    Shape s = shapes[i];
    size_t len = fill_shape(buf, s);
    const KavakLexerConfig *cfg = (s == SH_UNICODE) ? &uni_cfg : &ascii_cfg;

    double s_ms = bench_source(buf, len);
    uint32_t tok = 0;
    double l_ms = bench_lex(buf, len, cfg, &tok);

    /* Share of (source-init) within the full front-end byte cost. The lex pass
     * already re-runs source init internally is NOT the case here — kavak_lex
     * takes a pre-built source — so src and lex are independent measurements;
     * the % shows newline-scan cost relative to the lex pass. */
    double share = 100.0 * s_ms / (l_ms);
    printf("  %-11s %10.0f %12.0f %12u %9.1f\n",
           shape_name(s), mbps(len, s_ms), mbps(len, l_ms), tok, share);
  }

  printf("\nRead: 'lex MB/s' is end-to-end tokenization throughput; 'src MB/s' is\n"
         "newline scanning alone. Low lex MB/s on ident-run/whitespace ⇒ scan-bound\n"
         "(SIMD can help). High src/lex %% ⇒ newline scanning is a real slice.\n");

  free(buf);
  return 0;
}
