# HolyDuck Internals

## Feature Status

### DDL
- `CREATE TABLE ... ENGINE=DUCKDB` — creates a shared `#duckdb/global.duckdb` file per database
- **Table discovery** — DuckDB tables and views are auto-discovered on first query; no `CREATE TABLE` stub needed for DuckDB-native views defined in `holyduck_duckdb_extensions.sql`
- `DROP TABLE` — removes table from DuckDB
- `RENAME TABLE` — renames table in DuckDB
- `TRUNCATE TABLE` — pushed as DuckDB native TRUNCATE
- `ALTER TABLE ADD COLUMN` — pushed to DuckDB inplace
- `ALTER TABLE DROP COLUMN` — pushed to DuckDB inplace
- `ALTER TABLE RENAME COLUMN` / `CHANGE col` — pushed to DuckDB inplace
- `CREATE INDEX` / `DROP INDEX` — pushed to DuckDB inplace via inplace ALTER API

### DML
- `INSERT INTO` (single-row) — SQL path, enforces PK/UNIQUE constraints, returns 1022 on duplicate
- `INSERT INTO` (multi-row bulk) — DuckDB Appender path, enforces constraints, rejects entire batch on duplicate
- `REPLACE INTO` — uses DuckDB `INSERT OR REPLACE INTO`, handles conflict atomically
- `UPDATE ... WHERE` — direct pushdown: `UPDATE table SET ... WHERE pushed_where`
- `DELETE ... WHERE` — direct pushdown: `DELETE FROM table WHERE pushed_where`

### Query Pushdown

HolyDuck implements three pushdown paths. `EXPLAIN` output tells you which fired:

| EXPLAIN output | When it fires | What runs in DuckDB |
|---|---|---|
| `PUSHED SELECT` | Query has at least one DuckDB table; any InnoDB tables are injected as temp tables | Entire SELECT including JOIN, GROUP BY, ORDER BY |
| `PUSHED UNION` | All arms of UNION/INTERSECT/EXCEPT have at least one DuckDB table | Entire set operation |
| `PUSHED DERIVED` | CTE or subquery has at least one DuckDB table; non-DuckDB leaves are injected | Entire CTE/subquery |

**Injection** is how HolyDuck handles InnoDB tables in a pushed query. When the select handler
encounters an InnoDB table, it reads all rows from InnoDB and loads them into a DuckDB temp
table for the duration of the query. DuckDB then executes the full query — including joins and
aggregations — without knowing the source engine. This fires for both `PUSHED SELECT` and
`PUSHED DERIVED`.

The fallback path — `ha_duckdb::rnd_init()` with `cond_push()` — is used when pushdown cannot
fire at all (e.g., unsupported query shape). In this path MariaDB drives a batched scan of the
DuckDB table, pushing WHERE conditions into the DuckDB query and reading only the needed column
subset. This is correct but slower than injection for cross-engine queries.

### Concurrency
- Write locks upgraded to `TL_WRITE` in `store_lock()` — MariaDB serializes concurrent writers
  at the table level (DuckDB only supports one writer at a time).
- Concurrent readers work via DuckDB's MVCC — readers never blocked by other readers.
- Readers briefly blocked during active writes.

### MariaDB Function Compatibility
Macros installed into DuckDB at startup translate MariaDB function names:
- `DATE_FORMAT`, `UNIX_TIMESTAMP`, `FROM_UNIXTIME`, `LAST_DAY`
- `LOCATE`, `MID`, `SPACE`, `STRCMP`, `REGEXP_SUBSTR`, `FIND_IN_SET`
- `IF`
- `RoundDateTime(dt, bucket_secs)` — wraps DuckDB's native `time_bucket()` for time bucketing
- `CHAR(n)` rewritten to `chr(n)` in the SQL rewrite pass
- `<cache>(expr)` wrappers from MariaDB's AST printer stripped automatically

Macros live in `sql/holyduck_duckdb_extensions.sql` — edit and reload without recompiling.

### Data Type Mapping

Type mapping happens in two directions:

**MariaDB → DuckDB** (`field_type_to_duckdb()`, ~line 807): used when `CREATE TABLE ... ENGINE=DUCKDB`
is executed. Converts `MYSQL_TYPE_*` constants to DuckDB type strings.

| MariaDB Type | DuckDB Type |
|---|---|
| TINYINT, SMALLINT, INT, MEDIUMINT | INTEGER |
| BIGINT | BIGINT |
| FLOAT | FLOAT |
| DOUBLE | DOUBLE |
| DECIMAL, NUMERIC | DECIMAL |
| DATE | DATE |
| TIME | TIME |
| DATETIME, TIMESTAMP | TIMESTAMP |
| VARCHAR, TEXT, BLOB, and all others | VARCHAR |

