# Changelog

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
