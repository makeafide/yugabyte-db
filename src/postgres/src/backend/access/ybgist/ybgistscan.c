/*--------------------------------------------------------------------------
 *
 * ybgistscan.c
 *	  routines to manage scans of Yugabyte inverted index relations
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
 *			src/backend/access/ybgist/ybgistscan.c
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/yb_scan.h"
#include "access/ybgist.h"
#include "access/ybgist_private.h"
#include "commands/yb_tablegroup.h"
#include "miscadmin.h"
#include "pg_yb_utils.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "yb/yql/pggate/ybc_pggate.h"

/*
 * Parts copied from ginscan.c ginbeginscan.  Do the same thing except
 * - palloc size of YbgistScanOpaqueData
 * - name memory contexts Ybgist
 */
IndexScanDesc
ybgistbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	GinScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (GinScanOpaque) palloc(sizeof(YbgistScanOpaqueData));
	so->keys = NULL;
	so->nkeys = 0;
	so->tempCtx = AllocSetContextCreate(CurrentMemoryContext,
										"Ybgist scan temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->keyCtx = AllocSetContextCreate(CurrentMemoryContext,
									   "Ybgist scan key context",
									   ALLOCSET_DEFAULT_SIZES);
	initGinState(&so->ginstate, scan->indexRelation);

	/* YB spatial: ybctid de-dup set is created lazily on first fetch. */
	((YbgistScanOpaque) so)->yb_seen_ctids = NULL;

	/* YB spatial: range+probe request plan (filled at first exec). */
	((YbgistScanOpaque) so)->yb_probes = NULL;
	((YbgistScanOpaque) so)->yb_nprobes = 0;
	((YbgistScanOpaque) so)->yb_span_lo = NULL;
	((YbgistScanOpaque) so)->yb_span_hi = NULL;
	((YbgistScanOpaque) so)->yb_nspans = 0;
	((YbgistScanOpaque) so)->yb_total_reqs = 0;
	((YbgistScanOpaque) so)->yb_next_req = 0;
	((YbgistScanOpaque) so)->yb_legacy_bind = false;

	scan->opaque = so;

	return scan;
}

/*
 * YB spatial: create a fresh DocDB select handle for this index scan and apply
 * the pushdowns.  Called from ybgistrescan for the first request and from the
 * range+probe scan path (ybgistget.c) for each subsequent request -- DocDB
 * ANDs all conditions bound to one request, so every disjoint cell span needs
 * its own request/handle.
 */
void
ybgistInitHandle(IndexScanDesc scan)
{
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;
	YbcPgPrepareParameters prepare_params = {
		.index_relfilenode_oid = YbGetRelfileNodeId(scan->indexRelation),
		.index_only_scan = scan->xs_want_itup,
		.embedded_idx = YbIsScanningEmbeddedIdx(scan->heapRelation,
												scan->indexRelation),
	};

	HandleYBStatus(YBCPgNewSelect(YBCGetDatabaseOid(scan->heapRelation),
								  YbGetRelfileNodeId(scan->heapRelation),
								  &prepare_params,
								  YBCIsRegionLocal(scan->heapRelation),
								  &ybso->handle));
	YbApplyPrimaryPushdown(ybso->handle, scan->yb_rel_pushdown);
	YbApplySecondaryIndexPushdown(ybso->handle, scan->yb_idx_pushdown);
}

void
ybgistrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	YbgistScanOpaque ybso = (YbgistScanOpaque) scan->opaque;

	/* Initialize non-yb gin scan opaque fields. */
	ginrescan(scan, scankey, nscankeys, orderbys, norderbys);

	/* Initialize ybgist scan opaque handle. */
	ybgistInitHandle(scan);

	/* YB spatial: drop any de-dup set from a previous scan iteration. */
	if (ybso->yb_seen_ctids != NULL)
	{
		hash_destroy(ybso->yb_seen_ctids);
		ybso->yb_seen_ctids = NULL;
	}

	/* YB spatial: reset the range+probe request plan. */
	ybso->yb_probes = NULL;
	ybso->yb_nprobes = 0;
	ybso->yb_span_lo = NULL;
	ybso->yb_span_hi = NULL;
	ybso->yb_nspans = 0;
	ybso->yb_total_reqs = 0;
	ybso->yb_next_req = 0;
	ybso->yb_legacy_bind = false;

	/* Initialize ybgist scan opaque is_exec_done. */
	ybso->is_exec_done = false;
}

void
ybgistendscan(IndexScanDesc scan)
{
	/*
	 * This frees the scan opaque as if it were a GinScanOpaque rather than a
	 * YbgistScanOpaque, but that's fine since the extra field HANDLE doesn't
	 * need special handling.
	 */
	ginendscan(scan);
}
