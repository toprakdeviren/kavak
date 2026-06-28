// SPDX-License-Identifier: MIT
/**
 * @file src/dump.c
 * @brief Generic AST debug dumpers for KavakResult.
 *
 * Three serializers — indented text, JSON, S-expression — over the generic AST
 * shape. They stay language-agnostic: a node prints as `kind=N` by default, but
 * if the result's descriptor supplies the optional dump hooks (ast_kind_name /
 * ast_payload, Section 13) the dumps gain human kind names and per-kind payload
 * (operator spelling, literal text). Policy in the descriptor, walk in here.
 */

#include "kavak.h"

#include <stdio.h>
#include <string.h>

#define KAVAK_DUMP_RECURSION_LIMIT 1024u
#define KAVAK_DUMP_PAYLOAD_CAP      256u

typedef struct DumpCtx {
  const KavakLanguage *lang;
  const KavakSource   *source;
  const KavakASTNode  *seen[KAVAK_DUMP_RECURSION_LIMIT];
  uint32_t count;
} DumpCtx;

static FILE *out_or_stdout(FILE *out) {
  return out ? out : stdout;
}

static void indent(FILE *out, const int depth) {
  for (int i = 0; i < depth; ++i) fputs("  ", out);
}

static int dump_enter(DumpCtx *ctx, const KavakASTNode *node) {
  for (uint32_t i = 0; i < ctx->count; ++i) {
    if (ctx->seen[i] == node) return 0;
  }
  if (ctx->count >= KAVAK_DUMP_RECURSION_LIMIT) return 0;
  ctx->seen[ctx->count++] = node;
  return 1;
}

/* Descriptor-supplied human name for a kind, or NULL to fall back to kind=N. */
static const char *kind_name(const DumpCtx *ctx, const uint32_t kind) {
  return ctx->lang && ctx->lang->ast_kind_name ? ctx->lang->ast_kind_name(kind)
                                               : NULL;
}

/* Descriptor-supplied payload string for a node. Writes into buf (NUL-term) and
 * returns its length, or 0 when there is no payload hook / no payload. */
static size_t node_payload(const DumpCtx *ctx, const KavakASTNode *node,
                           char *buf, const size_t buf_len) {
  if (buf_len) buf[0] = '\0';
  if (!ctx->lang || !ctx->lang->ast_payload) return 0;
  return ctx->lang->ast_payload(node, ctx->source, buf, buf_len);
}

size_t kavak_json_escape(const char *s, const size_t len, char *buf,
                         const size_t buf_len) {
  static const char hex[] = "0123456789abcdef";
  size_t need = 0;
#define PUT(c) do { if (buf && need < buf_len) buf[need] = (char)(c); ++need; } while (0)
  for (size_t i = 0; s && i < len; ++i) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  PUT('\\'); PUT('"');  break;
      case '\\': PUT('\\'); PUT('\\'); break;
      case '\n': PUT('\\'); PUT('n');  break;
      case '\t': PUT('\\'); PUT('t');  break;
      case '\r': PUT('\\'); PUT('r');  break;
      case '\b': PUT('\\'); PUT('b');  break;
      case '\f': PUT('\\'); PUT('f');  break;
      default:
        if (c < 0x20u) {
          PUT('\\'); PUT('u'); PUT('0'); PUT('0');
          PUT(hex[(c >> 4) & 0xFu]); PUT(hex[c & 0xFu]);
        } else {
          PUT(c);
        }
    }
  }
#undef PUT
  if (buf && buf_len) buf[need < buf_len ? need : buf_len - 1u] = '\0';
  return need;
}

static void dump_text_node(const KavakASTNode *node, FILE *out, const int depth,
                           DumpCtx *ctx) {
  if (!node) return;
  indent(out, depth);
  const uint32_t mark = ctx->count;
  if (!dump_enter(ctx, node)) {
    fputs("<cycle-or-depth-limit>\n", out);
    return;
  }
  const char *name = kind_name(ctx, node->kind);
  if (name) fprintf(out, "%s span=%u:%u", name, node->span.start, node->span.len);
  else fprintf(out, "kind=%u span=%u:%u", node->kind, node->span.start, node->span.len);
  char payload[KAVAK_DUMP_PAYLOAD_CAP];
  if (node_payload(ctx, node, payload, sizeof(payload)) > 0) {
    fprintf(out, " payload=%s", payload);
  }
  if (node->type) fprintf(out, " type=%u", node->type->kind);
  if (node->modifiers & KAVAK_AST_FLAG_ERROR) fputs(" error", out);
  fputc('\n', out);
  for (const KavakASTNode *child = node->first_child; child;
       child = child->next_sibling) {
    dump_text_node(child, out, depth + 1, ctx);
  }
  ctx->count = mark;
}

