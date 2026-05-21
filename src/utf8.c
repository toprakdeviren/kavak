/**
 * @file src/utf8.c
 * @brief UTF-8 decoding and identifier predicates.
 *
 * kavak now delegates UTF-8 validation/conversion and Unicode identifier
 * predicates to decoder, which is built as a static library by the Makefile.
 */

#include "kavak.h"

#include <core.h>
#include <encoding.h>
#include <security.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

static void kavak_decoder_ensure_init(void) {
  static atomic_int state = ATOMIC_VAR_INIT(0);
  int expected = 0;
  if (atomic_load_explicit(&state, memory_order_acquire) == 2) return;
  if (atomic_compare_exchange_strong_explicit(&state, &expected, 1,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
    decoder_init();
    atomic_store_explicit(&state, 2, memory_order_release);
    return;
  }
  while (atomic_load_explicit(&state, memory_order_acquire) != 2) {}
}

static int utf8_lead_len(const unsigned char lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

int kavak_utf8_decode(const char *p, const char *end, uint32_t *out_cp) {
  if (!p || !end || p >= end) return 0;
  const unsigned char lead = (unsigned char)*p;
  const int len = utf8_lead_len(lead);
  if (len == 0 || (size_t)(end - p) < (size_t)len) return 0;

  kavak_decoder_ensure_init();

  uint32_t cp = 0;
  size_t written = 0;
  const int rc = decoder_utf8_to_utf32((const uint8_t *)p, (size_t)len,
                                       &cp, 1u, &written);
  if (rc != 0 || written != 1u) return 0;

  if (out_cp) *out_cp = cp;
  return len;
}

int kavak_utf8_advance(const char *p, const char *end) {
  return kavak_utf8_decode(p, end, NULL);
}

int kavak_ascii_is_ident_start(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_';
}

int kavak_ascii_is_ident_cont(uint32_t cp) {
  return kavak_ascii_is_ident_start(cp) || (cp >= '0' && cp <= '9');
}

int kavak_unicode_is_ident_start(uint32_t cp) {
  kavak_decoder_ensure_init();
  return cp == '_' || decoder_is_identifier_start(cp);
}

int kavak_unicode_is_ident_cont(uint32_t cp) {
  kavak_decoder_ensure_init();
  return cp == '_' || decoder_is_identifier_continue(cp);
}
