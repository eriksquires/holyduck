# Changelog

## v0.4.1 — 2026-04-03

### Highlights

**Streaming query results** — all three query paths (pushed SELECT, pushed DERIVED, and fallback
table scan) now stream results from DuckDB in ~2048-row chunks instead of materializing the
entire result set in memory. For large CTAS operations (e.g. 60M-row lineitem at SF=10), memory
usage plateaus at DuckDB's `memory_limit` (~4 GB) instead of growing linearly to 20+ GB and
OOM-killing the server.

### Bug Fixes

- **select_handler and derived_handler streaming** — replaced `connection->Query()` (which
  returns a `MaterializedQueryResult` holding all rows in memory) with `connection->SendQuery()`
  (which returns a `StreamQueryResult`). Rows are now fetched on demand via `Fetch()`, one
  `DataChunk` at a time. Previous chunks are freed before fetching the next.

- **Type-specific stores in derived_handler** — `derived_handler::next_row()` now uses the same
  type-dispatched `field->store()` calls as `select_handler::next_row()` (integer, float, date
  types stored directly) instead of converting every value through `ToString()`.

- **Missing void* casts in ha_duckdb destructor and close()** — `scan_result` and `scan_chunk`
  are stored as `void*` but were deleted without casting, which is undefined behavior (destructor
  not invoked). Both now use `delete static_cast<duckdb::QueryResult*>(scan_result)` and
  `delete static_cast<duckdb::DataChunk*>(scan_chunk)`. The `close()` path also now cleans up
  `scan_chunk`, which was previously leaked if `close()` ran without `rnd_end()`.

### Release Artifacts

| File | Description |
|---|---|
| `ha_duckdb-v0.4.1-ubuntu.so` | Ubuntu 22.04 (also covers Debian 12) |
| `ha_duckdb-v0.4.1-oracle8.so` | Oracle Linux 8 |
| `ha_duckdb-v0.4.1-oracle9.so` | Oracle Linux 9 |
| `holyduck_duckdb_extensions.sql` | DuckDB macros and views |
| `holyduck_mariadb_functions.sql` | MariaDB stored functions — install per database as needed |

---

## v0.4.0 — 2026-03-27

### Highlights

**UNION/INTERSECT/EXCEPT pushdown in mixed-engine mode** — set operations spanning DuckDB and
InnoDB tables now execute entirely inside DuckDB. InnoDB tables are injected per arm and the
full set operation runs natively. All 22 TPC-H queries pass in standalone, SM, and MM modes.

**Filtered injection caching** — predicate pushdown into InnoDB injection is now cached. The
first query materializes only matching rows into DuckDB; subsequent queries with the same
predicate skip injection entirely. Cache keys include a hash of the pushed predicates, so a
query with a different filter re-injects correctly. Large dimension tables with selective
predicates now pay the injection cost once per session instead of once per query.

### New Features

- **UNION/INTERSECT/EXCEPT in mixed-engine mode** — `create_duckdb_unit_handler` now accepts
  set operations mixing DuckDB and InnoDB tables. The injection loop walks all arms of the
  `lex_unit`, injecting any InnoDB table found in any arm before pushing the full original SQL
  to DuckDB.

- **Filtered injection caching by predicate hash** — `inject_table_into_duckdb` now caches
  both full and filtered injections. The cache entry stores a hash of the serialized push-down
  predicates alongside the InnoDB row count. A cache hit requires both to match; a predicate
  change or row count change triggers eviction and re-injection.

- **Automated multi-distro release script** (`scripts/release.sh`) — builds Ubuntu 22.04,
  Oracle Linux 8, and Oracle Linux 9 artifacts in sequence, extracts binaries via `docker cp`
  (distro-agnostic), and packages per-distro tarballs with SQL files.

### Bug Fixes

- TPC-H Q7/Q13/Q16/Q20 fixed in MM mode.
- Derived table crash fixed.
- Bare-join rewrite extended to additional MM-mode query shapes.

### Documentation

- **WRITING_SQL.md** — added Transaction Limitations section; updated CTE advice to reflect
  injection caching (CTE pre-aggregation now only warranted for large unfiltered InnoDB tables);
  rewrote Views for BI Tools section with cross-engine JOIN view example.
- Comparison documents moved to `comparisons/` with consistent `HD_vs_` naming.

### Release Artifacts

| File | Description |
|---|---|
| `ha_duckdb-v0.4.0-ubuntu.so` | Ubuntu 22.04 (also covers Debian 12) |
| `ha_duckdb-v0.4.0-oracle8.so` | Oracle Linux 8 |
| `ha_duckdb-v0.4.0-oracle9.so` | Oracle Linux 9 |
| `holyduck_duckdb_extensions.sql` | DuckDB macros and views |
| `holyduck_mariadb_functions.sql` | MariaDB stored functions — install per database as needed |

---

## v0.3.0 — 2026-03-25

### Highlights

The headline change in this release is **full cross-engine pushdown via InnoDB injection**.
Any query that contains at least one DuckDB table — including flat joins against InnoDB tables —
is now pushed entirely to DuckDB. InnoDB tables are read once, materialized as DuckDB temp
tables in RAM, and DuckDB executes the complete query natively. MariaDB never touches a row
during execution. This eliminates the cross-engine join performance penalty described in
earlier documentation; no special query restructuring is required for typical workloads.

