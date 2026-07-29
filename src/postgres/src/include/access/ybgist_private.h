/*--------------------------------------------------------------------------
 *
 * ybgist_private.h
 *	  header file for Yugabyte Generalized Inverted Index access method
 *	  implementation.
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *			src/include/access/ybgist_private.h
 *--------------------------------------------------------------------------
 */

#pragma once

#include "access/gin_private.h"
#include "utils/hsearch.h"
#include "utils/palloc.h"
#include "yb/yql/pggate/ybc_pg_typedefs.h"

typedef struct YbgistScanOpaqueData
{
	/*
	 * The scan opaque for the gin access method.  Therefore, this can be cast
	 * as GinScanOpaqueData to make use of gin access method functions.
	 */
	GinScanOpaqueData gin_scan_opaque;
	/* The handle for the internal YB Select statement. */
	YbcPgStatement handle;
	/*
	 * Whether the scan was executed so that later gettuple calls just fetch the
	 * cached rows.
	 */
	bool		is_exec_done;
	/*
	 * YB spatial: a cell-covering scan may bind several covering cells as an IN
	 * condition, so a single base row (whose bbox spans several matched cells)
	 * can be returned once per cell.  This set de-duplicates fetched rows by
	 * their ybctid.  NULL until first use; reset on rescan.
	 */
	HTAB	   *yb_seen_ctids;

	/*
	 * YB spatial range+probe scan (S2 cell spans).  The query's covering cells
	 * are turned into
	 *   - descendant SPANS: each query cell's subtree is a contiguous id range
	 *     [id - (2^s - 1), id + (2^s - 1)] (s = stop-bit position), coalesced
	 *     when adjacent -- matched with one CondBetween request per span;
	 *   - ancestor PROBES: the ids of every strict ancestor of every query
	 *     cell, sorted+deduped -- matched with one CondIn request.
	 * DocDB ANDs all conditions on a request, so each span/probe-set needs its
	 * own select request; requests run sequentially within one index scan and
	 * results are unioned via yb_seen_ctids.  yb_legacy_bind marks the
	 * preserved single-request partial-match (prefix) path.
	 */
	Datum	   *yb_probes;		/* sorted unique ancestor cell ids */
	int			yb_nprobes;
	int64	   *yb_span_lo;		/* coalesced spans, ascending */
	int64	   *yb_span_hi;
	int			yb_nspans;
	int			yb_total_reqs;	/* (yb_nprobes > 0) + yb_nspans, or 1 legacy */
	int			yb_next_req;	/* next request index to start */
	bool		yb_legacy_bind; /* partial-match path: binds pre-applied */

	/*
	 * Multicolumn: equality binds for the leading (non-spatial) key columns,
	 * replayed onto EVERY per-request handle.  The spatial column is always
	 * the LAST key column (enforced by ybgistCheckShape).
	 */
	int			yb_neq;
	AttrNumber *yb_eq_attno;
	Datum	   *yb_eq_value;
} YbgistScanOpaqueData;

typedef YbgistScanOpaqueData *YbgistScanOpaque;

extern const char *ybgistNullCategoryToString(GinNullCategory category);
extern const char *ybgistSearchModeToString(int32 searchMode);
extern void ybgistInitHandle(IndexScanDesc scan);
extern int	ybgistEstimateRequests(const int64 *ids, int n);
extern void ybgistCheckShape(Relation index);