void kavak_dump_text(const KavakResult *r, FILE *out) {
  out = out_or_stdout(out);
  const KavakASTNode *root = kavak_root(r);
  if (!root) {
    fputs("<empty>\n", out);
    return;
  }
  DumpCtx ctx = { .lang = kavak_result_language(r), .source = kavak_result_source(r), .count = 0 };
  dump_text_node(root, out, 0, &ctx);
}

/* Streams a JSON string literal straight to `out`, so unbounded inputs (e.g.
 * descriptor-supplied kind names) are never truncated through a fixed buffer.
 * The escape set matches kavak_json_escape. */
static void write_json_string(FILE *out, const char *s, const size_t len) {
  static const char hex[] = "0123456789abcdef";
  fputc('"', out);
  for (size_t i = 0; s && i < len; ++i) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\n': fputs("\\n", out);  break;
      case '\t': fputs("\\t", out);  break;
      case '\r': fputs("\\r", out);  break;
      case '\b': fputs("\\b", out);  break;
      case '\f': fputs("\\f", out);  break;
      default:
        if (c < 0x20u) {
          fputs("\\u00", out);
          fputc(hex[(c >> 4) & 0xFu], out);
          fputc(hex[c & 0xFu], out);
        } else {
          fputc((int)c, out);
        }
    }
  }
  fputc('"', out);
}

static void dump_json_node(const KavakASTNode *node, FILE *out, DumpCtx *ctx) {
  if (!node) {
    fputs("null", out);
    return;
  }
  const uint32_t mark = ctx->count;
  if (!dump_enter(ctx, node)) {
    fputs("{\"cycle\":true}", out);
    return;
  }
  fprintf(out, "{\"kind\":%u", node->kind);
  const char *name = kind_name(ctx, node->kind);
  if (name) {
    fputs(",\"kindName\":", out);
    write_json_string(out, name, strlen(name));
  }
  fprintf(out, ",\"span\":{\"start\":%u,\"len\":%u},\"error\":%s",
          node->span.start, node->span.len,
          (node->modifiers & KAVAK_AST_FLAG_ERROR) ? "true" : "false");
  if (node->type) fprintf(out, ",\"type\":%u", node->type->kind);
  char payload[KAVAK_DUMP_PAYLOAD_CAP];
  if (node_payload(ctx, node, payload, sizeof(payload)) > 0) {
    fputs(",\"payload\":", out);
    write_json_string(out, payload, strlen(payload));
  }
  fputs(",\"children\":[", out);
  for (const KavakASTNode *child = node->first_child; child;
       child = child->next_sibling) {
    if (child != node->first_child) fputc(',', out);
    dump_json_node(child, out, ctx);
  }
  fputs("]}", out);
  ctx->count = mark;
}

void kavak_dump_json(const KavakResult *r, FILE *out) {
  out = out_or_stdout(out);
  DumpCtx ctx = { .lang = kavak_result_language(r), .source = kavak_result_source(r), .count = 0 };
  dump_json_node(kavak_root(r), out, &ctx);
  fputc('\n', out);
}

static void dump_sexpr_node(const KavakASTNode *node, FILE *out, DumpCtx *ctx) {
  if (!node) {
    fputs("()", out);
    return;
  }
  const uint32_t mark = ctx->count;
  if (!dump_enter(ctx, node)) {
    fputs("(:cycle)", out);
    return;
  }
  const char *name = kind_name(ctx, node->kind);
  if (name) fprintf(out, "(%s", name);
  else fprintf(out, "(%u", node->kind);
  if (node->type) fprintf(out, " :type %u", node->type->kind);
  char payload[KAVAK_DUMP_PAYLOAD_CAP];
  if (node_payload(ctx, node, payload, sizeof(payload)) > 0) {
    fputs(" :payload ", out);
    write_json_string(out, payload, strlen(payload));
  }
  for (const KavakASTNode *child = node->first_child; child;
       child = child->next_sibling) {
    fputc(' ', out);
    dump_sexpr_node(child, out, ctx);
  }
  fputc(')', out);
  ctx->count = mark;
}

void kavak_dump_sexpr(const KavakResult *r, FILE *out) {
  out = out_or_stdout(out);
  DumpCtx ctx = { .lang = kavak_result_language(r), .source = kavak_result_source(r), .count = 0 };
  dump_sexpr_node(kavak_root(r), out, &ctx);
  fputc('\n', out);
}
