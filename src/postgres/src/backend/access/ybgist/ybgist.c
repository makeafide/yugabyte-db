/*--------------------------------------------------------------------------
 *
 * ybgist.c
 *	  Implementation of Yugabyte Generalized Inverted Index access method.
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
 *			src/backend/access/ybgist/ybgist.c
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/gin.h"
#include "access/ybgist.h"
#include "fmgr.h"
#include "nodes/nodes.h"
#include "postgres_ext.h"

/*
 * YBGIST handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
ybgisthandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = GINNProcs;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;	/* leading equality cols + trailing
										 * spatial col (ybgistCheckShape) */
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = true;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = true;	/* TODO(jason): check what this is */
	amroutine->amcanparallel = false;
	amroutine->amcaninclude = false;
	amroutine->ybamcanupdatetupleinplace = false;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = ybgistbuild;
	amroutine->ambuildempty = ybgistbuildempty;
	amroutine->aminsert = NULL; /* use yb_aminsert below instead */
	amroutine->ambulkdelete = ybgistbulkdelete;
	amroutine->amvacuumcleanup = ybgistvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = ybgistcostestimate;
	amroutine->amoptions = ybgistoptions;
	amroutine->amproperty = NULL;
	amroutine->amvalidate = ybgistvalidate;
	amroutine->ambeginscan = ybgistbeginscan;
	amroutine->amrescan = ybgistrescan;
	amroutine->amgettuple = ybgistgettuple;
	amroutine->amgetbitmap = NULL;	/* TODO(jason): support bitmap scan */
	amroutine->amendscan = ybgistendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;
	amroutine->yb_amisforybrelation = true;
	amroutine->yb_aminsert = ybgistinsert;
	amroutine->yb_amdelete = ybgistdelete;
	amroutine->yb_amupdate = NULL;
	amroutine->yb_ambackfill = ybgistbackfill;
	amroutine->yb_ammightrecheck = ybgistmightrecheck;
	amroutine->yb_ambindschema = ybgistbindschema;

	PG_RETURN_POINTER(amroutine);
}
