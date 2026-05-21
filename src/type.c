// SPDX-License-Identifier: MIT
/**
 * @file src/type.c
 * @brief Type-system toolkit: TypeInfo arena, constructors, helpers.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define KAVAK_TYPE_ARENA_CHUNK_SIZE 256u
#define KAVAK_TYPE_RECURSION_LIMIT 128u

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
  if (arena->aux) kavak_arena_init(arena->aux, 4096);
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
  TypeEqualCtx ctx = {0};
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
  TypeEqualCtx ctx = {0};
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

static KavakTypeInfo **subst_type_array(KavakTypeArena *arena,
                                        KavakTypeInfo *const *items,
                                        const uint32_t count,
                                        const KavakTypeSubst *subst,
                                        const uint32_t subst_count,
                                        int *changed) {
  if (count == 0) return NULL;
  size_t bytes = 0;
  if (!array_bytes(count, sizeof(KavakTypeInfo *), &bytes)) return NULL;
  KavakTypeInfo **copy = malloc(bytes);
  if (!copy) return NULL;
  for (uint32_t i = 0; i < count; ++i) {
    copy[i] = kavak_ty_substitute(arena, items[i], subst, subst_count);
    if (!copy[i]) {
      free(copy);
      return NULL;
    }
    if (copy[i] != items[i]) *changed = 1;
  }
  return copy;
}

KavakTypeInfo *kavak_ty_substitute(KavakTypeArena *arena,
                                   const KavakTypeInfo *type,
                                   const KavakTypeSubst *subst,
                                   const uint32_t subst_count) {
  if (!type) return NULL;
  KavakTypeInfo *replacement = subst_lookup(type, subst, subst_count);
  if (replacement) return replacement;
  if (subst_count == 0 || is_builtin_kind(type->kind)) {
    return (KavakTypeInfo *)type;
  }

  switch (type->kind) {
    case KAVAK_TY_NULLABLE: {
      KavakTypeInfo *inner = kavak_ty_substitute(arena, type->payload.nullable.inner,
                                                 subst, subst_count);
      if (!inner) return NULL;
      return inner == type->payload.nullable.inner
           ? (KavakTypeInfo *)type
           : kavak_ty_nullable(arena, inner);
    }
    case KAVAK_TY_FUNCTION: {
      int changed = 0;
      KavakTypeInfo *receiver = NULL;
      if (type->payload.function.receiver) {
        receiver = kavak_ty_substitute(arena, type->payload.function.receiver,
                                       subst, subst_count);
        if (!receiver) return NULL;
        if (receiver != type->payload.function.receiver) changed = 1;
      }
      KavakTypeInfo *ret = kavak_ty_substitute(arena, type->payload.function.ret,
                                               subst, subst_count);
      if (!ret) return NULL;
      if (ret != type->payload.function.ret) changed = 1;
      KavakTypeInfo **params = subst_type_array(arena, type->payload.function.params,
                                                type->payload.function.param_count,
                                                subst, subst_count, &changed);
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
                                              subst, subst_count, &changed);
      if (type->payload.named.arg_count != 0 && !args) return NULL;
      if (!changed) {
        free(args);
        return (KavakTypeInfo *)type;
      }
      KavakTypeInfo *out = kavak_ty_named(arena, type->payload.named.name, args,
                                          type->payload.named.arg_count,
                                          type->payload.named.decl);
      free(args);
      return out;
    }
    case KAVAK_TY_RECORD: {
      int changed = 0;
      KavakTypeInfo **pos = subst_type_array(arena, type->payload.record.positional,
                                             type->payload.record.positional_count,
                                             subst, subst_count, &changed);
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
          named[i].type = kavak_ty_substitute(arena, type->payload.record.named[i].type,
                                              subst, subst_count);
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
  TypeStringCtx ctx = {0};
  type_to_string_inner(type, &out, &ctx);
  if (buf_len != 0 && buf) {
    const size_t end = out.len < buf_len ? out.len : buf_len - 1u;
    buf[end] = '\0';
  }
  return out.len;
}
