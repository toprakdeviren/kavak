// SPDX-License-Identifier: MIT
/**
 * @file src/type.c
 * @brief Type-system toolkit: TypeInfo arena, constructors, helpers.
 */

#include "kavak.h"
#include "kavak_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KAVAK_TYPE_ARENA_CHUNK_SIZE 256u
/* Soft cap on type-graph recursion (equality, to_string, substitute). Shares
 * the single KAVAK_RECURSION_LIMIT with the parser's expression-depth guard so
 * the two cannot drift. Types nested deeper than this are compared / printed /
 * substituted conservatively instead of overflowing the C stack. */
#define KAVAK_TYPE_RECURSION_LIMIT KAVAK_RECURSION_LIMIT

struct KavakTypeArenaChunk {
  KavakTypeArenaChunk *next;
  size_t               used;
  KavakTypeInfo        items[KAVAK_TYPE_ARENA_CHUNK_SIZE];
};

typedef struct TypeString {
  char  *buf;
  size_t cap;
  size_t len;
} TypeString;

typedef struct TypePair {
  const KavakTypeInfo *a;
  const KavakTypeInfo *b;
} TypePair;

typedef struct TypeEqualCtx {
  TypePair pairs[KAVAK_TYPE_RECURSION_LIMIT];
  uint32_t count;
} TypeEqualCtx;

typedef struct TypeStringCtx {
  const KavakTypeInfo *seen[KAVAK_TYPE_RECURSION_LIMIT];
  uint32_t count;
} TypeStringCtx;

static int array_bytes(const uint32_t count, const size_t item_size,
                       size_t *out_bytes) {
  if (!out_bytes) return 0;
  if (item_size != 0 && (size_t)count > SIZE_MAX / item_size) return 0;
  *out_bytes = (size_t)count * item_size;
  return 1;
}

static int is_builtin_kind(const uint32_t kind) {
  return kind <= KAVAK_TY_STRING;
}

static const char *builtin_name(const uint32_t kind) {
  switch (kind) {
    case KAVAK_TY_INVALID: return "<invalid>";
    case KAVAK_TY_VOID:    return "Void";
    case KAVAK_TY_NEVER:   return "Never";
    case KAVAK_TY_ANY:     return "Any";
    case KAVAK_TY_BYTE:    return "Byte";
    case KAVAK_TY_UBYTE:   return "UByte";
    case KAVAK_TY_SHORT:   return "Short";
    case KAVAK_TY_USHORT:  return "UShort";
    case KAVAK_TY_INT:     return "Int";
    case KAVAK_TY_UINT:    return "UInt";
    case KAVAK_TY_LONG:    return "Long";
    case KAVAK_TY_ULONG:   return "ULong";
    case KAVAK_TY_FLOAT:   return "Float";
    case KAVAK_TY_DOUBLE:  return "Double";
    case KAVAK_TY_BOOL:    return "Bool";
    case KAVAK_TY_CHAR:    return "Char";
    case KAVAK_TY_STRING:  return "String";
  }
  return NULL;
}