All 22 TPC-H queries now pass against a mixed-engine setup (DuckDB fact tables + InnoDB
dimension tables).

### New Features

- **InnoDB injection for flat cross-engine SELECT** — queries joining DuckDB and InnoDB tables
  now get `PUSHED SELECT`. InnoDB rows are loaded into a DuckDB temp table via the Appender
  (native columnar in-memory format) before query execution. The retry loop handles tables not
  visible in the top-level leaf list by injecting on demand after a DuckDB "table not found"
  error.

- **Original SQL passthrough for pure-DuckDB queries** — when all tables in a query are DuckDB,
  `init_scan` forwards the original client SQL string to DuckDB directly, bypassing MariaDB's
  AST printer entirely. This avoids optimizer artifacts (`<in_optimizer>`, `<expr_cache>`,
  `<exists>`, etc.) that the printer injects and DuckDB cannot parse.

- **Multi-distro release pipeline** (`scripts/build-all.pl`) — single command builds Release
  artifacts for Ubuntu 22.04, Oracle Linux 8, and Oracle Linux 9 in sequence. Manages container
  lifecycle automatically. Release packages include binaries and SQL files for all three targets.

- **No-copy dev builds** — the plugin output directory is now bind-mounted directly into the
  container (`plugin-out-<distro>/`). `cmake --build` writes `ha_duckdb.so` straight to the
  MariaDB plugin directory; no `docker cp` step required.

- **GDB debugging support** (`debug/`) — `gdb-attach.sh` attaches GDB to the running MariaDB
  process with `ha_duckdb.so` symbols loaded. `watch-init-scan.gdb` breaks on `init_scan`,
  printing the SQL sent to DuckDB and logging each InnoDB table injection.

### Bug Fixes

- **Type discovery: DECIMAL and numeric columns** — DuckDB's `information_schema.columns.data_type`
  returns full parameterized type strings (`DECIMAL(15,2)`, not `DECIMAL`). The discovery
  function now uses prefix matching and parameter extraction, so DECIMAL, NUMERIC, VARCHAR,
  CHAR, and CHARACTER VARYING columns are all correctly mapped. Previously these all fell
  through to `VARCHAR(255)`.

- **CTE reference stripping** — fixed a bug where schema-qualified table references inside
  injected CTEs were not stripped, causing DuckDB to fail to resolve the unqualified temp table
  name on retry.

- **Bare JOIN syntax** — MariaDB's AST printer moves `JOIN ... ON` conditions to `WHERE`,
  producing syntax DuckDB rejects. The `fix_bare_joins()` rewrite now restores `JOIN ... ON TRUE`
  for bare joins in both the select and derived paths.

- **MariaDB optimizer artifact stripping** — added rewrite coverage for `<materialize>`,
  `<primary_index_lookup>`, and `!(...)` forms emitted by MariaDB's printer in complex queries
  (TPC-H Q22 and others).

### Testing

- Full TPC-H Q1–Q22 regression suite added, all passing.
- Mixed-engine regression suite expanded: `nation`, `region`, `part`, `supplier` moved to
  InnoDB; `sales`, `products`, `macro_inputs` remain DuckDB. All 32 regression tests pass
  against this configuration, confirming the injection path works correctly for every query
  shape in the suite.

### Documentation

- **INTERNALS.md** — major overhaul: corrected `PUSHED SELECT` description (now fires for
  cross-engine queries via injection, not only pure-DuckDB), updated architecture diagram,
  expanded type mapping tables (both directions), removed stale "cross-engine aggregation
  pushdown" limitation, updated repository layout.
- **WRITING_SQL.md** — rewrote cross-engine join section to reflect injection; removed the
  "naive query is slow" framing; CTE optimization advice retained but reframed as relevant only
  for genuinely large InnoDB tables.

### Release Artifacts

| File | Description |
|---|---|
| `ha_duckdb-ubuntu.so` | Ubuntu 22.04 / Debian 12 |
| `ha_duckdb-oracle8.so` | Oracle Linux 8 |
| `ha_duckdb-oracle9.so` | Oracle Linux 9 |
| `holyduck_duckdb_extensions.sql` | DuckDB macros and views — copy to plugin directory |
| `holyduck_mariadb_functions.sql` | MariaDB stored functions — install per database as needed |

---

## v0.2.7 — 2026-03-23

- Add `kill_query`, `show_status`, `discover_table_names`, `discover_table_existence` hooks
- System variable controls: `duckdb_execute_sql`, `duckdb_execute_script`, `duckdb_last_result`,
  `duckdb_reload_extensions`, `duckdb_max_threads`

## v0.2.5

- Mixed-engine CTE pushdown via proactive injection (`PUSHED DERIVED`)
- Initial regression test suite
- Bare-join parser fix for derived handler path
- `HOLYDUCK_VS_ALISQL.md` comparison document

## v0.2.0

- Initial public release
- `CREATE TABLE ... ENGINE=DUCKDB`, full DDL/DML pushdown
- `select_handler` / `derived_handler` pushdown framework
- MariaDB function compatibility macros (`DATE_FORMAT`, `IFNULL`, `DATEDIFF`, etc.)
- `RoundDateTime()` time-bucketing macro
- Automatic table and view discovery
- Multi-distro Docker build pipeline (Ubuntu, Oracle Linux 8/9, Debian 12)
- Dual license: GPL v2 + commercial
