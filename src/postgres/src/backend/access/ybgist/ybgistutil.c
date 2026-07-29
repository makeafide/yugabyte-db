/*--------------------------------------------------------------------------
 *
 * ybgistutil.c
 *	  Utility routines for the Yugabyte inverted index access method.
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
 *			src/backend/access/ybgist/ybgistutil.c
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/gin_private.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/ybgist_private.h"
#include "commands/yb_cmds.h"
#include "nodes/execnodes.h"
#include "nodes/nodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

/*
 * Per-candidate recheck cost for ybgist.  Every candidate is a random DocDB row
 * fetch + an exact recheck.  Crucially the fetch cost GROWS with table size: at
 * larger scale candidates scatter across more tablets/SST files, so the measured
 * per-candidate time rose ~8us -> ~34us as rows grew 1.75e5 -> 1e7, and the
 * index-vs-seqscan crossover selectivity fell from ~8% to ~2%.  A fixed fraction
 * cannot track that, so model the per-candidate fetch as
 *     yb_network_fetch_cost * COEF * tuples^EXP
 * with COEF/EXP fit to the measured crossovers at 2e5 and 1e7 rows on RF=3
 * with cover-only (r2) indexes.  Plus the recheck qual eval.  See
 * ybgistcostestimate.  COEF/EXP are the GUCs yb_ybgist_recheck_fetch_coef /
 * yb_ybgist_recheck_scale_exp (costsize.c) so recalibration on different
 * topologies needs no rebuild.  Broad queries phrased with PostGIS predicate
 * *functions* additionally need realistic procosts (PostGIS ships 5000,
 * ~100-1000x above measured) -- apply ybgist_tune_costs() from the ybgist
 * extension, else the inflated seqscan estimate keeps the index preferred
 * past its real crossover.  A multi-span covering's extra round trips are
 * charged separately via yb_ybgist_request_cost below.
 */

