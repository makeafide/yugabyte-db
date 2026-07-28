/*--------------------------------------------------------------------------
 *
 * ybgistget.c
 *	  fetch tuples from a Yugabyte GIN scan.
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
 *			src/backend/access/ybgist/ybgistget.c
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/sysattr.h"
#include "access/yb_scan.h"
#include "access/ybgist.h"
#include "access/ybgist_private.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"

#include "nodes/makefuncs.h"
#include "pg_yb_utils.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/yb_like_support.h"
#include "yb/yql/pggate/ybc_pggate.h"

/* Copied from ginget.c. */
#define GinIsVoidRes(s)		( ((GinScanOpaque) scan->opaque)->isVoidRes )

#define TSVECTOR_GIN_FAM_OID	((Oid) 3659)

/*
 * Parts copied from ginget.c.  Take the code right under label
 * restartScanEntry that initializes entry.
 */
static void
startScanEntry(GinScanEntry entry)
{
	entry->buffer = InvalidBuffer;
	ItemPointerSetMin(&entry->curItem);
	entry->offset = InvalidOffsetNumber;
	if (entry->list)
		pfree(entry->list);
	entry->list = NULL;
	entry->nlist = 0;
	entry->matchBitmap = NULL;
	entry->matchResult = NULL;
	entry->reduceResult = false;
	entry->predictNumberResult = 0;
}

/*
 * Comparison function for scan entry indexes. Sorts by predictNumberResult,
 * least frequent items first.
 *
 * Copied from ginget.c.
 */
static int
entryIndexByFrequencyCmp(const void *a1, const void *a2, void *arg)
{
	const GinScanKey key = (const GinScanKey) arg;
	int			i1 = *(const int *) a1;
	int			i2 = *(const int *) a2;
	uint32		n1 = key->scanEntry[i1]->predictNumberResult;
	uint32		n2 = key->scanEntry[i2]->predictNumberResult;

	if (n1 < n2)
		return -1;
	else if (n1 == n2)
		return 0;
	else
		return 1;
}

/*
 * Copied from ginget.c with the only difference being the lack of parameter
 * ginstate, which isn't used there either.
 */
static void
startScanKey(GinScanOpaque so, GinScanKey key)
{
	MemoryContext oldCtx = CurrentMemoryContext;
	int			i;
	int			j;
	int		   *entryIndexes;

	ItemPointerSetMin(&key->curItem);
	key->curItemMatches = false;
	key->recheckCurItem = false;
	key->isFinished = false;

	/*
	 * Divide the entries into two distinct sets: required and additional.
	 * Additional entries are not enough for a match alone, without any items
	 * from the required set, but are needed by the consistent function to
	 * decide if an item matches. When scanning, we can skip over items from
	 * additional entries that have no corresponding matches in any of the
	 * required entries. That speeds up queries like "frequent & rare"
	 * considerably, if the frequent term can be put in the additional set.
	 *
	 * There can be many legal ways to divide them entries into these two
	 * sets. A conservative division is to just put everything in the required
	 * set, but the more you can put in the additional set, the more you can
	 * skip during the scan. To maximize skipping, we try to put as many
	 * frequent items as possible into additional, and less frequent ones into
	 * required. To do that, sort the entries by frequency
	 * (predictNumberResult), and put entries into the required set in that
	 * order, until the consistent function says that none of the remaining
	 * entries can form a match, without any items from the required set. The
	 * rest go to the additional set.
	 */
	if (key->nentries > 1)
	{
		MemoryContextSwitchTo(so->tempCtx);

		entryIndexes = (int *) palloc(sizeof(int) * key->nentries);
		for (i = 0; i < key->nentries; i++)
			entryIndexes[i] = i;
		qsort_arg(entryIndexes, key->nentries, sizeof(int),
				  entryIndexByFrequencyCmp, key);

		for (i = 0; i < key->nentries - 1; i++)
		{
			/* Pass all entries <= i as FALSE, and the rest as MAYBE */
			for (j = 0; j <= i; j++)
				key->entryRes[entryIndexes[j]] = GIN_FALSE;
			for (j = i + 1; j < key->nentries; j++)
				key->entryRes[entryIndexes[j]] = GIN_MAYBE;

			if (key->triConsistentFn(key) == GIN_FALSE)
				break;
		}
		/* i is now the last required entry. */

		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = i + 1;
		key->nadditional = key->nentries - key->nrequired;
		key->requiredEntries = palloc(key->nrequired * sizeof(GinScanEntry));
		key->additionalEntries = palloc(key->nadditional * sizeof(GinScanEntry));

		j = 0;
		for (i = 0; i < key->nrequired; i++)
			key->requiredEntries[i] = key->scanEntry[entryIndexes[j++]];
		for (i = 0; i < key->nadditional; i++)
			key->additionalEntries[i] = key->scanEntry[entryIndexes[j++]];

		/* clean up after consistentFn calls (also frees entryIndexes) */
		MemoryContextReset(so->tempCtx);
	}
	else
	{
		MemoryContextSwitchTo(so->keyCtx);

		key->nrequired = 1;
		key->nadditional = 0;
		key->requiredEntries = palloc(1 * sizeof(GinScanEntry));
		key->requiredEntries[0] = key->scanEntry[0];
	}
	MemoryContextSwitchTo(oldCtx);
}

