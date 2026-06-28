// SPDX-License-Identifier: MIT
/**
 * @file src/kavak_internal.h
 * @brief Declarations shared across translation units but NOT part of the
 *        public ABI in include/kavak.h.
 *
 * Keep this list small. Anything here is implementation detail: it may change
 * or disappear between versions and must never appear in the installed header.
 */
#ifndef KAVAK_INTERNAL_H
#define KAVAK_INTERNAL_H

#include "kavak.h"

/* Single source of truth for the recursion/nesting bound shared by the parser's
 * expression-depth guard and the type-graph traversal guard. The two were
 * historically separate 512 constants documented as "kept aligned"; defining
 * them from one value makes the alignment real instead of a comment. 512 is far
 * beyond any real program's expression nesting or type depth, so it is a soft
 * cap that degrades conservatively rather than an observable limit. */
#define KAVAK_RECURSION_LIMIT 512u

/* One-time setup of the UTF-8 / Unicode XID machinery. Defined in utf8.c and
 * called from kavak_session_new(); a stable no-op today (the XID tables are
 * static const and the decoder is stateless). Declared here so utf8.c and its
 * caller share one prototype instead of a cross-TU local declaration. */
void kavak_utf8_init(void);

#endif /* KAVAK_INTERNAL_H */
