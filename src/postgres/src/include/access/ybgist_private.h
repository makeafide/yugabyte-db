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
} YbgistScanOpaqueData;

typedef YbgistScanOpaqueData *YbgistScanOpaque;

extern char *ybgistNullCategoryToString(GinNullCategory category);
extern char *ybgistSearchModeToString(int32 searchMode);
