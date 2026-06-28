// SPDX-License-Identifier: MIT
/**
 * @file tests/test_type.c
 * @brief Type-system toolkit tests.
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

static int type_string_is(const KavakTypeInfo *type, const char *want) {
  char buf[256];
  const size_t n = kavak_ty_to_string(type, buf, sizeof(buf));
  return n == strlen(want) && strcmp(buf, want) == 0;
}

static int test_builtins(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *int_ty2 = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *never_ty = kavak_ty_builtin(&arena, KAVAK_TY_NEVER);

  ASSERT(KAVAK_TY_USER_BASE == 256u, "user type namespace starts at 256");
  ASSERT(KAVAK_TY_BUILTIN_COUNT == 17u, "builtin singleton table includes string");
  ASSERT(int_ty && int_ty->kind == KAVAK_TY_INT, "int builtin exists");
  ASSERT(int_ty == int_ty2, "builtins are arena-local singletons");
  ASSERT(never_ty && type_string_is(never_ty, "Never"), "never string");
  ASSERT(kavak_ty_builtin(&arena, KAVAK_TY_NULLABLE) == NULL,
         "constructed kinds are not builtins");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_constructors_and_to_string(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *bool_ty = kavak_ty_builtin(&arena, KAVAK_TY_BOOL);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeInfo *void_ty = kavak_ty_builtin(&arena, KAVAK_TY_VOID);

  KavakTypeInfo *nullable = kavak_ty_nullable(&arena, int_ty);
  ASSERT(nullable && nullable->payload.nullable.inner == int_ty, "nullable wraps inner");
  ASSERT(type_string_is(nullable, "Int?"), "nullable string");

  KavakTypeInfo *params[] = { int_ty, bool_ty };
  KavakTypeInfo *fn = kavak_ty_function(&arena, string_ty, params, 2, void_ty,
                                        KAVAK_TY_FUNC_SUSPEND);
  ASSERT(fn && fn->payload.function.param_count == 2, "function payload");
  ASSERT(type_string_is(fn, "suspend String.(Int, Bool) -> Void"), "function string");

  KavakTypeInfo *box_args[] = { int_ty };
  KavakTypeInfo *box = kavak_ty_named(&arena, "Box", box_args, 1, NULL);
  ASSERT(box && type_string_is(box, "Box<Int>"), "named generic string");

  KavakRecordField fields[] = { { "name", string_ty } };
  KavakTypeInfo *positional[] = { int_ty };
  KavakTypeInfo *record = kavak_ty_record(&arena, positional, 1, fields, 1);
  ASSERT(record && type_string_is(record, "(Int, name: String)"), "record string");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_record_rejects_malformed_fields(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *positional[] = { NULL };
  KavakRecordField no_name[] = { { NULL, int_ty } };
  KavakRecordField no_type[] = { { "x", NULL } };

  ASSERT(kavak_ty_record(&arena, positional, 1, NULL, 0) == NULL,
         "record rejects null positional type");
  ASSERT(kavak_ty_record(&arena, NULL, 0, no_name, 1) == NULL,
         "record rejects null field name");
  ASSERT(kavak_ty_record(&arena, NULL, 0, no_type, 1) == NULL,
         "record rejects null field type");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_equality(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);

  KavakTypeInfo *box_int_args[] = { int_ty };
  KavakTypeInfo *box_string_args[] = { string_ty };
  KavakTypeInfo *box_int = kavak_ty_named(&arena, "Box", box_int_args, 1, NULL);
  KavakTypeInfo *box_string = kavak_ty_named(&arena, "Box", box_string_args, 1, NULL);
  KavakTypeInfo *box_int2 = kavak_ty_named(&arena, "Box", box_int_args, 1, NULL);

  ASSERT(kavak_ty_equal_nominal(box_int, box_string),
         "nominal equality ignores generic argument contents");
  ASSERT(!kavak_ty_equal_deep(box_int, box_string),
         "deep equality checks generic argument contents");
  ASSERT(kavak_ty_equal_deep(box_int, box_int2), "deep equality accepts same args");

  KavakTypeInfo *nullable_int = kavak_ty_nullable(&arena, int_ty);
  KavakTypeInfo *nullable_string = kavak_ty_nullable(&arena, string_ty);
  ASSERT(!kavak_ty_equal_nominal(nullable_int, nullable_string),
         "nullable nominal equality checks inner nominal type");

  KavakRecordField field_a[] = { { "x", int_ty } };
  KavakRecordField field_b[] = { { "x", string_ty } };
  KavakTypeInfo *record_a = kavak_ty_record(&arena, NULL, 0, field_a, 1);
  KavakTypeInfo *record_b = kavak_ty_record(&arena, NULL, 0, field_b, 1);
  ASSERT(kavak_ty_equal_nominal(record_a, record_b),
         "record nominal equality compares field shape");
  ASSERT(!kavak_ty_equal_deep(record_a, record_b),
         "record deep equality compares field types");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_cycle_guards(void) {
  KavakTypeInfo a = { .kind = KAVAK_TY_NULLABLE };
  KavakTypeInfo b = { .kind = KAVAK_TY_NULLABLE };
  a.payload.nullable.inner = &a;
  b.payload.nullable.inner = &b;

  ASSERT(kavak_ty_equal_deep(&a, &b), "deep equality handles matching cycles");

  char buf[64];
  const size_t n = kavak_ty_to_string(&a, buf, sizeof(buf));
  ASSERT(n > 0, "cyclic type string returns length");
  ASSERT(strstr(buf, "<cycle>") != NULL, "cyclic type string marks cycle");
  return 0;
}

static int test_wide_type_graph_does_not_trip_depth_guard(void) {
  enum { FIELD_COUNT = 160 };
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *left[FIELD_COUNT];
  KavakTypeInfo *right[FIELD_COUNT];
  for (uint32_t i = 0; i < FIELD_COUNT; ++i) {
    left[i] = kavak_ty_nullable(&arena, int_ty);
    right[i] = kavak_ty_nullable(&arena, int_ty);
  }

  KavakTypeInfo *left_record = kavak_ty_record(&arena, left, FIELD_COUNT, NULL, 0);
  KavakTypeInfo *right_record = kavak_ty_record(&arena, right, FIELD_COUNT, NULL, 0);
  ASSERT(kavak_ty_equal_deep(left_record, right_record),
         "wide shallow type graph does not hit depth guard");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_substitution(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *t = kavak_ty_param(&arena, "T", 0, NULL);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *list_args[] = { t };
  KavakTypeInfo *list_t = kavak_ty_named(&arena, "List", list_args, 1, NULL);

  KavakTypeSubst subst[] = { { t, string_ty } };
  KavakTypeInfo *list_string = kavak_ty_substitute(&arena, list_t, subst, 1);
  ASSERT(list_string && list_string != list_t, "substitution returns changed type");
  ASSERT(type_string_is(list_string, "List<String>"), "substituted named string");

  KavakTypeInfo *list_int_args[] = { int_ty };
  KavakTypeInfo *list_int = kavak_ty_named(&arena, "List", list_int_args, 1, NULL);
  ASSERT(kavak_ty_substitute(&arena, list_int, subst, 1) == list_int,
         "unchanged substitution reuses original type");

  KavakRecordField fields[] = { { "value", t } };
  KavakTypeInfo *record_t = kavak_ty_record(&arena, NULL, 0, fields, 1);
  KavakTypeInfo *record_string = kavak_ty_substitute(&arena, record_t, subst, 1);
  ASSERT(record_string && type_string_is(record_string, "(value: String)"),
         "substituted record string");

  kavak_type_arena_free(&arena);
  return 0;
}

/* Regression: a self-referential type must not send kavak_ty_substitute into
 * unbounded recursion. The depth guard stops and returns the original node. */