**DuckDB → MariaDB** (`duckdb_type_to_mariadb()`, ~line 531): used during table discovery
(`duckdb_discover_table()`). Converts the full parameterized type string from DuckDB's
`information_schema.columns.data_type` to a MariaDB column definition.

| DuckDB Type | MariaDB Type |
|---|---|
| DECIMAL(p,s), NUMERIC(p,s) | DECIMAL(p,s) |
| DECIMAL, NUMERIC (no params) | DECIMAL(18,3) |
| VARCHAR(n), CHARACTER VARYING(n) | VARCHAR(n) |
| VARCHAR, CHARACTER VARYING (no params) | VARCHAR(255) |
| CHARACTER(n) | CHAR(n) |
| TINYINT | TINYINT |
| SMALLINT | SMALLINT |
| INTEGER | INT |
| BIGINT | BIGINT |
| HUGEINT | DECIMAL(38,0) |
| UTINYINT | TINYINT UNSIGNED |
| USMALLINT | SMALLINT UNSIGNED |
| UINTEGER | INT UNSIGNED |
| UBIGINT | DECIMAL(20,0) |
| FLOAT, REAL | FLOAT |
| DOUBLE | DOUBLE |
| DATE | DATE |
| TIME, TIMETZ | TIME |
| TIMESTAMP, DATETIME, TIMESTAMPTZ, TIMESTAMP WITH TIME ZONE | DATETIME |
| INTERVAL | VARCHAR(64) |
| BOOLEAN, BOOL | TINYINT(1) |
| BLOB, BYTEA | BLOB |
| JSON | JSON |
| LIST, STRUCT, MAP, and other complex types | VARCHAR(255) |

Note: several MariaDB integer types (TINYINT, SMALLINT, MEDIUMINT) all map to DuckDB INTEGER
on the write path. DuckDB stores them efficiently; the distinction only matters at the MariaDB
display layer. The discovery path (`duckdb_type_to_mariadb`) maps back precisely because DuckDB
preserves the original column type.

---

## Architecture

```
[Client / DBeaver / R / Python]
       │
       ▼
[MariaDB 11.8.3]  ← standard SQL interface
       │
       ├─ Any SELECT with ≥1 DuckDB table
       │     │
       │     ├─ All DuckDB ──────────────► ha_duckdb_select_handler  (PUSHED SELECT)
       │     │                               DuckDB executes natively
       │     │
       │     └─ Mixed DuckDB + InnoDB ──► ha_duckdb_select_handler  (PUSHED SELECT)
       │                                    inject_table_into_duckdb()
       │                                    InnoDB rows → DuckDB temp table
       │                                    DuckDB executes full query
       │
       ├─ CTE/subquery with ≥1 DuckDB table
       │     └────────────────────────────► ha_duckdb_derived_handler (PUSHED DERIVED)
       │                                    same injection for non-DuckDB leaves
       │
       ├─ UNION/INTERSECT/EXCEPT (all DuckDB)
       │     └────────────────────────────► ha_duckdb_select_handler  (PUSHED UNION)
       │
       └─ Fallback (no pushdown handler claimed the query)
             └────────────────────────────► ha_duckdb::rnd_init()     (batched scan)
                                             cond_push() → WHERE in DuckDB query
                                             read_set   → SELECT only needed columns
       │
       ▼
[DuckDB embedded engine]  ← libduckdb.so v1.5.0
       │
       ▼
[#duckdb/global.duckdb]  ← one file per MariaDB database, on-disk columnar storage
```

### Intentional Design: No MariaDB Index Scans
`index_flags()` returns no read-capability bits. MariaDB cannot plan row-at-a-time index scans
against DuckDB tables. This is deliberate:
- Exposing index capability would allow the optimizer to drive index scans through the handler
  API, bypassing DuckDB's vectorized execution entirely.
- DuckDB's own planner uses indexes internally on pushed-down queries.
- The fallback path uses `type=ALL` + a single batched `rnd_init()` call, which is correct.
- `max_supported_keys()` returns 64 so MariaDB accepts index DDL; it just never uses them.

### Intentional Design: No Column Type Changes
`ALTER TABLE MODIFY COLUMN` (type changes) are not supported via inplace ALTER.
The safe pattern is: add new column → populate it → drop old column → rename new column.

---

## Data File Location and Backups

All DuckDB tables across all MariaDB databases are stored in a single file:

