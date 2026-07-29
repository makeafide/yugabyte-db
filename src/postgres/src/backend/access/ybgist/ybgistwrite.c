/*-------------------------------------------------------------------------
 *
 * ybgistwrite.c
 *	  insert and delete routines for the Yugabyte inverted index access method.
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
 *			src/backend/access/ybgist/ybgistwrite.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/sysattr.h"
#include "access/yb_scan.h"
#include "access/ybgist_private.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "catalog/yb_type.h"
#include "executor/ybModifyTable.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "pg_yb_utils.h"
#include "storage/off.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"

/*
 * Parts copied from GinBuildState.  Differences:
 * - no buildStats because those are tied to postgres storage
 * - no tmpCtx because that's tied to bulk inserts, which we won't do because
 *   it seems to be particularly beneficial for postgres btrees and not for
 *   Yugabyte DocDB
 * - no accum for the same reason
 * - add backfilltime to both indicate that the build is for online index
 *   backfill and specify the write time for it
 */
typedef struct
{
	GinState	ginstate;
	double		indtuples;
	MemoryContext funcCtx;
	uint64_t   *backfilltime;
} YbgistBuildState;

/*
 * Utility method to set binds for index write statement.
 */
static void
doBindsForIdxWrite(YbcPgStatement stmt,
				   void *indexstate,
				   Relation index,
				   Datum *values,
				   bool *isnull,
				   int n_bound_atts,
				   Datum ybbasectid,
				   bool ybctid_as_value)
{
	GinState   *ginstate = (GinState *) indexstate;
	TupleDesc	tupdesc = RelationGetDescr(index);

	if (ybbasectid == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("missing base table ybctid in index write request")));

	for (AttrNumber attnum = 1; attnum <= n_bound_atts; ++attnum)
	{
		Oid			type_id = GetTypeId(attnum, tupdesc);
		Oid			collation_id = YBEncodingCollation(stmt, attnum,
													   ginstate->supportCollation[attnum - 1]);
		Datum		value = values[attnum - 1];
		bool		is_null = isnull[attnum - 1];

		YbBindDatumToColumn(stmt, attnum, type_id, collation_id, value,
							is_null, &YBCGinNullTypeEntity);
	}

	/* Gin indexes cannot be unique. */
	Assert(!index->rd_index->indisunique);

	/* Write base ctid column because it is a key column. */
	YbBindDatumToColumn(stmt,
						YBIdxBaseTupleIdAttributeNumber,
						BYTEAOID,
						InvalidOid,
						ybbasectid,
						false /* is_null */ ,
						NULL /* null type_entity */ );
}

/*
 * Write one base row's index entries.
 *
 * Multicolumn shape (ybgistCheckShape): the LAST key column is the spatial
 * one; its extracted cells fan out into one DocDB index row each, while the
 * leading scalar columns are replicated verbatim into every emitted row --
 * a composite (lead1, ..., cell, basectid) range key.  NULL/empty spatial
 * values become one GIN null-category row (isnull cell), so the row stays
 * reachable by leading-column-only scans.
 */
static int32
ybgistRowWrite(GinState *ginstate, Relation index,
			   Datum *values, bool *isnull,
			   Datum ybctid, uint64_t *backfilltime,
			   bool isinsert)
{
	int			natts = ginstate->origTupdesc->natts;
	OffsetNumber spatialatt = (OffsetNumber) natts;
	Datum	   *entries;
	GinNullCategory *categories;
	Datum	   *rowvals;
	bool	   *rownull;
	int32		i,
				nentries;

	entries = ginExtractEntries(ginstate, spatialatt,
								values[natts - 1], isnull[natts - 1],
								&nentries, &categories);

	rowvals = (Datum *) palloc(natts * sizeof(Datum));
	rownull = (bool *) palloc(natts * sizeof(bool));
	for (i = 0; i < natts - 1; i++)
	{
		rowvals[i] = values[i];
		rownull[i] = isnull[i];
	}

	for (i = 0; i < nentries; i++)
	{
		/*
		 * Pass the null category down using the spot where the data usually
		 * goes.
		 */
		rowvals[natts - 1] = (categories[i] != GIN_CAT_NORM_KEY)
			? categories[i] : entries[i];
		rownull[natts - 1] = categories[i] != 0;

		if (isinsert)
			YBCExecuteInsertIndex(index, rowvals, rownull, ybctid,
								  backfilltime /* backfill_write_time */ ,
								  doBindsForIdxWrite, (void *) ginstate);
		else
		{
			Assert(!backfilltime);
			YBCExecuteDeleteIndex(index, rowvals, rownull, ybctid,
								  doBindsForIdxWrite, (void *) ginstate);
		}
	}

	pfree(rowvals);
	pfree(rownull);
	return nentries;
}

/*
 * Callback to insert index tuples after a base table tuple is retrieved.  See
 * similar ybcinbuildCallback.
 */
static void
ybgistBuildCallback(Relation index, Datum ybctid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *state)
{
	YbgistBuildState *buildstate = (YbgistBuildState *) state;
	GinState   *ginstate = &buildstate->ginstate;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->funcCtx);
	buildstate->indtuples += ybgistRowWrite(ginstate, index, values, isnull,
											ybctid, buildstate->backfilltime,
											true /* isinsert */ );

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->funcCtx);
}

