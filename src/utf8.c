// SPDX-License-Identifier: MIT
/**
 * @file src/utf8.c
 * @brief UTF-8 decoding and identifier predicates.
 *
 * Self-contained: codepoint classification uses kavak's own XID tables
 * (src/unicode_xid.c, generated from the Unicode Character Database), and the
 * UTF-8 decoder is a small standards-conformant scalar routine. No external
 * Unicode library is required.
 */

#include "kavak.h"
#include "kavak_internal.h"

#include "unicode_xid.h"

#include <stddef.h>
#include <stdint.h>

void kavak_utf8_init(void) {
  /* No initialization required: the XID tables are static const data and the
   * decoder is stateless. Kept as a stable no-op so callers (kavak.c) need not
   * change. */
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

  /* Strip the leading-byte tag bits; widths: 1→7, 2→5, 3→4, 4→3 payload bits. */
  static const unsigned char lead_mask[5] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };
  uint32_t cp = (uint32_t)(lead & lead_mask[len]);

  for (int i = 1; i < len; i++) {
    const unsigned char cont = (unsigned char)p[i];
    if ((cont & 0xC0) != 0x80) return 0; /* not a 10xxxxxx continuation byte */
    cp = (cp << 6) | (uint32_t)(cont & 0x3F);
  }

  /* Reject overlong encodings, UTF-16 surrogates, and out-of-range scalars.
   * (utf8_lead_len already rejects the 0xC0/0xC1 2-byte overlong leads.) */
  static const uint32_t min_cp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
  if (cp < min_cp[len]) return 0;             /* overlong */
  if (cp > 0x10FFFF) return 0;                /* beyond Unicode */
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0; /* lone surrogate */

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
  /* ASCII fast path covers the overwhelmingly common case without a search.
   * '_' is XID_Continue (Pc), not XID_Start, so kavak admits it explicitly. */
  if (cp < 0x80) return kavak_ascii_is_ident_start(cp);
  return kavak_xid_is_start(cp);
}

int kavak_unicode_is_ident_cont(uint32_t cp) {
  if (cp < 0x80) return kavak_ascii_is_ident_cont(cp);
  return kavak_xid_is_continue(cp);
}
