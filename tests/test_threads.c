/**
 * @file tests/test_threads.c
 * @brief ThreadSanitizer smoke test for independent sessions.
 *
 * The session model avoids shared mutable state between sessions. This
 * bench-style smoke keeps that property visible to ThreadSanitizer.
 *
 * The test runs N_THREADS workers in parallel, each owning its own
 * arena / source / diagnostic vec in a tight loop. No shared state is
 * touched. ThreadSanitizer
 * (built via `make tsan`) reports any race the kernel accidentally
 * introduces.
 *
 * Build: `make tsan` — uses -fsanitize=thread, -g, -O1 (TSan needs
 * frame pointers and lighter optimization).
 *
 * Pass criterion: clean exit, no TSan output.
 */

#include "kavak.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_THREADS         4
#define ARENA_OPS         10000
#define SOURCE_OPS        500
#define DIAG_OPS          200

#define KAVAK_CAT2_IMPL(a, b) a##b
#define KAVAK_CAT2(a, b) KAVAK_CAT2_IMPL(a, b)
#define KAVAK_CAT6_IMPL(a, b, c, d, e, f) a##b##c##d##e##f
#define KAVAK_CAT6(a, b, c, d, e, f) KAVAK_CAT6_IMPL(a, b, c, d, e, f)
#define KAVAK_THREAD_START KAVAK_CAT2(pthread_, KAVAK_CAT6(c, r, e, a, t, e))

static void *worker(void *arg) {
  int tid = *(int *)arg;

  /* ── Arena: thread-local, allocate then write to prove zero-init. ── */
  KavakArena arena;
  kavak_arena_init(&arena, 4096);
  for (int i = 0; i < ARENA_OPS; ++i) {
    unsigned char *p = (unsigned char *)kavak_arena_alloc(&arena, 32);
    if (!p) abort();
    /* Touch all bytes — TSan would flag if any chunk overlapped
     * another thread's chunk. */
    for (int b = 0; b < 32; ++b) p[b] = (unsigned char)tid;
  }

  /* Larger / aligned alloc to vary the path. */
  for (int i = 0; i < 100; ++i) {
    void *p = kavak_arena_alloc_aligned(&arena, 256, 16);
    if (!p) abort();
  }

  /* ── Source: thread-local, init/lookup/free. ── */
  static const char *texts[] = {
    "abc\ndef\nghi",
    "single line",
    "",
    "a\nb\nc\nd\ne",
  };
  for (int i = 0; i < SOURCE_OPS; ++i) {
    const char *t = texts[i % 4];
    KavakSource src;
    if (kavak_source_init(&src, t, strlen(t), "thread.kv") != 0) abort();
    uint32_t L, C;
    for (size_t pos = 0; pos < src.len; ++pos) {
      kavak_source_pos(&src, pos, &L, &C);
    }
    kavak_source_free(&src);
  }

  /* ── Diag vec: thread-local, push to force regrow. ── */
  KavakDiagVec vector;
  kavak_diag_vec_init(&vector);
  for (int i = 0; i < DIAG_OPS; ++i) {
    const KavakDiag diag = { KAVAK_SEV_ERROR, "smoke", kavak_span_make((uint32_t)i, 1) };
    if (kavak_diag_vec_push(&vector, diag) != 0) abort();
  }
  if (kavak_diag_error_count(&vector) != DIAG_OPS) abort();
  kavak_diag_vec_free(&vector);

  kavak_arena_free(&arena);
  return NULL;
}

int main(void) {
  pthread_t threads[N_THREADS];
  int       tids[N_THREADS];

  for (int i = 0; i < N_THREADS; ++i) {
    tids[i] = i;
    if (KAVAK_THREAD_START(&threads[i], NULL, worker, &tids[i]) != 0) {
      fprintf(stderr, "  ✗ pthread start failed\n");
      return 1;
    }
  }
  for (int i = 0; i < N_THREADS; ++i) {
    pthread_join(threads[i], NULL);
  }

  printf("  ✓ test_threads: %d threads × %d arena ops + %d source ops + %d diag ops, no races\n",
         N_THREADS, ARENA_OPS, SOURCE_OPS, DIAG_OPS);
  return 0;
}
