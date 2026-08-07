# ybgist branch — native spatial index AM for YSQL

This branch (`ybgist/2025.2.6.0`, based on upstream branch 2025.2 at 2025.2.6.0) adds **ybgist**, a
native spatial index access method for YugabyteDB: PostGIS geometries/geographies are
decomposed into S2-style hierarchical cell ids stored in a distributed DocDB index table and
scanned with range+probe matching (descendant-span `BETWEEN` requests + ancestor `IN`
probes, leading-column equality prefix for multicolumn indexes), giving transparent index
acceleration for `ST_Contains` / `ST_Intersects` / `ST_Within` / `ST_DWithin` / `&&` / …
with exact executor recheck.

**This branch is only half of the system.** The opclass extension
(`CREATE EXTENSION ybgist`), the patched PostGIS build, the install/build guides, the test
harness, and the releases all live in **[makeafide/yb-pggist](https://github.com/makeafide/yb-pggist)**
— start with its README.

## Install / use

You cannot add ybgist to a stock YugabyteDB install: the AM's catalog rows
(`pg_am.dat` oid **8121**, `pg_proc.dat` oid **8122**) are baked into the initdb
system-catalog snapshot, so clusters must be initialized from a build of this branch.

- **From released artifacts** (fork tarball + geo bundle):
  [yb-pggist docs/INSTALL-RELEASE.md](https://github.com/makeafide/yb-pggist/blob/main/docs/INSTALL-RELEASE.md)
  — current release: [`ybgist-2025.2.5.1-r2`](https://github.com/makeafide/yugabyte-db/releases/tag/ybgist-2025.2.5.1-r2)
  (note: r2 predates multicolumn/geography/cube — build from source for those until r3).
- **From source**:
  [yb-pggist docs/BUILD-FROM-SOURCE.md](https://github.com/makeafide/yb-pggist/blob/main/docs/BUILD-FROM-SOURCE.md)
  — core build one-liner:

  ```sh
  YB_LINKING_TYPE=dynamic ./yb_build.sh release --no-tests --skip-extra-pg-extensions -j "$(nproc)"
  ```

- **Usage, configuration, and the operational runbook** (per-database setup, tuning GUCs,
  drift guards, backfill verification):
  [yb-pggist README](https://github.com/makeafide/yb-pggist#readme).

Build notes for this branch:
- after editing the catalog `.dat` files, force the initdb snapshot regen
  (`rm -rf build/latest/share/initial_sys_catalog_snapshot*`, then `./yb_build.sh release initdb`);
- after editing ybgist C files, `rm build/latest/postgres_build/build_stamp` to defeat the
  git-diff build stamp;
- packaging with `yb_release`: stage any foreign geo `.so`s out of
  `build/latest/postgres/lib` first (the packager rejects their libc++ rpaths);
- `--skip-extra-pg-extensions` skips documentdb and pg_parquet, which this branch does not
  use and which have historically broken local release builds. It is upstream's own flag
  (`YB_SKIP_EXTRA_PG_EXTENSIONS`), so the branch carries no patch to
  `third-party-extensions/Makefile`; it does not change the `-D` set the geo stack is
  compiled against (`YB_ENABLE_YSQL_DOCUMENTDB_EXT` is set unconditionally in `CMakeLists.txt`).

## What this branch changes

| Area | Files | What |
|---|---|---|
| The AM | `src/postgres/src/backend/access/ybgist/` + `src/include/access/ybgist*.h` | ybgin-model write path (composite rows: leading scalar columns + spatial cell fan-out), multi-request range+probe scan with per-request equality replay and cross-request ybctid de-dup, tightest-key selection for multi-qual scans, `ybgistCheckShape` (spatial column must be last), scale-aware cost estimate |
| Catalog | `src/include/catalog/pg_am.dat` (8121), `pg_proc.dat` (8122) | AM registration (fork-distribution model — see above) |
| Cost model | `optimizer/path/costsize.c`, `utils/misc/guc.c`, `include/optimizer/cost.h` | GUCs `yb_ybgist_recheck_fetch_coef` (0.0028) / `yb_ybgist_recheck_scale_exp` (0.43), fit to measured crossovers at 2e5/1e7 rows; `yb_ybgist_request_cost` — each additional DocDB request of a multi-span scan, estimated at plan time via the opclass extractQuery |
| DocDB | `src/yb/dockv/pg_key_decoder.cc` | decode `kGinNull` key entries as SQL NULL (required for unconstrained multicolumn scans over GIN null-category rows) |

## Validation summary

Correctness idx==seq across a 157-case predicate matrix at 200k/10M rows (spot-verified at
100M), edge geometries (globe-spanning, antimeridian, poles, empty/degenerate),
UPDATE/DELETE churn, concurrent online backfill; multi-scan-key scans (e.g. explicit `&&`
plus the support-fn-derived `~` from `ST_Covers`) bind the tightest key and recheck the
rest; multicolumn (tenant equality prefix + spatial, incl. NULL-geom reachability),
geography opclass (geodesic-bulge-safe coverings), opt-in sphere-cube mapping
(property-tested conservative coverings, 0 false negatives); measured cost model with
planner/measured crossover agreement 14/14 in both raw-`&&` and predicate-function
phrasings. RF=3: full matrix re-pass (10M backfill 389s) plus node-kill during
query/backfill/cube/multicolumn/geography streams, tablet auto-split, and restart recovery
— all green. Full history and measured numbers:
[yb-pggist docs/ybgist-changelist.md](https://github.com/makeafide/yb-pggist/blob/main/docs/ybgist-changelist.md).