/*
 * Parts copied from ginget.c.  Don't bother copying GinFuzzySearchLimit code
 * since it's not supported.
 */
static void
startScan(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	uint32		i;

	for (i = 0; i < so->totalentries; i++)
		startScanEntry(so->entries[i]);

	/* Don't support GinFuzzySearchLimit. */

	/*
	 * Now that we have the estimates for the entry frequencies, finish
	 * initializing the scan keys.
	 */
	for (i = 0; i < so->nkeys; i++)
		startScanKey(so, so->keys + i);
}

/*
 * Set up the scan keys, and check for unsatisfiable query.
 *
 * Parts copied from ginget.c gingetbitmap.  Since ybgist does not use bitmap
 * scan, things are much different.  gingetbitmap iterates over the resulting
 * tuples, but ybgistgettuple will be called once for each result tuple.  Only
 * take the setup part of gingetbitmap.
 */
static bool
ybgistGetScanKeys(IndexScanDesc scan)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;

	ginFreeScanKeys(so);		/* there should be no keys yet, but just to be
								 * sure */
	ginNewScanKey(scan);

	if (GinIsVoidRes(scan))
		return false;

	/*
	 * Categorize scan entries to required and additional.
	 */
	startScan(scan);

	return true;
}

/*
 * Generate a Const node of text type from a C string.
 *
 * Parts copied from string_to_const.
 */
static Const *
text_to_const(Datum conval, Oid colloid)
{
	Oid			datatype = TEXTOID;
	int			constlen = -1;

	return makeConst(datatype, -1, colloid, constlen,
					 conval, false, false);
}

/*
 * Try to generate a string to serve as an exclusive upperbound for matching
 * strings with the given prefix.  If successful, return a palloc'd string in
 * the form of a Const node; else, return NULL.
 */
static Const *
get_greaterstr(Datum prefix, Oid datatype, Oid colloid)
{
	Const	   *prefix_const;
	FmgrInfo	ltproc;
	Oid			opfamily;
	Oid			oproid;

	/* yb_make_greater_string cannot accurately handle non-C collations. */
	if (!lc_collate_is_c(colloid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot handle ybgist scans with prefix on non-C collation %u",
						colloid)));

	/*
	 * For now, hardcode to assume type is text.  This is true for the four
	 * native postgres ybgist opclasses, but it may no longer be true when
	 * supporting extensions like btree_gin.  This assumption makes finding
	 * opfamily and operator easier.
	 */
	if (datatype != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot handle ybgist scans with prefix on key type %u",
						datatype)));

	opfamily = TEXT_LSM_FAM_OID;
	oproid = get_opfamily_member(opfamily, datatype, datatype,
								 BTLessStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no < operator for opfamily %u", opfamily);
	fmgr_info(get_opcode(oproid), &ltproc);
	prefix_const = text_to_const(prefix, colloid);
	return yb_make_greater_string(prefix_const, &ltproc, colloid);
}

