/*--------------------------------------------------------------------------
 *
 * ybgist.h
 *	  Public header file for Yugabyte Generalized Inverted Index access method.
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
 *			src/include/access/ybgist.h
 *--------------------------------------------------------------------------
 */

#pragma once

#include "access/amapi.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"

/* ybgist.c */
extern IndexBuildResult *ybgistbuild(Relation heap, Relation index,
									struct IndexInfo *indexInfo);
extern void ybgistbuildempty(Relation index);
extern IndexBulkDeleteResult *ybgistbulkdelete(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats,
											  IndexBulkDeleteCallback callback,
											  void *callback_state);
extern IndexBulkDeleteResult *ybgistvacuumcleanup(IndexVacuumInfo *info,
												 IndexBulkDeleteResult *stats);
extern void ybgistcostestimate(struct PlannerInfo *root,
							  struct IndexPath *path,
							  double loop_count,
							  Cost *indexStartupCost,
							  Cost *indexTotalCost,
							  Selectivity *indexSelectivity,
							  double *indexCorrelation,
							  double *indexPages);
extern bytea *ybgistoptions(Datum reloptions, bool validate);
extern bool ybgistvalidate(Oid opclassoid);
extern void ybgistbindschema(YbcPgStatement handle,
							struct IndexInfo *indexInfo,
							TupleDesc indexTupleDesc,
							int16 *coloptions,
							Oid *opclassIds,
							Datum reloptions);
extern IndexScanDesc ybgistbeginscan(Relation rel, int nkeys, int norderbys);
extern void ybgistrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						ScanKey orderbys, int norderbys);
extern bool ybgistgettuple(IndexScanDesc scan, ScanDirection dir);
extern void ybgistendscan(IndexScanDesc scan);
extern bool ybgistinsert(Relation rel, Datum *values, bool *isnull,
						Datum ybctid, Relation heapRel,
						IndexUniqueCheck checkUnique,
						struct IndexInfo *indexInfo, bool shared_insert);
extern void ybgistdelete(Relation rel, Datum *values, bool *isnull,
						Datum ybctid, Relation heapRel,
						struct IndexInfo *indexInfo);
extern IndexBuildResult *ybgistbackfill(Relation heap, Relation index,
									   struct IndexInfo *indexInfo,
									   struct YbBackfillInfo *bfinfo,
									   struct YbPgExecOutParam *bfresult);
extern bool ybgistmightrecheck(Scan *scan,
							  Relation heapRelation, Relation indexRelation,
							  bool xs_want_itup, ScanKey keys, int nkeys);
