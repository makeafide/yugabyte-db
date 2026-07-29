# ybgist branch — native spatial index AM for YSQL

This branch (`ybgist/2025.2.5.1`, based on upstream tag 2025.2.5.1) adds **ybgist**, a native
spatial index access method: PostGIS geometries are decomposed into S2-style hierarchical cell
ids stored in a DocDB index table, scanned with range+probe matching (descendant-span
`BETWEEN` requests + ancestor `IN` probes), giving transparent index acceleration for
`ST_Contains` / `ST_Intersects` / `ST_Within` / `&&` / … with exact recheck.

Everything else — the opclass extension (`CREATE EXTENSION ybgist`), the PostGIS patch, the
build pipeline, tests, docs, and releases — lives in
**[makeafide/yb-pggist](https://github.com/makeafide/yb-pggist)**:

- Install from releases: [docs/INSTALL-RELEASE.md](https://github.com/makeafide/yb-pggist/blob/main/docs/INSTALL-RELEASE.md)
- Build from source: [docs/BUILD-FROM-SOURCE.md](https://github.com/makeafide/yb-pggist/blob/main/docs/BUILD-FROM-SOURCE.md)
- Prebuilt distribution of this branch: release
  [`ybgist-2025.2.5.1-r2`](https://github.com/makeafide/yugabyte-db/releases/tag/ybgist-2025.2.5.1-r2)

## What this branch changes

- `src/postgres/src/backend/access/ybgist/` + `src/include/access/ybgist*.h` — the AM: ybgin-
  model write path, multi-request range+probe scan with cross-request ybctid de-dup, scale-
  aware cost estimate.
- `src/include/catalog/pg_am.dat` (oid **8121**) and `pg_proc.dat` (oid **8122**) — catalog
  registration. Because these live in the initdb sys-catalog snapshot, a stock YB release can
  never load ybgist; distributions must be built from this branch.
- `optimizer/path/costsize.c`, `utils/misc/guc.c`, `include/optimizer/cost.h` — cost GUCs
  `yb_ybgist_recheck_fetch_coef` (0.0028) / `yb_ybgist_recheck_scale_exp` (0.43) /
  `yb_ybgist_request_cost` (per additional DocDB request of a multi-span scan).
- Multicolumn: `amcanmulticol` — N leading scalar equality columns + one trailing spatial
  column (`USING ybgist(tenant_id, geom)`), shape-enforced by `ybgistCheckShape`.
- `dockv/pg_key_decoder.cc` — decode `kGinNull` key entries as SQL NULL (required for
  unconstrained multicolumn index scans over GIN null-category rows).
- `third-party-extensions/Makefile` — local build workaround (skips pg_parquet); not for
  upstreaming.

## Build

```sh
YB_LINKING_TYPE=dynamic ./yb_build.sh release --no-tests -j "$(nproc)"
```

Notes: after editing the catalog `.dat` files, force the initdb snapshot regen
(`rm -rf build/latest/share/initial_sys_catalog_snapshot …` then `./yb_build.sh release
initdb`); after editing ybgist C files, `rm build/latest/postgres_build/build_stamp` to defeat
the git-diff build stamp.

## Validation summary

Correctness idx==seq across a 157-case predicate matrix at 175k/10M rows (spot-verified at
100M), edge geometries (globe-spanning, antimeridian, poles, empty/degenerate), UPDATE/DELETE
churn, concurrent online backfill; multi-scan-key scans (several indexable quals on one geom
column — e.g. explicit `&&` plus the support-fn-derived `~` from `ST_Covers`) bind the
tightest key and recheck the rest; multicolumn (tenant equality prefix + spatial), geography
opclass, opt-in sphere-cube mapping (property-tested conservative coverings), and a
measured/refit cost model (crossover matrix 14/14 in both raw-`&&` and predicate-function
phrasings); RF=3: full matrix re-pass plus node-kill during queries/backfill, tablet
auto-splits, and restart recovery — all green. Details:
[yb-pggist docs/ybgist-changelist.md](https://github.com/makeafide/yb-pggist/blob/main/docs/ybgist-changelist.md).
