// SPDX-License-Identifier: MIT
/**
 * @file src/dump.c
 * @brief Generic AST debug dumpers for KavakResult.
 */

#include "kavak.h"

#include <stdio.h>

#define KAVAK_DUMP_RECURSION_LIMIT 1024u

typedef struct DumpCtx {
  const KavakASTNode *seen[KAVAK_DUMP_RECURSION_LIMIT];
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

static void dump_text_node(const KavakASTNode *node, FILE *out, const int depth,
                           DumpCtx *ctx) {
  if (!node) return;
  indent(out, depth);
  const uint32_t mark = ctx->count;
  if (!dump_enter(ctx, node)) {
    fputs("<cycle-or-depth-limit>\n", out);
    return;
  }
  fprintf(out, "kind=%u span=%u:%u", node->kind, node->span.start, node->span.len);
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
  DumpCtx ctx = {0};
  dump_text_node(root, out, 0, &ctx);
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
  fprintf(out,
          "{\"kind\":%u,\"span\":{\"start\":%u,\"len\":%u},\"error\":%s",
          node->kind, node->span.start, node->span.len,
          (node->modifiers & KAVAK_AST_FLAG_ERROR) ? "true" : "false");
  if (node->type) fprintf(out, ",\"type\":%u", node->type->kind);
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
  DumpCtx ctx = {0};
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
  fprintf(out, "(%u", node->kind);
  if (node->type) fprintf(out, " :type %u", node->type->kind);
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
  DumpCtx ctx = {0};
  dump_sexpr_node(kavak_root(r), out, &ctx);
  fputc('\n', out);
}