static int str_eq(const char *a, const char *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

static void init_builtins(KavakTypeArena *arena) {
  if (!arena) return;
  for (uint32_t i = 0; i < KAVAK_TY_BUILTIN_COUNT; ++i) {
    arena->builtins[i].kind = i;
  }
}

void kavak_type_arena_init(KavakTypeArena *arena) {
  if (!arena) return;
  memset(arena, 0, sizeof(*arena));
  arena->aux = malloc(sizeof(*arena->aux));
  if (arena->aux) kavak_arena_init(arena->aux, 0);
  init_builtins(arena);
}

void kavak_type_arena_free(KavakTypeArena *arena) {
  if (!arena) return;
  KavakTypeArenaChunk *chunk = arena->head;
  while (chunk) {
    KavakTypeArenaChunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  if (arena->aux) {
    kavak_arena_free(arena->aux);
    free(arena->aux);
  }
  memset(arena, 0, sizeof(*arena));
}

int kavak_type_arena_reset(KavakTypeArena *arena) {
  if (!arena) return -1;
  /* Reclaim every interned type but keep the arena usable for the next
   * analysis. Reuses the existing aux KavakArena struct in place (free wipes it
   * without releasing the struct itself). All KavakTypeInfo* handed out before
   * this call are invalidated — see the header contract. */
  KavakTypeArenaChunk *chunk = arena->head;
  while (chunk) {
    KavakTypeArenaChunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  arena->head = arena->tail = NULL;
  if (arena->aux) {
    kavak_arena_free(arena->aux);
    kavak_arena_init(arena->aux, 0);
    if (!arena->aux->tail) return -1;  /* OOM: aux store could not be rebuilt */
  }
  memset(arena->builtins, 0, sizeof(arena->builtins));
  init_builtins(arena);
  return 0;
}

KavakTypeInfo *kavak_ty_alloc(KavakTypeArena *arena, const uint32_t kind) {
  if (!arena) return NULL;
  if (!arena->tail || arena->tail->used == KAVAK_TYPE_ARENA_CHUNK_SIZE) {
    KavakTypeArenaChunk *chunk = calloc(1, sizeof(*chunk));
    if (!chunk) return NULL;
    if (arena->tail) arena->tail->next = chunk;
    else arena->head = chunk;
    arena->tail = chunk;
  }
  KavakTypeInfo *type = &arena->tail->items[arena->tail->used++];
  type->kind = kind;
  return type;
}

KavakTypeInfo *kavak_ty_builtin(KavakTypeArena *arena, const uint32_t kind) {
  if (!arena || !is_builtin_kind(kind)) return NULL;
  if (arena->builtins[kind].kind != kind) init_builtins(arena);
  return &arena->builtins[kind];
}

static KavakTypeInfo **copy_type_array(KavakTypeArena *arena,
                                       KavakTypeInfo *const *items,
                                       const uint32_t count) {
  if (count == 0) return NULL;
  if (!arena || !arena->aux || !items) return NULL;
  size_t bytes = 0;
  if (!array_bytes(count, sizeof(KavakTypeInfo *), &bytes)) return NULL;
  KavakTypeInfo **copy = kavak_arena_alloc(arena->aux, bytes);
  if (!copy) return NULL;
  for (uint32_t i = 0; i < count; ++i) copy[i] = items[i];
  return copy;
}

static KavakRecordField *copy_record_fields(KavakTypeArena *arena,
                                            const KavakRecordField *fields,
                                            const uint32_t count) {
  if (count == 0) return NULL;
  if (!arena || !arena->aux || !fields) return NULL;
  size_t bytes = 0;
  if (!array_bytes(count, sizeof(KavakRecordField), &bytes)) return NULL;
  KavakRecordField *copy = kavak_arena_alloc(arena->aux, bytes);
  if (!copy) return NULL;
  for (uint32_t i = 0; i < count; ++i) copy[i] = fields[i];
  return copy;
}

static int record_fields_valid(KavakTypeInfo *const *positional,
                               const uint32_t positional_count,
                               const KavakRecordField *named,
                               const uint32_t named_count) {
  for (uint32_t i = 0; i < positional_count; ++i) {
    if (!positional[i]) return 0;
  }
  for (uint32_t i = 0; i < named_count; ++i) {
    if (!named[i].name || !named[i].type) return 0;
  }
  return 1;
}

KavakTypeInfo *kavak_ty_nullable(KavakTypeArena *arena, KavakTypeInfo *inner) {
  if (!inner) return NULL;
  KavakTypeInfo *type = kavak_ty_alloc(arena, KAVAK_TY_NULLABLE);
  if (!type) return NULL;
  type->payload.nullable.inner = inner;
  return type;
}

KavakTypeInfo *kavak_ty_function(KavakTypeArena *arena,
                                 KavakTypeInfo *receiver,
                                 KavakTypeInfo *const *params,
                                 const uint32_t param_count,
                                 KavakTypeInfo *ret,
                                 const uint32_t flags) {
  if (!ret || (param_count != 0 && !params)) return NULL;
  KavakTypeInfo **params_copy = copy_type_array(arena, params, param_count);
  if (param_count != 0 && !params_copy) return NULL;
  KavakTypeInfo *type = kavak_ty_alloc(arena, KAVAK_TY_FUNCTION);
  if (!type) return NULL;
  type->flags = flags;
  type->payload.function.receiver = receiver;
  type->payload.function.params = params_copy;
  type->payload.function.param_count = param_count;
  type->payload.function.ret = ret;
  return type;
}

KavakTypeInfo *kavak_ty_named(KavakTypeArena *arena,
                              const char *name,
                              KavakTypeInfo *const *args,
                              const uint32_t arg_count,
                              const KavakASTNode *decl) {
  if (!name || (arg_count != 0 && !args)) return NULL;
  KavakTypeInfo **args_copy = copy_type_array(arena, args, arg_count);
  if (arg_count != 0 && !args_copy) return NULL;
  KavakTypeInfo *type = kavak_ty_alloc(arena, KAVAK_TY_NAMED);
  if (!type) return NULL;
  type->payload.named.name = name;
  type->payload.named.args = args_copy;
  type->payload.named.arg_count = arg_count;
  type->payload.named.decl = decl;
  type->payload.named.underlying = NULL;
  return type;
}

KavakTypeInfo *kavak_ty_param(KavakTypeArena *arena,
                              const char *name,
                              const uint32_t index,
                              const KavakASTNode *decl) {
  KavakTypeInfo *type = kavak_ty_alloc(arena, KAVAK_TY_PARAM);
  if (!type) return NULL;
  type->payload.param.name = name;
  type->payload.param.index = index;
  type->payload.param.decl = decl;
  return type;
}

KavakTypeInfo *kavak_ty_record(KavakTypeArena *arena,
                               KavakTypeInfo *const *positional,
                               const uint32_t positional_count,
                               const KavakRecordField *named,
                               const uint32_t named_count) {
  if ((positional_count != 0 && !positional) || (named_count != 0 && !named)) {
    return NULL;
  }
  if (!record_fields_valid(positional, positional_count, named, named_count)) {
    return NULL;
  }
  KavakTypeInfo **pos_copy = copy_type_array(arena, positional, positional_count);
  if (positional_count != 0 && !pos_copy) return NULL;
  KavakRecordField *named_copy = copy_record_fields(arena, named, named_count);
  if (named_count != 0 && !named_copy) return NULL;
  KavakTypeInfo *type = kavak_ty_alloc(arena, KAVAK_TY_RECORD);
  if (!type) return NULL;
  type->payload.record.positional = pos_copy;
  type->payload.record.positional_count = positional_count;
  type->payload.record.named = named_copy;
  type->payload.record.named_count = named_count;
  return type;
}

static int equal_pair_seen(const TypeEqualCtx *ctx,
                           const KavakTypeInfo *a,
                           const KavakTypeInfo *b) {
  for (uint32_t i = 0; i < ctx->count; ++i) {
    if (ctx->pairs[i].a == a && ctx->pairs[i].b == b) return 1;
  }
  return 0;
}

static int equal_pair_push(TypeEqualCtx *ctx,
                           const KavakTypeInfo *a,
                           const KavakTypeInfo *b) {
  if (ctx->count >= KAVAK_TYPE_RECURSION_LIMIT) return 0;
  ctx->pairs[ctx->count++] = (TypePair){ a, b };
  return 1;
}

static int type_equal_nominal_inner(const KavakTypeInfo *a,
                                    const KavakTypeInfo *b,
                                    TypeEqualCtx *ctx) {
  if (a == b) return 1;
  if (!a || !b || a->kind != b->kind) return 0;
  if (is_builtin_kind(a->kind)) return 1;
  if (equal_pair_seen(ctx, a, b)) return 1;
  if (!equal_pair_push(ctx, a, b)) return 0;

  const uint32_t mark = ctx->count - 1u;
  int equal = 0;

  switch (a->kind) {
    case KAVAK_TY_NULLABLE:
      equal = type_equal_nominal_inner(a->payload.nullable.inner,
                                       b->payload.nullable.inner, ctx);
      break;
    case KAVAK_TY_FUNCTION:
      equal = a->flags == b->flags &&
              (!!a->payload.function.receiver == !!b->payload.function.receiver) &&
              a->payload.function.param_count == b->payload.function.param_count;
      break;
    case KAVAK_TY_NAMED:
      equal = str_eq(a->payload.named.name, b->payload.named.name) &&
              a->payload.named.arg_count == b->payload.named.arg_count;
      break;
    case KAVAK_TY_PARAM:
      equal = a->payload.param.index == b->payload.param.index &&
              str_eq(a->payload.param.name, b->payload.param.name);
      break;
    case KAVAK_TY_RECORD:
      if (a->payload.record.positional_count != b->payload.record.positional_count ||
          a->payload.record.named_count != b->payload.record.named_count) {
        break;
      }
      equal = 1;
      for (uint32_t i = 0; i < a->payload.record.named_count; ++i) {
        if (!str_eq(a->payload.record.named[i].name,
                    b->payload.record.named[i].name)) {
          equal = 0;
          break;
        }
      }
      break;
  }
  ctx->count = mark;
  return equal;
}

int kavak_ty_equal_nominal(const KavakTypeInfo *a, const KavakTypeInfo *b) {
  TypeEqualCtx ctx;
  ctx.count = 0;  /* pairs[] is written before read; no need to zero 8 KB */
  return type_equal_nominal_inner(a, b, &ctx);
}

static int type_equal_deep_inner(const KavakTypeInfo *a, const KavakTypeInfo *b,
                                 TypeEqualCtx *ctx) {
  if (a == b) return 1;
  if (!a || !b || a->kind != b->kind) return 0;
  if (is_builtin_kind(a->kind)) return 1;
  if (equal_pair_seen(ctx, a, b)) return 1;
  if (!equal_pair_push(ctx, a, b)) return 0;

  const uint32_t mark = ctx->count - 1u;
  int equal = 0;

  switch (a->kind) {
    case KAVAK_TY_NULLABLE:
      equal = type_equal_deep_inner(a->payload.nullable.inner,
                                    b->payload.nullable.inner, ctx);
      break;
    case KAVAK_TY_FUNCTION:
      if (a->flags != b->flags ||
          a->payload.function.param_count != b->payload.function.param_count ||
          !type_equal_deep_inner(a->payload.function.receiver,
                                 b->payload.function.receiver, ctx) ||
          !type_equal_deep_inner(a->payload.function.ret,
                                 b->payload.function.ret, ctx)) {
        break;
      }
      equal = 1;
      for (uint32_t i = 0; i < a->payload.function.param_count; ++i) {
        if (!type_equal_deep_inner(a->payload.function.params[i],
                                   b->payload.function.params[i], ctx)) {
          equal = 0;
          break;
        }
      }
      break;
    case KAVAK_TY_NAMED:
      if (!str_eq(a->payload.named.name, b->payload.named.name) ||
          a->payload.named.arg_count != b->payload.named.arg_count ||
          a->payload.named.decl != b->payload.named.decl) {
        break;
      }
      equal = 1;
      for (uint32_t i = 0; i < a->payload.named.arg_count; ++i) {
        if (!type_equal_deep_inner(a->payload.named.args[i],
                                   b->payload.named.args[i], ctx)) {
          equal = 0;
          break;
        }
      }
      break;
    case KAVAK_TY_PARAM:
      equal = a->payload.param.index == b->payload.param.index &&
              a->payload.param.decl == b->payload.param.decl &&
              str_eq(a->payload.param.name, b->payload.param.name);
      break;
    case KAVAK_TY_RECORD:
      if (a->payload.record.positional_count != b->payload.record.positional_count ||
          a->payload.record.named_count != b->payload.record.named_count) {
        break;
      }
      equal = 1;
      for (uint32_t i = 0; i < a->payload.record.positional_count; ++i) {
        if (!type_equal_deep_inner(a->payload.record.positional[i],
                                   b->payload.record.positional[i], ctx)) {
          equal = 0;
          break;
        }
      }
      if (!equal) break;
      for (uint32_t i = 0; i < a->payload.record.named_count; ++i) {
        if (!str_eq(a->payload.record.named[i].name,
                    b->payload.record.named[i].name) ||
            !type_equal_deep_inner(a->payload.record.named[i].type,
                                   b->payload.record.named[i].type, ctx)) {
          equal = 0;
          break;
        }
      }
      break;
  }
  ctx->count = mark;
  return equal;
}

int kavak_ty_equal_deep(const KavakTypeInfo *a, const KavakTypeInfo *b) {
  TypeEqualCtx ctx;
  ctx.count = 0;  /* pairs[] is written before read; no need to zero 8 KB */
  return type_equal_deep_inner(a, b, &ctx);
}

static KavakTypeInfo *subst_lookup(const KavakTypeInfo *type,
                                   const KavakTypeSubst *subst,
                                   const uint32_t subst_count) {
  if (!type || type->kind != KAVAK_TY_PARAM || !subst) return NULL;
  for (uint32_t i = 0; i < subst_count; ++i) {
    if (subst[i].param == type) return subst[i].replacement;
  }
  return NULL;
}

static KavakTypeInfo *subst_inner(KavakTypeArena *arena, const KavakTypeInfo *type,
                                  const KavakTypeSubst *subst, uint32_t subst_count,
                                  uint32_t depth);

static KavakTypeInfo **subst_type_array(KavakTypeArena *arena,
                                        KavakTypeInfo *const *items,
                                        const uint32_t count,
                                        const KavakTypeSubst *subst,
                                        const uint32_t subst_count,
                                        int *changed,
                                        const uint32_t depth) {
  if (count == 0) return NULL;
  size_t bytes = 0;
  if (!array_bytes(count, sizeof(KavakTypeInfo *), &bytes)) return NULL;
  KavakTypeInfo **copy = malloc(bytes);
  if (!copy) return NULL;
  for (uint32_t i = 0; i < count; ++i) {
    copy[i] = subst_inner(arena, items[i], subst, subst_count, depth);
    if (!copy[i]) {
      free(copy);
      return NULL;
    }
    if (copy[i] != items[i]) *changed = 1;
  }
  return copy;
}

static KavakTypeInfo *subst_inner(KavakTypeArena *arena,
                                  const KavakTypeInfo *type,
                                  const KavakTypeSubst *subst,
                                  uint32_t subst_count,
                                  uint32_t depth) {
  if (!type) return NULL;
  KavakTypeInfo *replacement = subst_lookup(type, subst, subst_count);
  if (replacement) return replacement;
  if (subst_count == 0 || is_builtin_kind(type->kind)) {
    return (KavakTypeInfo *)type;
  }
  /* Soft recursion cap: a cyclic type (e.g. named.underlying = self) or a
   * pathologically deep one stops substituting here and returns the original
   * node, rather than overflowing the C stack. */
  if (depth >= KAVAK_TYPE_RECURSION_LIMIT) return (KavakTypeInfo *)type;

  switch (type->kind) {
    case KAVAK_TY_NULLABLE: {
      KavakTypeInfo *inner = subst_inner(arena, type->payload.nullable.inner,
                                         subst, subst_count, depth + 1u);
      if (!inner) return NULL;
      return inner == type->payload.nullable.inner
           ? (KavakTypeInfo *)type
           : kavak_ty_nullable(arena, inner);
    }
    case KAVAK_TY_FUNCTION: {
      int changed = 0;
      KavakTypeInfo *receiver = NULL;
      if (type->payload.function.receiver) {
        receiver = subst_inner(arena, type->payload.function.receiver,
                               subst, subst_count, depth + 1u);
        if (!receiver) return NULL;
        if (receiver != type->payload.function.receiver) changed = 1;
      }
      KavakTypeInfo *ret = subst_inner(arena, type->payload.function.ret,
                                       subst, subst_count, depth + 1u);
      if (!ret) return NULL;
      if (ret != type->payload.function.ret) changed = 1;
      KavakTypeInfo **params = subst_type_array(arena, type->payload.function.params,
                                                type->payload.function.param_count,
                                                subst, subst_count, &changed, depth + 1u);
      if (type->payload.function.param_count != 0 && !params) return NULL;
      if (!changed) {
        free(params);
        return (KavakTypeInfo *)type;
      }
      KavakTypeInfo *out = kavak_ty_function(arena, receiver, params,
                                             type->payload.function.param_count,
                                             ret, type->flags);
      free(params);
      return out;
    }
    case KAVAK_TY_NAMED: {
      int changed = 0;
      KavakTypeInfo **args = subst_type_array(arena, type->payload.named.args,
                                              type->payload.named.arg_count,
                                              subst, subst_count, &changed, depth + 1u);
      if (type->payload.named.arg_count != 0 && !args) return NULL;

      KavakTypeInfo *underlying = NULL;
      if (type->payload.named.underlying) {
        underlying = subst_inner(arena, type->payload.named.underlying,
                                 subst, subst_count, depth + 1u);
        if (!underlying) {
          free(args);
          return NULL;
        }
        if (underlying != type->payload.named.underlying) changed = 1;
      }

      if (!changed) {
        free(args);
        return (KavakTypeInfo *)type;
      }
      KavakTypeInfo *out = kavak_ty_named(arena, type->payload.named.name, args,
                                          type->payload.named.arg_count,
                                          type->payload.named.decl);
      if (out) out->payload.named.underlying = underlying;
      free(args);
      return out;
    }
    case KAVAK_TY_RECORD: {
      int changed = 0;
      KavakTypeInfo **pos = subst_type_array(arena, type->payload.record.positional,
                                             type->payload.record.positional_count,
                                             subst, subst_count, &changed, depth + 1u);
      if (type->payload.record.positional_count != 0 && !pos) return NULL;

      KavakRecordField *named = NULL;
      if (type->payload.record.named_count != 0) {
        size_t bytes = 0;
        if (!array_bytes(type->payload.record.named_count, sizeof(*named), &bytes)) {
          free(pos);
          return NULL;
        }
        named = malloc(bytes);
        if (!named) {
          free(pos);
          return NULL;
        }
        for (uint32_t i = 0; i < type->payload.record.named_count; ++i) {
          named[i].name = type->payload.record.named[i].name;
          named[i].type = subst_inner(arena, type->payload.record.named[i].type,
                                      subst, subst_count, depth + 1u);
          if (!named[i].type) {
            free(named);
            free(pos);
            return NULL;
          }
          if (named[i].type != type->payload.record.named[i].type) changed = 1;
        }
      }
      if (!changed) {
        free(named);
        free(pos);
        return (KavakTypeInfo *)type;
      }
      KavakTypeInfo *out = kavak_ty_record(arena, pos,
                                           type->payload.record.positional_count,
                                           named,
                                           type->payload.record.named_count);
      free(named);
      free(pos);
      return out;
    }
    case KAVAK_TY_PARAM:
      return (KavakTypeInfo *)type;
  }
  return (KavakTypeInfo *)type;
}

KavakTypeInfo *kavak_ty_substitute(KavakTypeArena *arena,
                                   const KavakTypeInfo *type,
                                   const KavakTypeSubst *subst,
                                   const uint32_t subst_count) {
  return subst_inner(arena, type, subst, subst_count, 0u);
}

static void ts_appendf(TypeString *out, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out->buf && out->len < out->cap ? out->buf + out->len : NULL,
                          out->buf && out->len < out->cap ? out->cap - out->len : 0,
                          fmt, args);
  va_end(args);
  if (n > 0) out->len += (size_t)n;
}

static int type_string_seen(const TypeStringCtx *ctx, const KavakTypeInfo *type) {
  for (uint32_t i = 0; i < ctx->count; ++i) {
    if (ctx->seen[i] == type) return 1;
  }
  return 0;
}

static int type_string_push(TypeStringCtx *ctx, const KavakTypeInfo *type) {
  if (ctx->count >= KAVAK_TYPE_RECURSION_LIMIT) return 0;
  ctx->seen[ctx->count++] = type;
  return 1;
}

static void type_to_string_inner(const KavakTypeInfo *type, TypeString *out,
                                 TypeStringCtx *ctx) {
  if (!type) {
    ts_appendf(out, "<null>");
    return;
  }
  const char *builtin = builtin_name(type->kind);
  if (builtin) {
    ts_appendf(out, "%s", builtin);
    return;
  }
  if (type_string_seen(ctx, type)) {
    ts_appendf(out, "<cycle>");
    return;
  }
  if (!type_string_push(ctx, type)) {
    ts_appendf(out, "<depth>");
    return;
  }
  const uint32_t mark = ctx->count - 1u;

  switch (type->kind) {
    case KAVAK_TY_NULLABLE:
      type_to_string_inner(type->payload.nullable.inner, out, ctx);
      ts_appendf(out, "?");
      break;
    case KAVAK_TY_FUNCTION:
      if (type->flags & KAVAK_TY_FUNC_SUSPEND) ts_appendf(out, "suspend ");
      if (type->flags & KAVAK_TY_FUNC_ASYNC) ts_appendf(out, "async ");
      if (type->payload.function.receiver) {
        type_to_string_inner(type->payload.function.receiver, out, ctx);
        ts_appendf(out, ".");
      }
      ts_appendf(out, "(");
      for (uint32_t i = 0; i < type->payload.function.param_count; ++i) {
        if (i) ts_appendf(out, ", ");
        type_to_string_inner(type->payload.function.params[i], out, ctx);
      }
      ts_appendf(out, ") -> ");
      type_to_string_inner(type->payload.function.ret, out, ctx);
      break;
    case KAVAK_TY_NAMED:
      ts_appendf(out, "%s", type->payload.named.name ? type->payload.named.name : "<anon>");
      if (type->payload.named.arg_count != 0) {
        ts_appendf(out, "<");
        for (uint32_t i = 0; i < type->payload.named.arg_count; ++i) {
          if (i) ts_appendf(out, ", ");
          type_to_string_inner(type->payload.named.args[i], out, ctx);
        }
        ts_appendf(out, ">");
      }
      break;
    case KAVAK_TY_PARAM:
      if (type->payload.param.name) ts_appendf(out, "%s", type->payload.param.name);
      else ts_appendf(out, "T%u", type->payload.param.index);
      break;
    case KAVAK_TY_RECORD:
      ts_appendf(out, "(");
      for (uint32_t i = 0; i < type->payload.record.positional_count; ++i) {
        if (i) ts_appendf(out, ", ");
        type_to_string_inner(type->payload.record.positional[i], out, ctx);
      }
      for (uint32_t i = 0; i < type->payload.record.named_count; ++i) {
        if (i || type->payload.record.positional_count) ts_appendf(out, ", ");
        ts_appendf(out, "%s: ", type->payload.record.named[i].name
                              ? type->payload.record.named[i].name
                              : "<anon>");
        type_to_string_inner(type->payload.record.named[i].type, out, ctx);
      }
      ts_appendf(out, ")");
      break;
    default:
      ts_appendf(out, "<type:%u>", type->kind);
      break;
  }
  ctx->count = mark;
}

size_t kavak_ty_to_string(const KavakTypeInfo *type, char *buf, const size_t buf_len) {
  TypeString out = { .buf = buf, .cap = buf_len, .len = 0 };
  TypeStringCtx ctx;
  ctx.count = 0;  /* seen[] is written before read; no need to zero 4 KB */
  type_to_string_inner(type, &out, &ctx);
  if (buf_len != 0 && buf) {
    const size_t end = out.len < buf_len ? out.len : buf_len - 1u;
    buf[end] = '\0';
  }
  return out.len;
}

/* ── Type relations: nullability, assignability, join (LUB) ─────────────────
 *
 * Generic structural skeletons. Universal cases (identity, the NEVER bottom,
 * the ANY top, nullability) are decided here from the kernel's own builtin
 * vocabulary; nominal and numeric policy is delegated to the language's
 * optional descriptor hooks. The traversal is the kernel's, the policy is the
 * language's — so these stay language-agnostic. */

int kavak_ty_is_nullable(const KavakTypeInfo *type) {
  return type != NULL && type->kind == KAVAK_TY_NULLABLE;
}

const KavakTypeInfo *kavak_ty_unwrap_nullable(const KavakTypeInfo *type) {
  /* Strip every nullable layer to the non-null core. The counter bounds a
   * self-referential nullable (inner == self) instead of looping forever. */
  uint32_t guard = 0;
  while (type && type->kind == KAVAK_TY_NULLABLE &&
         guard++ < KAVAK_TYPE_RECURSION_LIMIT) {
    type = type->payload.nullable.inner;
  }
  return type;
}

static int ty_assignable_inner(const KavakTypeInfo *sub,
                               const KavakTypeInfo *super,
                               const KavakLanguage *lang,
                               const uint32_t depth) {
  if (!sub || !super) return 0;
  /* Conservative stop on a pathological / cyclic nullable chain. */
  if (depth >= KAVAK_TYPE_RECURSION_LIMIT) return 0;

  /* Identity (covers builtins and deep structural equality). */
  if (kavak_ty_equal_deep(sub, super)) return 1;

  /* Bottom: Never is assignable to anything. */
  if (sub->kind == KAVAK_TY_NEVER) return 1;

  /* Nullability:
   *   - assigning into a nullable slot (super = U?) succeeds iff the non-null
   *     core of sub is assignable to U;
   *   - a nullable sub is NOT assignable to a non-null super. */
  if (super->kind == KAVAK_TY_NULLABLE) {
    const KavakTypeInfo *u = super->payload.nullable.inner;
    const KavakTypeInfo *s = sub->kind == KAVAK_TY_NULLABLE
                           ? sub->payload.nullable.inner
                           : sub;
    return ty_assignable_inner(s, u, lang, depth + 1u);
  }
  if (sub->kind == KAVAK_TY_NULLABLE) return 0;

  /* Top: any non-null type is assignable to Any (super is non-null here). */
  if (super->kind == KAVAK_TY_ANY) return 1;

  /* Nominal / user relation: defer to the language. A descriptor may give the
   * full predicate (is_subtype) or just direct edges (supertypes) and let the
   * kernel walk the chain. */
  if (lang && lang->is_subtype) return lang->is_subtype(lang, sub, super) != 0;
  if (lang && lang->supertypes) return kavak_ty_conforms(sub, super, lang);

  return 0;
}

int kavak_ty_assignable(const KavakTypeInfo *sub, const KavakTypeInfo *super,
                        const KavakLanguage *lang) {
  return ty_assignable_inner(sub, super, lang, 0u);
}

const KavakTypeInfo *kavak_ty_numeric_common(const KavakTypeInfo *a,
                                             const KavakTypeInfo *b,
                                             const KavakLanguage *lang) {
  if (!a || !b || !lang || !lang->numeric_rank) return NULL;
  const uint32_t ra = lang->numeric_rank(lang, a->kind);
  const uint32_t rb = lang->numeric_rank(lang, b->kind);
  if (ra == 0 || rb == 0) return NULL;  /* not both numeric */
  return ra >= rb ? a : b;
}

/* Join of two already-unwrapped (non-null at the top) cores. */
static KavakTypeInfo *ty_join_cores(KavakTypeArena *arena,
                                    KavakTypeInfo *a, KavakTypeInfo *b,
                                    const KavakLanguage *lang) {
  /* Bottom absorption: Never widens to the other side. */
  if (a->kind == KAVAK_TY_NEVER) return b;
  if (b->kind == KAVAK_TY_NEVER) return a;

  /* Identity. */
  if (kavak_ty_equal_deep(a, b)) return a;

  /* Numeric widening (language policy). */
  const KavakTypeInfo *num = kavak_ty_numeric_common(a, b, lang);
  if (num) return (KavakTypeInfo *)num;

  /* Language join (nominal common supertype, etc.). */
  if (lang && lang->type_join) {
    KavakTypeInfo *joined = lang->type_join(arena, a, b, lang);
    if (joined) return joined;
  }

  /* Universal fallback: the top type. */
  return kavak_ty_builtin(arena, KAVAK_TY_ANY);
}

KavakTypeInfo *kavak_ty_join(KavakTypeArena *arena,
                             const KavakTypeInfo *a, const KavakTypeInfo *b,
                             const KavakLanguage *lang) {
  if (!a) return (KavakTypeInfo *)b;
  if (!b) return (KavakTypeInfo *)a;

  /* The result is nullable if either operand is. */
  const int nullable = kavak_ty_is_nullable(a) || kavak_ty_is_nullable(b);
  KavakTypeInfo *ca = (KavakTypeInfo *)kavak_ty_unwrap_nullable(a);
  KavakTypeInfo *cb = (KavakTypeInfo *)kavak_ty_unwrap_nullable(b);
  if (!ca) return (KavakTypeInfo *)b;
  if (!cb) return (KavakTypeInfo *)a;

  KavakTypeInfo *core = ty_join_cores(arena, ca, cb, lang);
  if (nullable && core && core->kind != KAVAK_TY_NULLABLE) {
    return kavak_ty_nullable(arena, core);
  }
  return core;
}

/* ── Structural unification ─────────────────────────────────────────────────
 *
 * Walks a generic `pattern` against a `concrete` type in lockstep, binding each
 * KAVAK_TY_PARAM leaf it meets to the matching concrete sub-type. Deliberately
 * lenient (the 80% solution, not full Hindley–Milner): shape mismatches are
 * skipped instead of failing, the first binding for a param wins, and there is
 * no occurs-check or variance. The recursion only descends the shapes kavak
 * already models, so nothing here is language-specific. */

static int subst_already_bound(const KavakTypeSubst *subst, const uint32_t count,
                               const KavakTypeInfo *param) {
  for (uint32_t i = 0; i < count; ++i) {
    if (subst[i].param == param) return 1;
  }
  return 0;
}

static uint32_t min_u32(const uint32_t a, const uint32_t b) {
  return a < b ? a : b;
}

static uint32_t ty_unify_inner(const KavakTypeInfo *pattern,
                               const KavakTypeInfo *concrete,
                               KavakTypeSubst *subst, const uint32_t subst_cap,
                               uint32_t *subst_count, const uint32_t depth) {
  if (!pattern || !concrete) return 0;
  if (depth >= KAVAK_TYPE_RECURSION_LIMIT) return 0;

  /* A param leaf binds to whatever it is matched against (first wins). */
  if (pattern->kind == KAVAK_TY_PARAM) {
    if (subst_already_bound(subst, *subst_count, pattern)) return 0;
    if (*subst_count >= subst_cap) return 0;
    subst[*subst_count].param = pattern;
    subst[*subst_count].replacement = (KavakTypeInfo *)concrete;
    ++(*subst_count);
    return 1;
  }

  /* Mismatched shapes: nothing to learn, skip (lenient). */
  if (pattern->kind != concrete->kind) return 0;

  uint32_t added = 0;
  switch (pattern->kind) {
    case KAVAK_TY_NULLABLE:
      added += ty_unify_inner(pattern->payload.nullable.inner,
                              concrete->payload.nullable.inner,
                              subst, subst_cap, subst_count, depth + 1u);
      break;
    case KAVAK_TY_FUNCTION: {
      if (pattern->payload.function.receiver &&
          concrete->payload.function.receiver) {
        added += ty_unify_inner(pattern->payload.function.receiver,
                                concrete->payload.function.receiver,
                                subst, subst_cap, subst_count, depth + 1u);
      }
      const uint32_t n = min_u32(pattern->payload.function.param_count,
                                 concrete->payload.function.param_count);
      for (uint32_t i = 0; i < n; ++i) {
        added += ty_unify_inner(pattern->payload.function.params[i],
                                concrete->payload.function.params[i],
                                subst, subst_cap, subst_count, depth + 1u);
      }
      added += ty_unify_inner(pattern->payload.function.ret,
                              concrete->payload.function.ret,
                              subst, subst_cap, subst_count, depth + 1u);
      break;
    }
    case KAVAK_TY_NAMED: {
      const uint32_t n = min_u32(pattern->payload.named.arg_count,
                                 concrete->payload.named.arg_count);
      for (uint32_t i = 0; i < n; ++i) {
        added += ty_unify_inner(pattern->payload.named.args[i],
                                concrete->payload.named.args[i],
                                subst, subst_cap, subst_count, depth + 1u);
      }
      break;
    }
    case KAVAK_TY_RECORD: {
      const uint32_t np = min_u32(pattern->payload.record.positional_count,
                                  concrete->payload.record.positional_count);
      for (uint32_t i = 0; i < np; ++i) {
        added += ty_unify_inner(pattern->payload.record.positional[i],
                                concrete->payload.record.positional[i],
                                subst, subst_cap, subst_count, depth + 1u);
      }
      const uint32_t nn = min_u32(pattern->payload.record.named_count,
                                  concrete->payload.record.named_count);
      for (uint32_t i = 0; i < nn; ++i) {
        added += ty_unify_inner(pattern->payload.record.named[i].type,
                                concrete->payload.record.named[i].type,
                                subst, subst_cap, subst_count, depth + 1u);
      }
      break;
    }
  }
  return added;
}

uint32_t kavak_ty_unify(const KavakTypeInfo *pattern,
                        const KavakTypeInfo *concrete,
                        KavakTypeSubst *subst, const uint32_t subst_cap,
                        uint32_t *subst_count) {
  if (!subst || !subst_count) return 0;
  return ty_unify_inner(pattern, concrete, subst, subst_cap, subst_count, 0u);
}

/* ── Strict unification ─────────────────────────────────────────────────────
 *
 * The sound counterpart of ty_unify_inner: shapes/arities/named-heads must line
 * up (else MISMATCH), a param forced to two unequal types is a CONFLICT, and a
 * param that would bind into a type mentioning itself is an OCCURS failure. */

static int type_mentions_param(const KavakTypeInfo *type,
                               const KavakTypeInfo *param, const uint32_t depth) {
  if (!type || depth >= KAVAK_TYPE_RECURSION_LIMIT) return 0;
  if (type == param) return 1;
  switch (type->kind) {
    case KAVAK_TY_NULLABLE:
      return type_mentions_param(type->payload.nullable.inner, param, depth + 1u);
    case KAVAK_TY_FUNCTION:
      if (type_mentions_param(type->payload.function.receiver, param, depth + 1u)) return 1;
      for (uint32_t i = 0; i < type->payload.function.param_count; ++i)
        if (type_mentions_param(type->payload.function.params[i], param, depth + 1u)) return 1;
      return type_mentions_param(type->payload.function.ret, param, depth + 1u);
    case KAVAK_TY_NAMED:
      for (uint32_t i = 0; i < type->payload.named.arg_count; ++i)
        if (type_mentions_param(type->payload.named.args[i], param, depth + 1u)) return 1;
      return 0;
    case KAVAK_TY_RECORD:
      for (uint32_t i = 0; i < type->payload.record.positional_count; ++i)
        if (type_mentions_param(type->payload.record.positional[i], param, depth + 1u)) return 1;
      for (uint32_t i = 0; i < type->payload.record.named_count; ++i)
        if (type_mentions_param(type->payload.record.named[i].type, param, depth + 1u)) return 1;
      return 0;
    default:
      return 0;
  }
}

static KavakUnifyResult ty_unify_strict_inner(const KavakTypeInfo *pattern,
                                              const KavakTypeInfo *concrete,
                                              KavakTypeSubst *subst,
                                              const uint32_t subst_cap,
                                              uint32_t *subst_count,
                                              const uint32_t depth) {
  if (depth >= KAVAK_TYPE_RECURSION_LIMIT) return KAVAK_UNIFY_CAP_EXCEEDED;
  if (!pattern || !concrete) return KAVAK_UNIFY_MISMATCH;

  if (pattern->kind == KAVAK_TY_PARAM) {
    for (uint32_t i = 0; i < *subst_count; ++i) {
      if (subst[i].param == pattern) {
        return kavak_ty_equal_deep(subst[i].replacement, concrete)
                 ? KAVAK_UNIFY_OK : KAVAK_UNIFY_CONFLICT;
      }
    }
    if (type_mentions_param(concrete, pattern, 0u)) return KAVAK_UNIFY_OCCURS;
    if (*subst_count >= subst_cap) return KAVAK_UNIFY_CAP_EXCEEDED;
    subst[*subst_count].param = pattern;
    subst[*subst_count].replacement = (KavakTypeInfo *)concrete;
    ++(*subst_count);
    return KAVAK_UNIFY_OK;
  }

  if (pattern->kind != concrete->kind) return KAVAK_UNIFY_MISMATCH;

  switch (pattern->kind) {
    case KAVAK_TY_NULLABLE:
      return ty_unify_strict_inner(pattern->payload.nullable.inner,
                                   concrete->payload.nullable.inner,
                                   subst, subst_cap, subst_count, depth + 1u);
    case KAVAK_TY_FUNCTION: {
      if (pattern->flags != concrete->flags ||
          !!pattern->payload.function.receiver != !!concrete->payload.function.receiver ||
          pattern->payload.function.param_count != concrete->payload.function.param_count) {
        return KAVAK_UNIFY_MISMATCH;
      }
      if (pattern->payload.function.receiver) {
        const KavakUnifyResult r = ty_unify_strict_inner(
            pattern->payload.function.receiver, concrete->payload.function.receiver,
            subst, subst_cap, subst_count, depth + 1u);
        if (r != KAVAK_UNIFY_OK) return r;
      }
      for (uint32_t i = 0; i < pattern->payload.function.param_count; ++i) {
        const KavakUnifyResult r = ty_unify_strict_inner(
            pattern->payload.function.params[i], concrete->payload.function.params[i],
            subst, subst_cap, subst_count, depth + 1u);
        if (r != KAVAK_UNIFY_OK) return r;
      }
      return ty_unify_strict_inner(pattern->payload.function.ret,
                                   concrete->payload.function.ret,
                                   subst, subst_cap, subst_count, depth + 1u);
    }
    case KAVAK_TY_NAMED: {
      if (!str_eq(pattern->payload.named.name, concrete->payload.named.name) ||
          pattern->payload.named.arg_count != concrete->payload.named.arg_count) {
        return KAVAK_UNIFY_MISMATCH;
      }
      for (uint32_t i = 0; i < pattern->payload.named.arg_count; ++i) {
        const KavakUnifyResult r = ty_unify_strict_inner(
            pattern->payload.named.args[i], concrete->payload.named.args[i],
            subst, subst_cap, subst_count, depth + 1u);
        if (r != KAVAK_UNIFY_OK) return r;
      }
      return KAVAK_UNIFY_OK;
    }
    case KAVAK_TY_RECORD: {
      if (pattern->payload.record.positional_count != concrete->payload.record.positional_count ||
          pattern->payload.record.named_count != concrete->payload.record.named_count) {
        return KAVAK_UNIFY_MISMATCH;
      }
      for (uint32_t i = 0; i < pattern->payload.record.positional_count; ++i) {
        const KavakUnifyResult r = ty_unify_strict_inner(
            pattern->payload.record.positional[i], concrete->payload.record.positional[i],
            subst, subst_cap, subst_count, depth + 1u);
        if (r != KAVAK_UNIFY_OK) return r;
      }
      for (uint32_t i = 0; i < pattern->payload.record.named_count; ++i) {
        if (!str_eq(pattern->payload.record.named[i].name,
                    concrete->payload.record.named[i].name)) {
          return KAVAK_UNIFY_MISMATCH;
        }
        const KavakUnifyResult r = ty_unify_strict_inner(
            pattern->payload.record.named[i].type, concrete->payload.record.named[i].type,
            subst, subst_cap, subst_count, depth + 1u);
        if (r != KAVAK_UNIFY_OK) return r;
      }
      return KAVAK_UNIFY_OK;
    }
    default:
      /* Equal-kind leaves (builtins, Void, …) unify with nothing to bind. */
      return KAVAK_UNIFY_OK;
  }
}

KavakUnifyResult kavak_ty_unify_strict(const KavakTypeInfo *pattern,
                                       const KavakTypeInfo *concrete,
                                       KavakTypeSubst *subst,
                                       const uint32_t subst_cap,
                                       uint32_t *subst_count) {
  if (!subst || !subst_count) return KAVAK_UNIFY_MISMATCH;
  return ty_unify_strict_inner(pattern, concrete, subst, subst_cap, subst_count, 0u);
}

/* ── Nominal conformance: supertype-chain walks ─────────────────────────────
 *
 * The language exposes only LOCAL edges of its type graph (a node's direct
 * supertypes, its own members, a sealed type's cases); the kernel owns the
 * transitive, cycle-guarded walks. Nothing here knows a specific language's
 * hierarchy — it only follows descriptor-supplied edges. */

#define KAVAK_TY_SUPERS_CAP    16u   /* stack scratch for the common case    */
#define KAVAK_TY_VARIANTS_CAP  64u   /* cases examined for exhaustiveness    */

static int visited_seen(const KavakTypeInfo *const *visited,
                        const uint32_t count, const KavakTypeInfo *node) {
  for (uint32_t i = 0; i < count; ++i) {
    if (visited[i] == node) return 1;
  }
  return 0;
}

/* Fetch ALL direct supertypes of `node`, never just the first 16: walking a
 * truncated edge set would make conformance/member-lookup silently wrong for a
 * node with many supertypes. The hook returns the total even when it writes
 * fewer, so the common case (total <= stack_cap) costs exactly one hook call
 * and no allocation; only a node with more supertypes than the stack holds
 * triggers a heap buffer and a second fill. On OOM we degrade to the already-
 * filled stack scratch (truncates, but never crashes). *out is the buffer to
 * iterate; the caller frees it with `if (*out != stack_buf) free(...)`. */
static uint32_t fetch_supers(const KavakLanguage *lang, const KavakTypeInfo *node,
                             const KavakTypeInfo **stack_buf, uint32_t stack_cap,
                             const KavakTypeInfo ***out) {
  uint32_t n = lang->supertypes(lang, node, stack_buf, stack_cap);
  if (n <= stack_cap) { *out = stack_buf; return n; }

  const uint32_t total = n;
  const KavakTypeInfo **heap = malloc((size_t)total * sizeof(*heap));
  if (!heap) { *out = stack_buf; return stack_cap; }
  n = lang->supertypes(lang, node, heap, total);
  *out = heap;
  return n < total ? n : total;
}

static int conforms_dfs(const KavakTypeInfo *sub, const KavakTypeInfo *super,
                        const KavakLanguage *lang,
                        const KavakTypeInfo **visited, uint32_t *vcount,
                        const uint32_t depth) {
  if (!sub) return 0;
  if (kavak_ty_equal_deep(sub, super)) return 1;
  if (depth >= KAVAK_TYPE_RECURSION_LIMIT) return 0;
  if (visited_seen(visited, *vcount, sub)) return 0;
  if (*vcount >= KAVAK_TYPE_RECURSION_LIMIT) return 0;
  visited[(*vcount)++] = sub;

  const KavakTypeInfo *stack_buf[KAVAK_TY_SUPERS_CAP];
  const KavakTypeInfo **supers;
  const uint32_t m = fetch_supers(lang, sub, stack_buf, KAVAK_TY_SUPERS_CAP, &supers);
  int found = 0;
  for (uint32_t i = 0; i < m; ++i) {
    if (conforms_dfs(supers[i], super, lang, visited, vcount, depth + 1u)) { found = 1; break; }
  }
  if (supers != stack_buf) free((void *)supers);
  return found;
}

int kavak_ty_conforms(const KavakTypeInfo *sub, const KavakTypeInfo *super,
                      const KavakLanguage *lang) {
  if (!sub || !super) return 0;
  if (kavak_ty_equal_deep(sub, super)) return 1;
  if (!lang || !lang->supertypes) return 0;
  const KavakTypeInfo *visited[KAVAK_TYPE_RECURSION_LIMIT];
  uint32_t vcount = 0;
  return conforms_dfs(sub, super, lang, visited, &vcount, 0u);
}

static const KavakTypeInfo *find_member_dfs(const KavakTypeInfo *type,
                                            const char *name,
                                            const uint32_t name_len,
                                            const KavakLanguage *lang,
                                            const KavakTypeInfo **visited,
                                            uint32_t *vcount,
                                            const uint32_t depth) {
  if (!type || depth >= KAVAK_TYPE_RECURSION_LIMIT) return NULL;
  if (visited_seen(visited, *vcount, type)) return NULL;
  if (*vcount >= KAVAK_TYPE_RECURSION_LIMIT) return NULL;
  visited[(*vcount)++] = type;

  if (lang->find_own_member) {
    const KavakTypeInfo *member = lang->find_own_member(lang, type, name, name_len);
    if (member) return member;
  }
  if (!lang->supertypes) return NULL;

  const KavakTypeInfo *stack_buf[KAVAK_TY_SUPERS_CAP];
  const KavakTypeInfo **supers;
  const uint32_t m = fetch_supers(lang, type, stack_buf, KAVAK_TY_SUPERS_CAP, &supers);
  const KavakTypeInfo *found = NULL;
  for (uint32_t i = 0; i < m; ++i) {
    found = find_member_dfs(supers[i], name, name_len, lang,
                            visited, vcount, depth + 1u);
    if (found) break;
  }
  if (supers != stack_buf) free((void *)supers);
  return found;
}

const KavakTypeInfo *kavak_ty_find_member(const KavakTypeInfo *type,
                                          const char *name,
                                          const uint32_t name_len,
                                          const KavakLanguage *lang) {
  if (!type || !name || !lang) return NULL;
  const uint32_t len = name_len ? name_len : (uint32_t)strlen(name);
  const KavakTypeInfo *visited[KAVAK_TYPE_RECURSION_LIMIT];
  uint32_t vcount = 0;
  return find_member_dfs(type, name, len, lang, visited, &vcount, 0u);
}

int kavak_ty_exhaustive(const KavakTypeInfo *sealed,
                        const KavakTypeInfo *const *covered,
                        const uint32_t count,
                        const KavakTypeInfo **missing,
                        const KavakLanguage *lang) {
  if (missing) *missing = NULL;
  if (!sealed || !lang || !lang->variants) return 0;

  const KavakTypeInfo *vars[KAVAK_TY_VARIANTS_CAP];
  const uint32_t n = lang->variants(lang, sealed, vars, KAVAK_TY_VARIANTS_CAP);
  if (n == 0 || n > KAVAK_TY_VARIANTS_CAP) return 0;  /* unknown / too many */

  for (uint32_t i = 0; i < n; ++i) {
    int covered_i = 0;
    for (uint32_t j = 0; j < count; ++j) {
      /* A case is covered when it conforms to a matched type — so matching the
       * base (or the case itself) counts. */
      if (covered[j] && kavak_ty_conforms(vars[i], covered[j], lang)) {
        covered_i = 1;
        break;
      }
    }
    if (!covered_i) {
      if (missing) *missing = vars[i];
      return 0;
    }
  }
  return 1;
}