void
ybgistcostestimate(struct PlannerInfo *root, struct IndexPath *path,
				  double loop_count, Cost *indexStartupCost,
				  Cost *indexTotalCost, Selectivity *indexSelectivity,
				  double *indexCorrelation, double *indexPages)
{
	List	   *indexQuals;
	QualCost	qual_cost;
	double		baserel_tuples;
	double		candidates;

	Assert(!path->yb_index_path_info.merge_scan_saop_cols);

	gincostestimate(root, path, loop_count, indexStartupCost, indexTotalCost,
					indexSelectivity, indexCorrelation, indexPages);

	/*
	 * ybgist over-covers each geometry with adaptive cells and ALWAYS rechecks
	 * every candidate heap tuple against the exact indexable operator (unlike
	 * GIN, which returns exact TIDs and needs no recheck).  gincostestimate
	 * therefore omits the recheck entirely, and the generic cost_index() heap-
	 * fetch term also collapses in YugabyteDB because DocDB tables expose ~no PG
	 * heap pages -- so a broad ybgist scan is badly under-costed against a
	 * seqscan and the planner wrongly keeps the index up to ~100% selectivity.
	 *
	 * Charge each estimated candidate a (batched) DocDB row fetch plus the exact
	 * recheck qual evaluation.  Selective scans (few candidates) are essentially
	 * unaffected; only broad scans gain enough cost to correctly lose to a
	 * seqscan, matching the measured crossover.
	 */
	indexQuals = get_quals_from_indexclauses(path->indexclauses);
	cost_qual_eval(&qual_cost, indexQuals, root);
	baserel_tuples = path->indexinfo->rel->tuples;
	candidates = (*indexSelectivity) * baserel_tuples;
	if (candidates > 0.0 && baserel_tuples > 1.0)
	{
		double		per_cand_fetch = yb_network_fetch_cost *
			yb_ybgist_recheck_fetch_coef *
			pow(baserel_tuples, yb_ybgist_recheck_scale_exp);

		*indexTotalCost += candidates *
			(per_cand_fetch + cpu_tuple_cost + qual_cost.per_tuple);
	}

	/*
	 * Charge the sequential round trips of a multi-span scan: a query
	 * covering of N coalesced cell spans (plus an ancestor-probe batch)
	 * executes as N+1 DocDB requests, one after another (ybgistPlanRequests).
	 * Estimate the request count by running the opclass extractQuery support
	 * function on the first constant index qual -- the same covering the scan
	 * itself will use.  Parameterized quals are skipped (unknown at plan
	 * time); the scan binds a single key, so the first constant clause is
	 * representative.
	 */
	{
		ListCell   *lc;
		bool		charged = false;

		foreach(lc, path->indexclauses)
		{
			IndexClause *iclause = (IndexClause *) lfirst(lc);
			ListCell   *lc2;

			foreach(lc2, iclause->indexquals)
			{
				RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
				OpExpr	   *clause;
				Node	   *operand;
				Relation	indexRel;
				FmgrInfo   *extractProc;
				Oid			collation;
				StrategyNumber strategy;
				Datum	   *entries;
				int32		nentries = 0;
				bool	   *partial_matches = NULL;
				Pointer	   *extra_data = NULL;
				bool	   *null_flags = NULL;
				int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
				int			nreqs;

				if (!IsA(rinfo->clause, OpExpr))
					continue;
				clause = (OpExpr *) rinfo->clause;
				if (list_length(clause->args) != 2)
					continue;
				operand = (Node *) lsecond(clause->args);
				if (IsA(operand, RelabelType))
					operand = (Node *) ((RelabelType *) operand)->arg;
				if (!IsA(operand, Const) || ((Const *) operand)->constisnull)
					continue;

				indexRel = index_open(path->indexinfo->indexoid,
									  AccessShareLock);
				extractProc = index_getprocinfo(indexRel, iclause->indexcol + 1,
												GIN_EXTRACTQUERY_PROC);
				collation = indexRel->rd_indcollation[iclause->indexcol];
				strategy = get_op_opfamily_strategy(clause->opno,
													indexRel->rd_opfamily[iclause->indexcol]);
				entries = (Datum *)
					DatumGetPointer(FunctionCall7Coll(extractProc,
													  collation,
													  ((Const *) operand)->constvalue,
													  PointerGetDatum(&nentries),
													  UInt16GetDatum(strategy),
													  PointerGetDatum(&partial_matches),
													  PointerGetDatum(&extra_data),
													  PointerGetDatum(&null_flags),
													  PointerGetDatum(&searchMode)));
				index_close(indexRel, AccessShareLock);

				if (entries != NULL && nentries > 0 &&
					searchMode == GIN_SEARCH_MODE_DEFAULT &&
					partial_matches == NULL)
				{
					StaticAssertStmt(sizeof(Datum) == sizeof(int64),
									 "ybgist cell ids require 64-bit Datums");
					nreqs = ybgistEstimateRequests((const int64 *) entries,
												   (int) nentries);
					if (nreqs > 1)
						*indexTotalCost += (nreqs - 1) * yb_ybgist_request_cost;
				}
				charged = true;
				break;
			}
			if (charged)
				break;
		}
	}
}

/*
 * Enforce the supported multicolumn shape: N leading SCALAR equality columns
 * plus exactly one trailing SPATIAL (cell-covering) column.  A spatial
 * opclass is recognized by being type-changing (opcintype geometry/box/
 * geography -> STORAGE int8 cell ids), while the scalar equality opclasses
 * are type-preserving.  The scan and write paths treat the LAST key column
 * as the cell column, so a spatial column anywhere else would silently
 * produce garbage spans (false negatives) -- hence a hard error here.
 * Called from build, write, and beginscan entry points (cheap).
 */