/*
 * Build code for both ybgistbuild and ybgistbackfill.
 *
 * Parts copied from ginbuild.  Differences are
 * - don't deal with postgres storage (e.g. buffers, pages, tmpCtx)
 * - additionally pass through backfill parameters
 * - name memory context Ybgist
 * - use yb_table_index_build_scan, not table_index_build_scan
 */
static IndexBuildResult *
ybgistBuildCommon(Relation heap, Relation index, struct IndexInfo *indexInfo,
				 struct YbBackfillInfo *bfinfo,
				 struct YbPgExecOutParam *bfresult)
{
	IndexBuildResult *result;
	double		reltuples = 0;
	YbgistBuildState buildstate;

	buildstate.indtuples = 0;

	/*
	 * We don't need to build YB indexes during a major version upgrade, as we
	 * simply link the old DocDB table on master.
	 */
	if (!IsBinaryUpgrade)
	{
		ybgistCheckShape(index);
		initGinState(&buildstate.ginstate, index);
		if (bfinfo)
			buildstate.backfilltime = &bfinfo->read_time;
		else
			buildstate.backfilltime = NULL;

		/*
		 * create a temporary memory context that is used for calling
		 * ginExtractEntries(), and can be reset after each tuple
		 */
		buildstate.funcCtx = AllocSetContextCreate(CurrentMemoryContext,
												   "Ybgist build temporary context for user-defined function",
												   ALLOCSET_DEFAULT_SIZES);

		/*
		 * Do the heap scan.
		 */
		if (!bfinfo)
			reltuples = yb_table_index_build_scan(heap, index, indexInfo, true,
												  ybgistBuildCallback,
												  (void *) &buildstate,
												  NULL /* HeapScanDesc */ );
		else
			reltuples = IndexBackfillHeapRangeScan(heap, index, indexInfo,
												   ybgistBuildCallback,
												   (void *) &buildstate,
												   bfinfo,
												   bfresult);

		MemoryContextDelete(buildstate.funcCtx);
	}

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

IndexBuildResult *
ybgistbuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	return ybgistBuildCommon(heap, index, indexInfo,
							NULL /* bfinfo */ , NULL /* bfresult */ );
}

void
ybgistbuildempty(Relation index)
{
	YBC_LOG_WARNING("Unexpected building of empty unlogged index");
}

IndexBulkDeleteResult *
ybgistbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				IndexBulkDeleteCallback callback, void *callback_state)
{
	YBC_LOG_WARNING("Unexpected bulk delete of index via vacuum");
	return NULL;
}

IndexBulkDeleteResult *
ybgistvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	YBC_LOG_WARNING("Unexpected index cleanup via vacuum");
	return NULL;
}

/*
 * Write code for both ybgistinsert and ybgistdelete.
 *
 * Parts copied from gininsert.  Differences are
 * - don't copy fastupdate code since it's not supported
 * - additionally handle deletes
 * - name memory context Ybgist
 */
static void
ybgistWrite(Relation index, Datum *values, bool *isnull, Datum ybctid,
		   Relation heap, struct IndexInfo *indexInfo, bool isinsert)
{
	GinState   *ginstate = (GinState *) indexInfo->ii_AmCache;
	MemoryContext oldCtx;
	MemoryContext writeCtx;

	/* Initialize GinState cache if first call in this statement */
	if (ginstate == NULL)
	{
		ybgistCheckShape(index);
		oldCtx = MemoryContextSwitchTo(indexInfo->ii_Context);
		ginstate = (GinState *) palloc(sizeof(GinState));
		initGinState(ginstate, index);
		indexInfo->ii_AmCache = (void *) ginstate;
		MemoryContextSwitchTo(oldCtx);
	}

	writeCtx = AllocSetContextCreate(CurrentMemoryContext,
									 "Ybgist write temporary context",
									 ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(writeCtx);

	/*
	 * No GinGetUseFastUpdate check: ybgist has no reloptions (ybgistoptions
	 * always returns NULL) and the macro's Assert rejects the ybgist AM oid.
	 */
	ybgistRowWrite(ginstate, index, values, isnull, ybctid,
				   NULL /* backfilltime */ , isinsert);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(writeCtx);
}

bool
ybgistinsert(Relation index, Datum *values, bool *isnull, Datum ybctid,
			Relation heap, IndexUniqueCheck checkUnique,
			struct IndexInfo *indexInfo, bool shared_insert)
{
	ybgistWrite(index, values, isnull, ybctid, heap, indexInfo, true /* isinsert */ );

	/* index cannot be unique */
	return false;
}

void
ybgistdelete(Relation index, Datum *values, bool *isnull, Datum ybctid,
			Relation heap, struct IndexInfo *indexInfo)
{
	ybgistWrite(index, values, isnull, ybctid, heap, indexInfo,
			   false /* isinsert */ );
}

IndexBuildResult *
ybgistbackfill(Relation heap, Relation index, struct IndexInfo *indexInfo,
			  struct YbBackfillInfo *bfinfo, struct YbPgExecOutParam *bfresult)
{
	return ybgistBuildCommon(heap, index, indexInfo, bfinfo, bfresult);
}
