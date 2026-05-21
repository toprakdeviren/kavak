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

int main(void) {
  int fails = 0;
  fails += test_builtins();
  fails += test_constructors_and_to_string();
  fails += test_equality();
  fails += test_cycle_guards();
  fails += test_wide_type_graph_does_not_trip_depth_guard();
  fails += test_substitution();
  fails += test_session_type_arena();

  if (fails == 0) {
    printf("  ✓ test_type: 7/7 passed\n");
    return 0;
  }
  fprintf(stderr, "  ✗ test_type: %d failure(s)\n", fails);
  return 1;
}