static void
ybgistSetupBindsForPrefix(TupleDesc tupdesc, YbgistScanOpaque ybso,
						 GinScanEntry entry)
{
	Const	   *greaterstr;
	GinScanOpaque so = (GinScanOpaque) ybso;
	Oid			colloid;
	Oid			typoid;
	YbcPgExpr	expr_start,
				expr_end;

	colloid = so->ginstate.supportCollation[0];
	typoid = TupleDescAttr(tupdesc, 0)->atttypid;

	expr_start = YBCNewConstant(ybso->handle,
								typoid,
								colloid,
								entry->queryKey,
								false /* is_null */ );

	greaterstr = get_greaterstr((Datum) entry->queryKey,
								typoid,
								colloid);
	if (greaterstr)
	{
		expr_end = YBCNewConstant(ybso->handle,
								  typoid,
								  colloid,
								  greaterstr->constvalue,
								  false /* is_null */ );
		HandleYBStatus(YBCPgDmlBindColumnCondBetween(ybso->handle,
													 1 /* attr_num */ ,
													 expr_start,
													 true,
													 expr_end,
													 false));
		pfree(greaterstr);
	}
	else
		HandleYBStatus(YBCPgDmlBindColumnCondBetween(ybso->handle,
													 1 /* attr_num */ ,
													 expr_start,
													 true,
													 NULL /* attr_value_end */ ,
													 true));
}

static void
ybgistSetupBindsForPartialMatch(TupleDesc tupdesc, YbgistScanOpaque ybso,
							   GinScanEntry entry)
{
	GinScanOpaque so = (GinScanOpaque) ybso;

	/*
	 * For now, assume partial match always means prefix match.  In the
	 * future, this should be handled by a new support function, similar to
	 * the existing support function comparePartial.
	 *
	 * TODO(jason): don't assume one column when multicolumn is supported.
	 */
	if (so->ginstate.index->rd_opfamily[0] != TSVECTOR_GIN_FAM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported ybgist index scan"),
				 errdetail("Partial match with ybgist index method"
						   " currently only supports opfamily %u: got %u.",
						   TSVECTOR_GIN_FAM_OID,
						   so->ginstate.index->rd_opfamily[0]),
				 errhint("Turn off index scan using"
						 " \"SET enable_indexscan TO false\".")));
	ybgistSetupBindsForPrefix(tupdesc, ybso, entry);
}

/*
 * YB spatial cell-id bit math.  Mirrors the encoding in the ybgist opclass
 * extension (ybgist_cells.h): a cell id is the Morton interleave of (i,j)
 * shifted left with a trailing "stop" bit marking the level, so
 *   - the stop bit is the lowest set bit;
 *   - a cell's whole subtree (all strictly finer descendants plus itself)
 *     occupies the contiguous id interval [id - (2^s - 1), id + (2^s - 1)]
 *     where s is the stop-bit position;
 *   - the parent cell clears the two finest Morton bits and moves the stop
 *     bit up by two.
 * Ancestor ids never fall inside a descendant span (their stop bit exceeds
 * the span's half-width), so probes and spans are disjoint.
 */
static inline int
ybgistCellStopShift(int64 id)
{
	Assert(id > 0);
	return __builtin_ctzll((uint64) id);
}

/* id of the parent cell, or 0 if id is already the level-0 root */
static inline int64
ybgistCellParent(int64 id)
{
	int			s = ybgistCellStopShift(id);

	if (s >= 60)				/* level 0: no parent */
		return 0;
	return (int64) (((uint64) id & ~((1ULL << (s + 3)) - 1)) |
					(1ULL << (s + 2)));
}

static inline void
ybgistCellSpan(int64 id, int64 *lo, int64 *hi)
{
	int64		half = ((int64) 1 << ybgistCellStopShift(id)) - 1;

	*lo = id - half;
	*hi = id + half;
}