```
<datadir>/#duckdb/global.duckdb
```

Typically `/var/lib/mysql/#duckdb/global.duckdb`. Confirm your datadir with `SELECT @@datadir`.

The recommended backup approach is a cold copy — stop MariaDB, copy the file, restart:

```bash
systemctl stop mariadb
cp /var/lib/mysql/#duckdb/global.duckdb /backups/global.duckdb.$(date +%Y%m%d)
systemctl start mariadb
```

### Direct access via DuckDB clients

Because `global.duckdb` is a standard DuckDB database file, any DuckDB client can open it
directly in read-only mode while MariaDB is running. This can be handy for exploratory analysis
but means two control planes are accessing the same data — read-only mode is required since
MariaDB holds the write lock.

For administrative work — bulk loads, schema changes, repairs, or copying data between DuckDB
databases — stop MariaDB first, do the work with any DuckDB client, then bring MariaDB back up.

---

## System Variables

MariaDB has no native way to expose DuckDB methods, so HolyDuck uses `SET GLOBAL` variables as
a command interface for DuckDB administration without restarting MariaDB.

| Variable | Type | Description |
|---|---|---|
| `duckdb_max_threads` | ULONG | Max CPU threads DuckDB may use (0 = all cores) |
| `duckdb_reload_extensions` | ULONG | Set to any value to reload `holyduck_duckdb_extensions.sql` without restarting MariaDB |
| `duckdb_execute_script` | STRING | Path to a DuckDB SQL script to execute immediately |
| `duckdb_execute_sql` | STRING | Execute a single DDL/DML statement directly in DuckDB. **SELECT results are discarded** — use for CREATE, INSERT, DROP, COPY, etc. |
| `duckdb_last_result` | STRING (read-only) | Outcome of the last `execute_sql` or `execute_script` call — row count or error message |

### Examples

```sql
-- Reload extensions after editing holyduck_duckdb_extensions.sql
SET GLOBAL duckdb_reload_extensions = 1;

-- Run a DuckDB SQL script
SET GLOBAL duckdb_execute_script = '/data/load_parquet.sql';
SELECT @@GLOBAL.duckdb_last_result;

-- Execute a single statement directly in DuckDB
SET GLOBAL duckdb_execute_sql = 'COPY my_table FROM ''/data/file.parquet''';
SELECT @@GLOBAL.duckdb_last_result;

-- Check engine status
SHOW ENGINE DUCKDB STATUS;
```

---

## Known Limitations

| Area | Status |
|---|---|
| Column type changes | Not supported — use add/populate/rename/drop pattern |
| `INSERT ... ON DUPLICATE KEY UPDATE` | Not supported |
| Bulk INSERT constraint errors | Returns error 1030 instead of 1022 (batch rejected, correct behavior) |
| High availability / replication | Not supported — single node only |
| Multiple concurrent writers | Single writer at a time (DuckDB limitation) |

---

## Repository Layout

```
├── src/
│   ├── ha_duckdb.cc              # Storage engine implementation
│   ├── ha_duckdb.h               # Header
│   └── CMakeLists.txt            # Standalone cmake build
├── sql/
│   ├── holyduck_duckdb_extensions.sql   # DuckDB macros and views (loaded into DuckDB at startup)
│   ├── holyduck_mariadb_functions.sql   # MariaDB stored functions (install once per database)
│   └── holyduck_mariadb_tables.sql      # MariaDB table stubs for DuckDB-native views
├── docker/
│   ├── base-ubuntu.dockerfile
│   ├── base-oracle8.dockerfile
│   ├── base-oracle9.dockerfile
│   └── base-debian12.dockerfile
├── scripts/
│   ├── fetch-deps.sh             # Download MariaDB source + DuckDB library
│   ├── build-base.sh             # Build Docker base image
│   ├── docker-run.sh             # Start dev container (per-distro, handles plugin-out mount)
│   ├── cmake-setup.sh            # Build MariaDB + configure plugin cmake (one-time per distro)
│   ├── deploy.sh                 # Build plugin inside running container
│   └── build-all.pl              # Build release artifacts for all distros (ubuntu, oracle8, oracle9)
├── debug/
│   ├── gdb-attach.sh             # Attach GDB to the running MariaDB process with symbols loaded
│   └── watch-init-scan.gdb       # GDB script: break on init_scan, print SQL and injection events
├── tests/
│   └── regression/               # SQL regression suite (setup.sql, teardown.sql, test cases)
└── lib/                          # gitignored — populated by fetch-deps.sh
    ├── libduckdb.so
    ├── duckdb.hpp
    └── duckdb.h
```