static int test_substitute_cyclic_type_terminates(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *t = kavak_ty_param(&arena, "T", 0, NULL);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeSubst subst[] = { { t, string_ty } };

  KavakTypeInfo cyclic = { .kind = KAVAK_TY_NULLABLE };
  cyclic.payload.nullable.inner = &cyclic;  /* nullable that wraps itself */

  /* Before the depth guard this recursed forever and overflowed the stack. */
  KavakTypeInfo *out = kavak_ty_substitute(&arena, &cyclic, subst, 1);
  ASSERT(out == &cyclic, "cyclic substitute terminates, returns the original");

  kavak_type_arena_free(&arena);
  return 0;
}

/* Regression: two structurally-identical types nested deeper than the old
 * 128-pair limit must still compare equal (the limit was a false-negative). */
static int test_deep_acyclic_equality(void) {
  enum { DEPTH = 200 };  /* > old 128 limit, < the 512 soft cap */
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *a = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *b = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  for (int i = 0; i < DEPTH; ++i) {
    a = kavak_ty_nullable(&arena, a);
    b = kavak_ty_nullable(&arena, b);
  }
  ASSERT(a != b, "the two deep chains are pointer-distinct");
  ASSERT(kavak_ty_equal_deep(a, b),
         "200-deep identical types compare equal (was false-negative at 128)");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_session_type_arena(void) {
  static const KavakLanguage TEST_LANG = {
    .name = "TypeArenaTest",
    .file_extension = ".typearena",
    .version = 1,
  };
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");

  KavakTypeArena *arena = kavak_session_type_arena(session);
  ASSERT(arena != NULL, "session exposes type arena");
  KavakTypeInfo *int_ty = kavak_ty_builtin(arena, KAVAK_TY_INT);
  KavakTypeInfo *nullable = kavak_ty_nullable(arena, int_ty);
  ASSERT(nullable && type_string_is(nullable, "Int?"), "session arena allocates types");

  ASSERT(kavak_session_type_arena(NULL) == NULL, "null session accessor");
  kavak_session_free(session);
  return 0;
}

static int test_session_reset_types(void) {
  static const KavakLanguage TEST_LANG = {
    .name = "ResetTypes",
    .file_extension = ".reset",
    .version = 1,
  };
  KavakSession *session = kavak_session_new(&TEST_LANG);
  ASSERT(session != NULL, "session new");
  KavakTypeArena *arena = kavak_session_type_arena(session);

  /* Intern enough types to allocate at least one chunk. */
  for (uint32_t i = 0; i < 128u; ++i) {
    ASSERT(kavak_ty_named(arena, "T", NULL, 0, NULL) != NULL, "intern grows arena");
  }
  ASSERT(arena->head != NULL, "arena has chunks before reset");

  ASSERT(kavak_session_reset_types(session) == 0, "reset ok");
  ASSERT(arena->head == NULL && arena->tail == NULL, "chunks released by reset");

  /* The arena stays usable: builtins and fresh interning (which exercises the
   * rebuilt aux store via kavak_ty_nullable) still work. */
  KavakTypeInfo *int_ty = kavak_ty_builtin(arena, KAVAK_TY_INT);
  ASSERT(int_ty && type_string_is(int_ty, "Int"), "builtins usable after reset");
  KavakTypeInfo *nullable = kavak_ty_nullable(arena, int_ty);
  ASSERT(nullable && type_string_is(nullable, "Int?"),
         "interning works after reset (aux store rebuilt)");
  ASSERT(arena->head != NULL, "arena re-grows after reset");

  ASSERT(kavak_session_reset_types(NULL) == -1, "null session => -1");

  kavak_session_free(session);
  return 0;
}

/* ── A tiny mock language exercising the type-relation hooks ─────────────────
 * It supplies all three optional policy hooks so the same kernel helpers behave
 * as a real (non-Kotlin) frontend would, proving the seam is generic:
 *   - numeric ladder: Int(1) < Long(2) < Double(3)
 *   - nominal hierarchy: Cat <: Animal (by name)
 *   - type_join: Cat ⊔ Animal => Animal */

static uint32_t mock_numeric_rank(const KavakLanguage *lang, uint32_t kind) {
  (void)lang;
  switch (kind) {
    case KAVAK_TY_INT:    return 1;
    case KAVAK_TY_LONG:   return 2;
    case KAVAK_TY_DOUBLE: return 3;
    default:              return 0;
  }
}

static int mock_is_subtype(const KavakLanguage *lang, const KavakTypeInfo *sub,
                           const KavakTypeInfo *super) {
  (void)lang;
  if (sub->kind == KAVAK_TY_NAMED && super->kind == KAVAK_TY_NAMED) {
    return strcmp(sub->payload.named.name, "Cat") == 0 &&
           strcmp(super->payload.named.name, "Animal") == 0;
  }
  return 0;
}

static KavakTypeInfo *mock_type_join(KavakTypeArena *arena,
                                     const KavakTypeInfo *a,
                                     const KavakTypeInfo *b,
                                     const KavakLanguage *lang) {
  (void)arena;
  (void)lang;
  if (a->kind == KAVAK_TY_NAMED && b->kind == KAVAK_TY_NAMED) {
    const char *na = a->payload.named.name;
    const char *nb = b->payload.named.name;
    if (strcmp(na, "Cat") == 0 && strcmp(nb, "Animal") == 0) return (KavakTypeInfo *)b;
    if (strcmp(na, "Animal") == 0 && strcmp(nb, "Cat") == 0) return (KavakTypeInfo *)a;
  }
  return NULL;
}

static const KavakLanguage MOCK_LANG = {
  .name           = "Mock",
  .file_extension = ".mock",
  .version        = 1,
  .is_subtype     = mock_is_subtype,
  .numeric_rank   = mock_numeric_rank,
  .type_join      = mock_type_join,
};

static int test_nullable_helpers(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *nint = kavak_ty_nullable(&arena, int_ty);
  KavakTypeInfo *nnint = kavak_ty_nullable(&arena, nint);  /* Int?? */

  ASSERT(!kavak_ty_is_nullable(int_ty), "Int is not nullable");
  ASSERT(kavak_ty_is_nullable(nint), "Int? is nullable");
  ASSERT(!kavak_ty_is_nullable(NULL), "null is not nullable");

  ASSERT(kavak_ty_unwrap_nullable(int_ty) == int_ty, "unwrap of non-null is identity");
  ASSERT(kavak_ty_unwrap_nullable(nint) == int_ty, "unwrap Int? -> Int");
  ASSERT(kavak_ty_unwrap_nullable(nnint) == int_ty, "unwrap Int?? strips all layers");
  ASSERT(kavak_ty_unwrap_nullable(NULL) == NULL, "unwrap null -> null");

  KavakTypeInfo cyclic = { .kind = KAVAK_TY_NULLABLE };
  cyclic.payload.nullable.inner = &cyclic;
  ASSERT(kavak_ty_unwrap_nullable(&cyclic) == &cyclic,
         "self-cyclic nullable unwrap terminates");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_assignability_structural(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *long_ty = kavak_ty_builtin(&arena, KAVAK_TY_LONG);
  KavakTypeInfo *never = kavak_ty_builtin(&arena, KAVAK_TY_NEVER);
  KavakTypeInfo *any = kavak_ty_builtin(&arena, KAVAK_TY_ANY);
  KavakTypeInfo *nint = kavak_ty_nullable(&arena, int_ty);
  KavakTypeInfo *nany = kavak_ty_nullable(&arena, any);

  ASSERT(kavak_ty_assignable(int_ty, int_ty, NULL), "identity is assignable");
  ASSERT(kavak_ty_assignable(never, int_ty, NULL), "Never <: Int (bottom)");
  ASSERT(kavak_ty_assignable(never, nint, NULL), "Never <: Int?");
  ASSERT(kavak_ty_assignable(int_ty, any, NULL), "Int <: Any (top)");
  ASSERT(kavak_ty_assignable(int_ty, nint, NULL), "Int <: Int? (nullable widen)");
  ASSERT(!kavak_ty_assignable(nint, int_ty, NULL), "Int? not <: Int");
  ASSERT(kavak_ty_assignable(nint, nint, NULL), "Int? <: Int?");
  ASSERT(!kavak_ty_assignable(nint, any, NULL), "Int? not <: Any (Any is non-null top)");
  ASSERT(kavak_ty_assignable(nint, nany, NULL), "Int? <: Any?");
  ASSERT(!kavak_ty_assignable(int_ty, long_ty, NULL),
         "Int not <: Long with no nominal hook");
  ASSERT(!kavak_ty_assignable(NULL, int_ty, NULL), "null sub not assignable");
  ASSERT(!kavak_ty_assignable(int_ty, NULL, NULL), "null super not assignable");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_assignability_nominal_hook(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *cat = kavak_ty_named(&arena, "Cat", NULL, 0, NULL);
  KavakTypeInfo *animal = kavak_ty_named(&arena, "Animal", NULL, 0, NULL);
  KavakTypeInfo *ncat = kavak_ty_nullable(&arena, cat);
  KavakTypeInfo *nanimal = kavak_ty_nullable(&arena, animal);

  ASSERT(!kavak_ty_assignable(cat, animal, NULL), "no hook => Cat not <: Animal");
  ASSERT(kavak_ty_assignable(cat, animal, &MOCK_LANG), "hook: Cat <: Animal");
  ASSERT(!kavak_ty_assignable(animal, cat, &MOCK_LANG), "hook: Animal not <: Cat");
  ASSERT(kavak_ty_assignable(cat, nanimal, &MOCK_LANG), "Cat <: Animal? (nominal + nullable)");
  ASSERT(!kavak_ty_assignable(ncat, animal, &MOCK_LANG), "Cat? not <: Animal");
  ASSERT(kavak_ty_assignable(ncat, nanimal, &MOCK_LANG), "Cat? <: Animal?");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_numeric_common(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *long_ty = kavak_ty_builtin(&arena, KAVAK_TY_LONG);
  KavakTypeInfo *double_ty = kavak_ty_builtin(&arena, KAVAK_TY_DOUBLE);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);

  ASSERT(kavak_ty_numeric_common(int_ty, long_ty, NULL) == NULL,
         "no lang => no numeric common");
  ASSERT(kavak_ty_numeric_common(int_ty, long_ty, &MOCK_LANG) == long_ty,
         "Int,Long => Long");
  ASSERT(kavak_ty_numeric_common(long_ty, int_ty, &MOCK_LANG) == long_ty,
         "Long,Int => Long (order-independent)");
  ASSERT(kavak_ty_numeric_common(int_ty, double_ty, &MOCK_LANG) == double_ty,
         "Int,Double => Double");
  ASSERT(kavak_ty_numeric_common(int_ty, int_ty, &MOCK_LANG) == int_ty, "Int,Int => Int");
  ASSERT(kavak_ty_numeric_common(int_ty, string_ty, &MOCK_LANG) == NULL,
         "String is not numeric => NULL");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_join(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *long_ty = kavak_ty_builtin(&arena, KAVAK_TY_LONG);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeInfo *never = kavak_ty_builtin(&arena, KAVAK_TY_NEVER);
  KavakTypeInfo *nint = kavak_ty_nullable(&arena, int_ty);

  ASSERT(kavak_ty_join(&arena, int_ty, int_ty, NULL) == int_ty, "join(Int,Int)=Int");
  ASSERT(kavak_ty_join(&arena, never, int_ty, NULL) == int_ty, "join(Never,Int)=Int");
  ASSERT(kavak_ty_join(&arena, int_ty, never, NULL) == int_ty, "join(Int,Never)=Int");

  ASSERT(type_string_is(kavak_ty_join(&arena, int_ty, nint, NULL), "Int?"),
         "join(Int,Int?)=Int? (nullability union)");
  ASSERT(type_string_is(kavak_ty_join(&arena, never, nint, NULL), "Int?"),
         "join(Never,Int?)=Int?");

  KavakTypeInfo *j_any = kavak_ty_join(&arena, int_ty, string_ty, NULL);
  ASSERT(j_any && j_any->kind == KAVAK_TY_ANY,
         "join(Int,String) with no hooks = Any (fallback)");

  ASSERT(kavak_ty_join(&arena, int_ty, long_ty, &MOCK_LANG) == long_ty,
         "join(Int,Long) with rank ladder = Long");

  KavakTypeInfo *cat = kavak_ty_named(&arena, "Cat", NULL, 0, NULL);
  KavakTypeInfo *animal = kavak_ty_named(&arena, "Animal", NULL, 0, NULL);
  ASSERT(kavak_ty_join(&arena, cat, animal, &MOCK_LANG) == animal,
         "join(Cat,Animal) via type_join = Animal");

  KavakTypeInfo *nanimal = kavak_ty_nullable(&arena, animal);
  ASSERT(type_string_is(kavak_ty_join(&arena, cat, nanimal, &MOCK_LANG), "Animal?"),
         "join(Cat,Animal?)=Animal? (nominal join + nullability)");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_unify(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeInfo *t = kavak_ty_param(&arena, "T", 0, NULL);

  /* List<T> ~ List<Int>  =>  T := Int */
  KavakTypeInfo *list_t_args[] = { t };
  KavakTypeInfo *list_int_args[] = { int_ty };
  KavakTypeInfo *list_t = kavak_ty_named(&arena, "List", list_t_args, 1, NULL);
  KavakTypeInfo *list_int = kavak_ty_named(&arena, "List", list_int_args, 1, NULL);

  KavakTypeSubst subst[8];
  uint32_t count = 0;
  uint32_t added = kavak_ty_unify(list_t, list_int, subst, 8, &count);
  ASSERT(added == 1 && count == 1, "List<T> ~ List<Int> adds one binding");
  ASSERT(subst[0].param == t && subst[0].replacement == int_ty, "binding is T := Int");

  /* the learned binding instantiates a generic return type */
  ASSERT(kavak_ty_substitute(&arena, t, subst, count) == int_ty,
         "substitute applies the unified binding");

  /* first-binding-wins across repeated occurrences: (T) -> T ~ (Int) -> Int */
  KavakTypeInfo *pat_params[] = { t };
  KavakTypeInfo *arg_params[] = { int_ty };
  KavakTypeInfo *fn_pat = kavak_ty_function(&arena, NULL, pat_params, 1, t, 0);
  KavakTypeInfo *fn_arg = kavak_ty_function(&arena, NULL, arg_params, 1, int_ty, 0);
  count = 0;
  added = kavak_ty_unify(fn_pat, fn_arg, subst, 8, &count);
  ASSERT(added == 1 && count == 1, "T bound once across both param and return");

  /* mismatched shapes learn nothing (lenient, no failure) */
  count = 0;
  added = kavak_ty_unify(list_t, int_ty, subst, 8, &count);
  ASSERT(added == 0 && count == 0, "mismatched shapes bind nothing");

  /* subst_cap bounds the bindings collected: Pair<T, U> into cap 1 */
  KavakTypeInfo *u = kavak_ty_param(&arena, "U", 1, NULL);
  KavakTypeInfo *pair_pat_args[] = { t, u };
  KavakTypeInfo *pair_arg_args[] = { int_ty, string_ty };
  KavakTypeInfo *pair_pat = kavak_ty_named(&arena, "Pair", pair_pat_args, 2, NULL);
  KavakTypeInfo *pair_arg = kavak_ty_named(&arena, "Pair", pair_arg_args, 2, NULL);
  count = 0;
  added = kavak_ty_unify(pair_pat, pair_arg, subst, 1, &count);
  ASSERT(added == 1 && count == 1, "subst_cap caps the bindings collected");

  kavak_type_arena_free(&arena);
  return 0;
}

static int test_unify_strict(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  KavakTypeInfo *int_ty = kavak_ty_builtin(&arena, KAVAK_TY_INT);
  KavakTypeInfo *string_ty = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  KavakTypeInfo *t = kavak_ty_param(&arena, "T", 0, NULL);
  KavakTypeInfo *u = kavak_ty_param(&arena, "U", 1, NULL);

  KavakTypeInfo *list_t_args[] = { t };
  KavakTypeInfo *list_int_args[] = { int_ty };
  KavakTypeInfo *list_t = kavak_ty_named(&arena, "List", list_t_args, 1, NULL);
  KavakTypeInfo *list_int = kavak_ty_named(&arena, "List", list_int_args, 1, NULL);

  KavakTypeSubst subst[8];
  uint32_t count = 0;

  /* OK: List<T> ~ List<Int> => T := Int */
  KavakUnifyResult r = kavak_ty_unify_strict(list_t, list_int, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_OK && count == 1 &&
         subst[0].param == t && subst[0].replacement == int_ty,
         "List<T> ~ List<Int> => OK, T:=Int");

  /* OK: Pair<T,T> ~ Pair<Int,Int> — repeated param, consistent binding */
  KavakTypeInfo *pair_tt_args[] = { t, t };
  KavakTypeInfo *pair_ii_args[] = { int_ty, int_ty };
  KavakTypeInfo *pair_tt = kavak_ty_named(&arena, "Pair", pair_tt_args, 2, NULL);
  KavakTypeInfo *pair_ii = kavak_ty_named(&arena, "Pair", pair_ii_args, 2, NULL);
  count = 0;
  r = kavak_ty_unify_strict(pair_tt, pair_ii, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_OK && count == 1, "Pair<T,T> ~ Pair<Int,Int> => OK, one binding");

  /* CONFLICT: Pair<T,T> ~ Pair<Int,String> — T forced to two unequal types */
  KavakTypeInfo *pair_is_args[] = { int_ty, string_ty };
  KavakTypeInfo *pair_is = kavak_ty_named(&arena, "Pair", pair_is_args, 2, NULL);
  count = 0;
  r = kavak_ty_unify_strict(pair_tt, pair_is, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_CONFLICT, "Pair<T,T> ~ Pair<Int,String> => CONFLICT");

  /* MISMATCH: differing kinds */
  count = 0;
  r = kavak_ty_unify_strict(list_t, int_ty, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_MISMATCH, "List<T> ~ Int => MISMATCH (kind)");

  /* MISMATCH: same kind/arity but different named head */
  KavakTypeInfo *map_int_args[] = { int_ty };
  KavakTypeInfo *map_int = kavak_ty_named(&arena, "Map", map_int_args, 1, NULL);
  count = 0;
  r = kavak_ty_unify_strict(list_t, map_int, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_MISMATCH, "List<T> ~ Map<Int> => MISMATCH (head)");

  /* OCCURS: T ~ List<T> — param appears inside its own binding */
  count = 0;
  r = kavak_ty_unify_strict(t, list_t, subst, 8, &count);
  ASSERT(r == KAVAK_UNIFY_OCCURS, "T ~ List<T> => OCCURS");

  /* CAP_EXCEEDED: two distinct params into a single slot */
  KavakTypeInfo *pair_tu_args[] = { t, u };
  KavakTypeInfo *pair_tu = kavak_ty_named(&arena, "Pair", pair_tu_args, 2, NULL);
  count = 0;
  r = kavak_ty_unify_strict(pair_tu, pair_is, subst, 1, &count);
  ASSERT(r == KAVAK_UNIFY_CAP_EXCEEDED, "Pair<T,U> into cap 1 => CAP_EXCEEDED");

  /* NULL guard */
  count = 0;
  r = kavak_ty_unify_strict(list_t, list_int, NULL, 8, &count);
  ASSERT(r == KAVAK_UNIFY_MISMATCH, "NULL subst => MISMATCH");

  kavak_type_arena_free(&arena);
  return 0;
}

/* ── A mock nominal hierarchy for the conformance helpers ────────────────────
 * Animal <- Cat <- Kitten, plus Animal <- Dog. Animal is a sealed type whose
 * cases are {Cat, Dog}. Animal declares member "name"; Cat declares "meow".
 * Deliberately NO is_subtype — so kavak_ty_assignable must fall back to the
 * supertype-chain walk. */
static const KavakTypeInfo *g_animal, *g_cat, *g_dog, *g_kitten;
static const KavakTypeInfo *g_member_string, *g_member_int;

static uint32_t hier_supertypes(const KavakLanguage *lang, const KavakTypeInfo *t,
                                const KavakTypeInfo **out, uint32_t cap) {
  (void)lang;
  if (t == g_cat || t == g_dog) { if (cap) out[0] = g_animal; return 1; }
  if (t == g_kitten)            { if (cap) out[0] = g_cat;    return 1; }
  return 0;
}

static const KavakTypeInfo *hier_own_member(const KavakLanguage *lang,
                                            const KavakTypeInfo *t,
                                            const char *name, uint32_t len) {
  (void)lang;
  if (t == g_animal && len == 4 && memcmp(name, "name", 4) == 0) return g_member_string;
  if (t == g_cat && len == 4 && memcmp(name, "meow", 4) == 0) return g_member_int;
  return NULL;
}

static uint32_t hier_variants(const KavakLanguage *lang, const KavakTypeInfo *t,
                              const KavakTypeInfo **out, uint32_t cap) {
  (void)lang;
  if (t == g_animal) {
    if (cap >= 2) { out[0] = g_cat; out[1] = g_dog; }
    return 2;
  }
  return 0;
}

static const KavakLanguage HIER_LANG = {
  .name = "Hier",
  .file_extension = ".hier",
  .version = 1,
  .supertypes = hier_supertypes,
  .find_own_member = hier_own_member,
  .variants = hier_variants,
};

static int test_conformance(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  g_animal = kavak_ty_named(&arena, "Animal", NULL, 0, NULL);
  g_cat    = kavak_ty_named(&arena, "Cat", NULL, 0, NULL);
  g_dog    = kavak_ty_named(&arena, "Dog", NULL, 0, NULL);
  g_kitten = kavak_ty_named(&arena, "Kitten", NULL, 0, NULL);
  g_member_string = kavak_ty_builtin(&arena, KAVAK_TY_STRING);
  g_member_int    = kavak_ty_builtin(&arena, KAVAK_TY_INT);

  /* conforms: reflexive + transitive over direct edges */
  ASSERT(kavak_ty_conforms(g_cat, g_cat, &HIER_LANG), "type conforms to itself");
  ASSERT(kavak_ty_conforms(g_cat, g_animal, &HIER_LANG), "Cat <: Animal (direct)");
  ASSERT(kavak_ty_conforms(g_kitten, g_animal, &HIER_LANG),
         "Kitten <: Animal (transitive)");
  ASSERT(!kavak_ty_conforms(g_animal, g_cat, &HIER_LANG), "Animal not <: Cat");
  ASSERT(!kavak_ty_conforms(g_cat, g_dog, &HIER_LANG), "siblings do not conform");
  ASSERT(!kavak_ty_conforms(g_cat, g_animal, NULL), "no lang => identity only");

  /* kavak_ty_assignable falls back to conforms when is_subtype is unset */
  ASSERT(kavak_ty_assignable(g_kitten, g_animal, &HIER_LANG),
         "assignable uses the supertype chain when is_subtype is absent");
  KavakTypeInfo *n_animal = kavak_ty_nullable(&arena, (KavakTypeInfo *)g_animal);
  ASSERT(kavak_ty_assignable(g_kitten, n_animal, &HIER_LANG),
         "Kitten <: Animal? (nullable + nominal chain)");

  /* member lookup walks up the chain */
  ASSERT(kavak_ty_find_member(g_animal, "name", 4, &HIER_LANG) == g_member_string,
         "own member found");
  ASSERT(kavak_ty_find_member(g_kitten, "name", 0, &HIER_LANG) == g_member_string,
         "inherited member found via the chain (name_len 0 => strlen)");
  ASSERT(kavak_ty_find_member(g_kitten, "meow", 4, &HIER_LANG) == g_member_int,
         "member from an intermediate supertype found");
  ASSERT(kavak_ty_find_member(g_animal, "meow", 4, &HIER_LANG) == NULL,
         "a subtype-only member is not visible on the base");
  ASSERT(kavak_ty_find_member(g_cat, "nope", 4, &HIER_LANG) == NULL,
         "unknown member is NULL");

  /* exhaustiveness over the sealed set {Cat, Dog} */
  const KavakTypeInfo *all[] = { g_cat, g_dog };
  ASSERT(kavak_ty_exhaustive(g_animal, all, 2, NULL, &HIER_LANG),
         "covering every case is exhaustive");
  const KavakTypeInfo *base[] = { g_animal };
  ASSERT(kavak_ty_exhaustive(g_animal, base, 1, NULL, &HIER_LANG),
         "matching the base covers all cases");
  const KavakTypeInfo *partial[] = { g_cat };
  const KavakTypeInfo *missing = NULL;
  ASSERT(!kavak_ty_exhaustive(g_animal, partial, 1, &missing, &HIER_LANG),
         "missing a case is not exhaustive");
  ASSERT(missing == g_dog, "reports the first uncovered case");

  kavak_type_arena_free(&arena);
  return 0;
}

/* ── A node with more direct supertypes than the kernel's stack scratch ───────
 * Regression for silent truncation: the conformance/member walks must examine
 * EVERY direct supertype, not just the first KAVAK_TY_SUPERS_CAP (16). */
#define WIDE_N 20u
static const KavakTypeInfo *g_wide;
static const KavakTypeInfo *g_wide_supers[WIDE_N];
static const KavakTypeInfo *g_wide_member;

static uint32_t wide_supertypes(const KavakLanguage *lang, const KavakTypeInfo *t,
                                const KavakTypeInfo **out, uint32_t cap) {
  (void)lang;
  if (t == g_wide) {
    for (uint32_t i = 0; i < WIDE_N && i < cap; ++i) out[i] = g_wide_supers[i];
    return WIDE_N;  /* total, even when cap < WIDE_N */
  }
  return 0;
}

static const KavakTypeInfo *wide_own_member(const KavakLanguage *lang,
                                            const KavakTypeInfo *t,
                                            const char *name, uint32_t len) {
  (void)lang;
  /* Declared only on the LAST super (index 19), past the old 16-entry cap. */
  if (t == g_wide_supers[WIDE_N - 1u] && len == 4 && memcmp(name, "deep", 4) == 0)
    return g_wide_member;
  return NULL;
}

static const KavakLanguage WIDE_LANG = {
  .name = "Wide",
  .file_extension = ".wide",
  .version = 1,
  .supertypes = wide_supertypes,
  .find_own_member = wide_own_member,
};

static int test_wide_supertypes_not_truncated(void) {
  KavakTypeArena arena;
  kavak_type_arena_init(&arena);

  /* Names are borrowed by kavak_ty_named, so keep distinct storage alive for
   * the whole test. */
  char names[WIDE_N][8];
  g_wide = kavak_ty_named(&arena, "Wide", NULL, 0, NULL);
  for (uint32_t i = 0; i < WIDE_N; ++i) {
    snprintf(names[i], sizeof names[i], "S%u", i);
    g_wide_supers[i] = kavak_ty_named(&arena, names[i], NULL, 0, NULL);
  }
  g_wide_member = kavak_ty_builtin(&arena, KAVAK_TY_INT);

  ASSERT(kavak_ty_conforms(g_wide, g_wide_supers[0], &WIDE_LANG),
         "conforms finds an early supertype");
  ASSERT(kavak_ty_conforms(g_wide, g_wide_supers[WIDE_N - 1u], &WIDE_LANG),
         "conforms finds the supertype past the 16-entry stack cap");
  ASSERT(kavak_ty_find_member(g_wide, "deep", 4, &WIDE_LANG) == g_wide_member,
         "member lookup reaches the supertype past the 16-entry stack cap");

  kavak_type_arena_free(&arena);
  return 0;
}

int main(void) {
  int fails = 0;
  fails += test_builtins();
  fails += test_constructors_and_to_string();
  fails += test_record_rejects_malformed_fields();
  fails += test_equality();
  fails += test_cycle_guards();
  fails += test_wide_type_graph_does_not_trip_depth_guard();
  fails += test_substitution();
  fails += test_substitute_cyclic_type_terminates();
  fails += test_deep_acyclic_equality();
  fails += test_session_type_arena();
  fails += test_session_reset_types();
  fails += test_nullable_helpers();
  fails += test_assignability_structural();
  fails += test_assignability_nominal_hook();
  fails += test_numeric_common();
  fails += test_join();
  fails += test_unify();
  fails += test_unify_strict();
  fails += test_conformance();
  fails += test_wide_supertypes_not_truncated();

  if (fails == 0) {
    printf("  ✓ test_type: 20/20 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_type: %d failure(s)\n", fails);
  return 1;
}