static int
ybgistCmpInt64(const void *a, const void *b)
{
	int64		lhs = *(const int64 *) a;
	int64		rhs = *(const int64 *) b;

	return (lhs > rhs) - (lhs < rhs);
}

/*
 * Plan the DocDB select requests for a spatial range+probe scan.
 *
 * Every query entry (a covering cell id) contributes one descendant span and
 * its chain of strict-ancestor probe ids.  Spans are sorted and coalesced when
 * they touch; probes are sorted and de-duplicated.  Matching semantics: an
 * indexed cell X overlaps a query cell Q iff X is in Q's subtree (span) or X
 * is a strict ancestor of Q (probe) -- together with the executor's exact
 * recheck this preserves GIN overlap semantics without requiring ancestors to
 * be materialized in the index or the query covering.
 */
static void
ybgistPlanRequests(IndexScanDesc scan)
{
	GinScanEntry entry;
	GinScanKey	key;
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	TupleDesc	tupdesc;
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	/*
	 * For now, only handle single-key scans.  Multiple keys are possible even
	 * if multicolumn is disabled by specifiying the same column in multiple
	 * conditions (e.g. v @@ 'abc' and v @@ 'bcd').
	 */
	if (so->nkeys != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported ybgist index scan"),
				 errdetail("ybgist index method cannot use"
						   " more than one scan key: got %d.",
						   so->nkeys),
				 errhint("Consider rewriting the query with INTERSECT and"
						 " UNION.")));
	key = &so->keys[0];

	/*
	 * For now, only handle the default search mode.
	 */
	if (key->searchMode != GIN_SEARCH_MODE_DEFAULT)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported ybgist index scan"),
				 errdetail("ybgist index method does not support"
						   " non-default search mode: %s.",
						   ybgistSearchModeToString(key->searchMode))));

	/* Bind to the index because why else would we do an index scan? */
	tupdesc = RelationGetDescr(scan->indexRelation);

	/*
	 * YB spatial (ybgist): the required-entry set may have more than one entry.
	 * For a cell-covering spatial opclass this happens when a query bbox spans
	 * several grid cells -- the entries are the covering cells and a base row
	 * matches if it shares ANY of them (GIN overlap semantics).  Bind them all
	 * as an IN condition on the index key column.  A base row whose bbox covers
	 * several matched query cells is returned once per cell; ybgistgettuple
	 * de-duplicates by ybctid.
	 *
	 * The single-entry partial-match path (prefix scans) is preserved as-is.
	 */
	if (key->nrequired == 1 && key->requiredEntries[0]->isPartialMatch)
	{
		entry = key->requiredEntries[0];
		if (entry->queryCategory != GIN_CAT_NORM_KEY)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported ybgist index scan"),
					 errdetail("ybgist index method does not support"
							   " non-normal null category: %s.",
							   ybgistNullCategoryToString(entry->queryCategory))));
		ybgistSetupBindsForPartialMatch(tupdesc, ybso, entry);
		ybso->yb_legacy_bind = true;
		ybso->yb_total_reqs = 1;
	}
	else
	{
		MemoryContext scanCtx = GetMemoryChunkContext(ybso);
		int64	   *lo;
		int64	   *hi;
		int64	   *probes;
		int			nprobes = 0;
		int			nspans = 0;
		int			maxprobes = 0;
		int			i;

		/*
		 * Turn EVERY query entry (not just GIN's "required" subset -- the
		 * required/additional split is selectivity-driven and would drop
		 * entries that overlap semantics need) into a descendant span plus
		 * ancestor probes.
		 */
		for (i = 0; i < key->nentries; i++)
		{
			entry = key->scanEntry[i];

			/* For now, don't handle null entries. */
			if (entry->queryCategory != GIN_CAT_NORM_KEY)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unsupported ybgist index scan"),
						 errdetail("ybgist index method does not support"
								   " non-normal null category: %s.",
								   ybgistNullCategoryToString(entry->queryCategory))));
			/* Partial match cannot be combined with span/probe requests. */
			if (entry->isPartialMatch)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("unsupported ybgist index scan"),
						 errdetail("ybgist index method cannot combine partial"
								   " match with multiple entries.")));
			/* ancestor-chain length == the cell's level == 30 - stopshift/2 */
			maxprobes += 30 - ybgistCellStopShift((int64)
												  DatumGetInt64(entry->queryKey)) / 2;
		}

		lo = (int64 *) MemoryContextAlloc(scanCtx,
										  key->nentries * sizeof(int64));
		hi = (int64 *) MemoryContextAlloc(scanCtx,
										  key->nentries * sizeof(int64));
		probes = (int64 *) MemoryContextAlloc(scanCtx,
											  Max(maxprobes, 1) * sizeof(int64));

		for (i = 0; i < key->nentries; i++)
		{
			int64		id = DatumGetInt64(key->scanEntry[i]->queryKey);
			int64		anc;

			ybgistCellSpan(id, &lo[nspans], &hi[nspans]);
			nspans++;
			for (anc = ybgistCellParent(id); anc != 0;
				 anc = ybgistCellParent(anc))
				probes[nprobes++] = anc;
		}
		Assert(nprobes <= maxprobes);

		/* sort + coalesce adjacent/overlapping spans */
		{
			typedef struct
			{
				int64		lo;
				int64		hi;
			} YbgistSpan;
			YbgistSpan *spans = (YbgistSpan *)
				MemoryContextAlloc(scanCtx, nspans * sizeof(YbgistSpan));
			int			nmerged = 0;

			for (i = 0; i < nspans; i++)
			{
				spans[i].lo = lo[i];
				spans[i].hi = hi[i];
			}
			qsort(spans, nspans, sizeof(YbgistSpan), ybgistCmpInt64);

			for (i = 0; i < nspans; i++)
			{
				if (nmerged > 0 && spans[i].lo <= hi[nmerged - 1] + 1)
				{
					if (spans[i].hi > hi[nmerged - 1])
						hi[nmerged - 1] = spans[i].hi;
				}
				else
				{
					lo[nmerged] = spans[i].lo;
					hi[nmerged] = spans[i].hi;
					nmerged++;
				}
			}
			nspans = nmerged;
			pfree(spans);
		}

		/* sort + de-duplicate probes (ascending order is load-bearing for the
		 * range-sorted index key column: YBCPgDmlBindColumnCondIn probes the
		 * values in the given order) */
		if (nprobes > 1)
		{
			int			nuniq = 1;

			qsort(probes, nprobes, sizeof(int64), ybgistCmpInt64);
			for (i = 1; i < nprobes; i++)
				if (probes[i] != probes[nuniq - 1])
					probes[nuniq++] = probes[i];
			nprobes = nuniq;
		}

		ybso->yb_span_lo = lo;
		ybso->yb_span_hi = hi;
		ybso->yb_nspans = nspans;
		ybso->yb_nprobes = nprobes;
		ybso->yb_probes = (Datum *) probes;	/* int64 == Datum here */
		StaticAssertStmt(sizeof(Datum) == sizeof(int64),
						 "ybgist cell ids require 64-bit Datums");
		ybso->yb_total_reqs = (nprobes > 0 ? 1 : 0) + nspans;

		/*
		 * Aggregate pushdown streams straight from one DocDB request and
		 * bypasses the ybctid de-dup, so it cannot span multiple requests.
		 * The planner should never choose it (ybgistmightrecheck => true);
		 * fail loudly if it somehow does.
		 */
		if (scan->yb_aggrefs != NIL && ybso->yb_total_reqs > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported ybgist index scan"),
					 errdetail("ybgist aggregate pushdown cannot span"
							   " multiple cell requests.")));
	}
}