void
ybgistCheckShape(Relation index)
{
	TupleDesc	tupdesc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	int			i;

	for (i = 0; i < natts; i++)
	{
		bool		typechanging = (index->rd_opcintype[i] !=
									TupleDescAttr(tupdesc, i)->atttypid);

		if (i == natts - 1 && !typechanging)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported ybgist index shape"),
					 errdetail("The last column of a ybgist index must use a"
							   " cell-covering (spatial) operator class.")));
		if (i < natts - 1 && typechanging)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported ybgist index shape"),
					 errdetail("The cell-covering (spatial) column must be the"
							   " LAST ybgist index column; leading columns"
							   " must use scalar equality operator"
							   " classes.")));
	}
}

bytea *
ybgistoptions(Datum reloptions, bool validate)
{
	relopt_value *options;
	int			numoptions;
	int			i;

	/*
	 * Rather than creating a new RELOPT_KIND_YBGIN, reuse RELOPT_KIND_GIN and
	 * disallow the options that aren't supported.  The downside is that most
	 * gin options will probably apply to only one of (pg)gin or ybgist.  The
	 * upside is that the unsupported options can be ignored to be compatible
	 * with existing scripts without needing modification.
	 *
	 * Since, currently, we disallow any gin reloptions being set, we should be
	 * able to change our mind later and introduce a RELOPT_KIND_YBGIN if
	 * desired.
	 *
	 * TODO(jason): ignore and clear, rather than error, on unsupported options
	 *
	 * To automatically disallow new options in case upstream postgres creates
	 * them and we import them,
	 * 1. if the relopt kind is not gin, allow
	 * 2. allow specific gin relopts (currently none)
	 * 3. disallow the rest
	 */
	options = parseRelOptions(reloptions, validate, RELOPT_KIND_GIN,
							  &numoptions);
	for (i = 0; i < numoptions; i++)
	{
		if (!options[i].isset)
			continue;
		if (options[i].gen->kinds != RELOPT_KIND_GIN)
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ybgist indexes do not support reloption %s",
						options[i].gen->name)));
	}

	return NULL;
}

bool
ybgistvalidate(Oid opclassoid)
{
	/*
	 * Calling ginvalidate is probably right.  It's hard to tell since it
	 * doesn't work in the first place (issue #8949).
	 *
	 * TODO(jason): when it starts working, refactor to use "ybgist" instead of
	 * "gin" for error messages (probably should create a layer to accept this
	 * string and pass it down).
	 */
	return ginvalidate(opclassoid);
}

void
ybgistbindschema(YbcPgStatement handle,
				struct IndexInfo *indexInfo,
				TupleDesc indexTupleDesc,
				int16 *coloptions,
				Oid *opclassOids,
				Datum reloptions)
{
	YBCBindCreateIndexColumns(handle,
							  indexInfo,
							  indexTupleDesc,
							  coloptions,
							  indexInfo->ii_NumIndexKeyAttrs);
}

/*
 * Given gin null category, return human-readable string.
 */
const char *
ybgistNullCategoryToString(GinNullCategory category)
{
	switch (category)
	{
		case GIN_CAT_NORM_KEY:
			return "normal";
		case GIN_CAT_NULL_KEY:
			return "null-key";
		case GIN_CAT_EMPTY_ITEM:
			return "empty-item";
		case GIN_CAT_NULL_ITEM:
			return "null-item";
		case GIN_CAT_EMPTY_QUERY:
			return "empty-query";
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("unrecognized null category: %d",
							category)));
	}
}

/*
 * Given gin search mode, return human-readable string.
 */
const char *
ybgistSearchModeToString(int32 searchMode)
{
	switch (searchMode)
	{
		case GIN_SEARCH_MODE_DEFAULT:
			return "default";
		case GIN_SEARCH_MODE_INCLUDE_EMPTY:
			return "include-empty";
		case GIN_SEARCH_MODE_ALL:
			return "all";
		case GIN_SEARCH_MODE_EVERYTHING:
			return "everything";
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("unrecognized search mode: %d",
							searchMode)));
	}
}
