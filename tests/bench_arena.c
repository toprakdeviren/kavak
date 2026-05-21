// SPDX-License-Identifier: MIT
/**
 * @file tests/bench_arena.c
 * @brief Arena-churn benchmark for narrowing-style allocations.
 *
 * Simulates the allocation pattern used by flow-sensitive narrowing:
 * one small holder per shadowed binding inside a branch. This bench
 * measures that shape against the bump arena.
 *
 * Two views:
 *   1. Linear count   — total ns/alloc at sizes 1K..1M.
 *   2. Chain depth    — repeats a depth-d `if let a, b, …` cascade
 *                        1000× in an outer loop, simulating a sema pass
 *                        over many functions each with the same shape.
 *
 * The bench is informational, not a regression gate. Run via
 * `make bench`. Output goes to stdout for manual comparison.
 */

#include "kavak.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Synthetic narrowing-holder size = KavakASTNode header prefix. */
#define HOLDER_SIZE 48

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void bench_linear(uint32_t n) {
  KavakArena arena;
  kavak_arena_init(&arena, 64 * 1024);   /* 64 KB chunks */

  double t0 = now_ms();
  for (uint32_t i = 0; i < n; ++i) {
    volatile void *p = kavak_arena_alloc(&arena, HOLDER_SIZE);
    (void)p;                          /* defeat dead-store opt */
  }
  double dt = now_ms() - t0;

  printf("  linear  n=%-8u  %8.2f ms   %6.1f ns/alloc   %8lu bytes used\n",
         n, dt, dt * 1.0e6 / (double)n, (unsigned long)arena.total_used);

  kavak_arena_free(&arena);
}

static void bench_cascade(uint32_t depth, uint32_t outer) {
  /* Simulates `if let a, b, c, ..., n` repeated `outer` times. Each
   * inner pass allocates `depth` holders, then the arena keeps
   * growing — sema doesn't free between iterations because the
   * holders' lifetime is the enclosing scope's lifetime. */
  KavakArena arena;
  kavak_arena_init(&arena, 64 * 1024);

  double t0 = now_ms();
  for (uint32_t o = 0; o < outer; ++o) {
    for (uint32_t d = 0; d < depth; ++d) {
      volatile void *p = kavak_arena_alloc(&arena, HOLDER_SIZE);
      (void)p;
    }
  }
  double dt = now_ms() - t0;

  uint64_t total = (uint64_t)outer * depth;
  printf("  cascade depth=%-4u outer=%-5u  %8.2f ms   %6.1f ns/alloc   %lu allocs\n",
         depth, outer, dt, dt * 1.0e6 / (double)total, (unsigned long)total);

  kavak_arena_free(&arena);
}

int main(void) {
  printf("KavakArena synthetic narrowing-holder benchmark\n");
  printf("  holder_size = %d bytes (KavakASTNode header)\n", HOLDER_SIZE);
  printf("  chunk_size  = 65536 bytes (64 KB)\n\n");

  printf("Linear allocation count:\n");
  bench_linear(1000);
  bench_linear(10000);
  bench_linear(100000);
  bench_linear(1000000);

  printf("\nIf-let cascade (sema's flow-narrowing pattern):\n");
  bench_cascade(1,    1000);  /* 1×1000 = 1K  shallow */
  bench_cascade(10,   1000);  /* 10×1000 = 10K shallow + repeated */
  bench_cascade(100,  1000);  /* 100×1000 = 100K — realistic upper */
  bench_cascade(1000, 100);   /* 1000×100 = 100K — pathological depth */

  printf("\nVerdict: use these numbers to judge whether pooling is worthwhile.\n");
  return 0;
}