/*
 * Bind the conditions of request number `req` onto the current handle:
 * request 0 is the ancestor-probe IN (when there are probes); the rest are
 * one CondBetween per coalesced descendant span (both bounds inclusive, so
 * behavior does not depend on yb_pushdown_strict_inequality).
 */
static void
ybgistBindRequest(IndexScanDesc scan, int req)
{
	GinScanOpaque so = (GinScanOpaque) scan->opaque;
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	Oid			keytypid = TupleDescAttr(tupdesc, 0)->atttypid;
	Oid			keycoll = so->ginstate.supportCollation[0];
	int			spanidx = req - (ybso->yb_nprobes > 0 ? 1 : 0);

	if (ybso->yb_nprobes > 0 && req == 0)
	{
		YbcPgExpr	colref;
		YbcPgExpr  *values;
		int			i;

		values = (YbcPgExpr *) palloc(ybso->yb_nprobes * sizeof(YbcPgExpr));
		for (i = 0; i < ybso->yb_nprobes; i++)
			values[i] = YBCNewConstant(ybso->handle, keytypid, keycoll,
									   ybso->yb_probes[i], false /* is_null */ );
		colref = YBCNewColumnRef(ybso->handle, 1 /* attr_num */ , keytypid,
								 keycoll, NULL /* type_attrs */ );
		HandleYBStatus(YBCPgDmlBindColumnCondIn(ybso->handle, colref,
												ybso->yb_nprobes, values));
		pfree(values);
	}
	else
	{
		YbcPgExpr	lo_expr;
		YbcPgExpr	hi_expr;

		Assert(spanidx >= 0 && spanidx < ybso->yb_nspans);
		lo_expr = YBCNewConstant(ybso->handle, keytypid, keycoll,
								 Int64GetDatum(ybso->yb_span_lo[spanidx]),
								 false /* is_null */ );
		hi_expr = YBCNewConstant(ybso->handle, keytypid, keycoll,
								 Int64GetDatum(ybso->yb_span_hi[spanidx]),
								 false /* is_null */ );
		HandleYBStatus(YBCPgDmlBindColumnCondBetween(ybso->handle,
													 1 /* attr_num */ ,
													 lo_expr,
													 true /* start_inclusive */ ,
													 hi_expr,
													 true /* end_inclusive */ ));
	}
}

