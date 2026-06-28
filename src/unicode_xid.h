// SPDX-License-Identifier: MIT
/**
 * @file src/unicode_xid.h
 * @brief Unicode XID_Start / XID_Continue predicates (UAX #31).
 *
 * Backed by a small generated range table (src/unicode_xid.c, produced by
 * scripts/gen_xid_table.py). kavak owns this table outright — no external
 * Unicode library is required for identifier classification.
 */
#ifndef KAVAK_UNICODE_XID_H
#define KAVAK_UNICODE_XID_H

#include <stdbool.h>
#include <stdint.h>

/** @brief True if @p cp carries the Unicode XID_Start property. */
bool kavak_xid_is_start(uint32_t cp);

/** @brief True if @p cp carries the Unicode XID_Continue property. */
bool kavak_xid_is_continue(uint32_t cp);

#endif /* KAVAK_UNICODE_XID_H */