/*
 * Add targets for the select.
 */
static void
ybgistSetupTargets(IndexScanDesc scan)
{
	TupleDesc	tupdesc;
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	/*
	 * We don't support IndexOnlyScan, which would directly target the index
	 * table.  Therefore, as an IndexScan, target the base table.  Change this
	 * if we ever support ybgist IndexOnlyScan.
	 */
	tupdesc = RelationGetDescr(scan->heapRelation);

	/*
	 * For scans that touch the base table, we seem to always query for the
	 * ybctid, even if the table may have explicit primary keys.  A lower layer
	 * probably filters this out when not applicable.
	 */
	YbDmlAppendTargetSystem(YBTupleIdAttributeNumber, ybso->handle);
	/*
	 * For now, target all non-system columns of the base table.  This can be
	 * very inefficient.  The lsm index access method avoids this using
	 * filtering (see YbAddTargetColumnIfRequired).
	 *
	 * TODO(jason): don't target unnecessary columns.
	 */
	for (AttrNumber attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		if (!TupleDescAttr(tupdesc, attnum - 1)->attisdropped)
			YbDmlAppendTargetRegular(tupdesc, attnum, ybso->handle);
	}
}

/*
 * With select prepared, ask pggate to execute it for the first time.  This
 * will prefetch some rows.  Later fetches should not use this.
 */
static void
ybgistExecSelect(IndexScanDesc scan, ScanDirection dir)
{
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	Assert(!ScanDirectionIsBackward(dir));
	/* Set scan direction, if matters */
	if (ScanDirectionIsForward(dir))
		HandleYBStatus(YBCPgSetForwardScan(ybso->handle, true));

	HandleYBStatus(YBCPgExecSelect(ybso->handle, NULL /* exec_params */ ));
}

/*
 * Start the next planned request: (re)create the handle if this is not the
 * first request, bind its conditions, set targets, and execute.  The first
 * request reuses the handle created by ybgistrescan (which, on the legacy
 * partial-match path, already carries its binds).
 */
static void
ybgistStartNextRequest(IndexScanDesc scan, ScanDirection dir)
{
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;
	int			req = ybso->yb_next_req;

	Assert(req < ybso->yb_total_reqs);

	if (req > 0)
	{
		/*
		 * DocDB ANDs every condition bound to one request, so each disjoint
		 * span needs a fresh select request.  Drop the drained handle and
		 * build a new one (pushdowns reapplied by ybgistInitHandle).
		 *
		 * CRITICAL: YBCPgNewSelect registers the statement in the CURRENT PG
		 * memory context, and gettuple runs in a short-lived per-tuple
		 * context -- a statement created there is freed when that context
		 * resets, leaving ybso->handle dangling (SIGSEGV on the next fetch).
		 * Create the replacement handle in the scan opaque's context, the
		 * same lifetime the first handle got from ybgistrescan.
		 */
		MemoryContext oldctx = MemoryContextSwitchTo(GetMemoryChunkContext(ybso));

		YBCPgDeleteStatement(ybso->handle);
		ybgistInitHandle(scan);
		MemoryContextSwitchTo(oldctx);
	}

	if (!ybso->yb_legacy_bind)
		ybgistBindRequest(scan, req);

	/* targets */
	if (scan->yb_aggrefs != NIL)
		/*
		 * As of 2023-06-28, aggregate pushdown is only implemented for
		 * IndexOnlyScan, not IndexScan.
		 */
		YbDmlAppendTargetsAggregate(scan->yb_aggrefs,
									NULL,
									RelationGetDescr(scan->indexRelation),
									scan->indexRelation,
									scan->xs_want_itup,
									ybso->handle);
	else
		ybgistSetupTargets(scan);

	YbSetCatalogCacheVersion(ybso->handle, YbGetCatalogCacheVersion());
	YbMaybeSetNonSystemTablespaceOid(ybso->handle, scan->indexRelation);

	/* execute select */
	ybgistExecSelect(scan, dir);

	ybso->yb_next_req = req + 1;
}

/*
 * Prepare and request the initial execution of select to pggate.
 */
static bool
ybgistDoFirstExec(IndexScanDesc scan, ScanDirection dir)
{
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	if (!ybgistGetScanKeys(scan))
		return false;

	/* plan the span/probe requests (or apply the legacy prefix binds) */
	ybgistPlanRequests(scan);
	if (ybso->yb_total_reqs == 0)
		return false;

	ybgistStartNextRequest(scan, dir);

	return true;
}

/*
 * Fetch the next tuple from pggate.
 */
static HeapTuple
ybgistFetchNextHeapTuple(IndexScanDesc scan)
{
	bool		has_data = false;
	bool	   *nulls;
	Datum	   *values;
	HeapTuple	tuple = NULL;
	TupleDesc	tupdesc;
	YbcPgSysColumns syscols;
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	/*
	 * As an IndexScan, target the base table.  Change this if we ever support
	 * ybgist IndexOnlyScan.
	 */
	tupdesc = RelationGetDescr(scan->heapRelation);
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));

	HandleYBStatus(YBCPgDmlFetch(ybso->handle,
								 tupdesc->natts,
								 (uint64_t *) values,
								 nulls,
								 &syscols,
								 &has_data));
	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		tuple->t_tableOid = RelationGetRelid(scan->heapRelation);
		if (syscols.ybctid != NULL)
			HEAPTUPLE_YBCTID(tuple) = PointerGetDatum(syscols.ybctid);
	}
	pfree(values);
	pfree(nulls);

	return tuple;
}

/*
 * YB spatial de-dup: hash set of ybctids already returned by this scan.
 * A cell-covering scan binds several covering cells (IN condition), so a base
 * row whose bbox spans several matched cells is fetched once per cell.  We key
 * a hash table by the ybctid bytes (variable length) via a pointer key.
 */
typedef struct YbgistCtidKey
{
	struct varlena *ctid;
} YbgistCtidKey;

static uint32
ybgistCtidHash(const void *key, Size keysize)
{
	const struct varlena *v = ((const YbgistCtidKey *) key)->ctid;

	return hash_bytes((const unsigned char *) VARDATA_ANY(v),
					  (int) VARSIZE_ANY_EXHDR(v));
}

static int
ybgistCtidMatch(const void *a, const void *b, Size keysize)
{
	const struct varlena *va = ((const YbgistCtidKey *) a)->ctid;
	const struct varlena *vb = ((const YbgistCtidKey *) b)->ctid;
	Size		la = VARSIZE_ANY_EXHDR(va);
	Size		lb = VARSIZE_ANY_EXHDR(vb);

	if (la != lb)
		return 1;
	return memcmp(VARDATA_ANY(va), VARDATA_ANY(vb), la);
}

/*
 * Returns true if this row (by ybctid) was already returned by the current
 * scan; otherwise records it and returns false.  Rows without a ybctid are
 * treated as unique.
 */
static bool
ybgistTupleAlreadySeen(YbgistScanOpaque ybso, HeapTuple tup)
{
	Datum		ybctid = HEAPTUPLE_YBCTID(tup);
	MemoryContext ctx;
	struct varlena *v;
	struct varlena *vcopy;
	Size		sz;
	YbgistCtidKey probe;
	bool		found;

	if (ybctid == (Datum) 0)
		return false;

	ctx = GetMemoryChunkContext(ybso);
	if (ybso->yb_seen_ctids == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(YbgistCtidKey);
		ctl.entrysize = sizeof(YbgistCtidKey);
		ctl.hash = ybgistCtidHash;
		ctl.match = ybgistCtidMatch;
		ctl.hcxt = ctx;
		ybso->yb_seen_ctids = hash_create("ybgist ybctid dedup", 256, &ctl,
										  HASH_ELEM | HASH_FUNCTION |
										  HASH_COMPARE | HASH_CONTEXT);
	}

	v = (struct varlena *) DatumGetPointer(ybctid);
	sz = VARSIZE_ANY(v);
	vcopy = (struct varlena *) MemoryContextAlloc(ctx, sz);
	memcpy(vcopy, v, sz);
	probe.ctid = vcopy;

	(void) hash_search(ybso->yb_seen_ctids, &probe, HASH_ENTER, &found);
	if (found)
	{
		pfree(vcopy);			/* key already stored; drop our copy */
		return true;
	}
	return false;
}

bool
ybgistgettuple(IndexScanDesc scan, ScanDirection dir)
{
	HeapTuple	tup;
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	/* Sanity check: amcanbackward. */
	Assert(!ScanDirectionIsBackward(dir));

	if (!ybso->is_exec_done)
	{
		if (!ybgistDoFirstExec(scan, dir))
			return false;
		ybso->is_exec_done = true;
	}

	/* fetch */
	if (scan->yb_aggrefs)
	{
		/*
		 * TODO(jason): don't assume that recheck is needed.
		 */
		scan->xs_recheck = true;

		/*
		 * Aggregate pushdown directly modifies the scan slot rather than
		 * passing it through xs_hitup or xs_itup.
		 */
		return ybc_getnext_aggslot(scan, ybso->handle, scan->xs_want_itup);
	}
	for (;;)
	{
		while (HeapTupleIsValid(tup = ybgistFetchNextHeapTuple(scan)))
		{
			/*
			 * A row can match several probe cells/spans (and thus be returned
			 * by more than one request).  Skip rows already returned by this
			 * scan; the de-dup set persists across all requests.
			 */
			if (ybgistTupleAlreadySeen(ybso, tup))
			{
				heap_freetuple(tup);
				continue;
			}

			scan->xs_hitup = tup;
			scan->xs_hitupdesc = RelationGetDescr(scan->heapRelation);

			/* TODO(jason): don't assume that recheck is needed. */
			scan->xs_recheck = true;
			return true;
		}

		/* current request drained; move on to the next span, if any */
		if (ybso->yb_next_req >= ybso->yb_total_reqs)
			return false;
		ybgistStartNextRequest(scan, dir);
	}
}

/*
 * TODO(jason): don't assume that recheck is needed.
 */
bool
ybgistmightrecheck(Scan *scan, Relation heapRelation, Relation indexRelation,
				  bool xs_want_itup, ScanKey keys, int nkeys)
{
	return true;
}
