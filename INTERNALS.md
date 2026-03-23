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

HolyDuck implements three pushdown paths. EXPLAIN output tells you which fired:

| EXPLAIN output | When it fires | What runs in DuckDB |
|---|---|---|
| `PUSHED SELECT` | All tables in query are DUCKDB | Entire SELECT including GROUP BY, ORDER BY |
| `PUSHED UNION` | All arms of UNION/INTERSECT/EXCEPT are DUCKDB | Entire set operation |
| `PUSHED DERIVED` | CTE or subquery references only DUCKDB tables | Entire CTE/subquery |

For mixed-engine queries where pushdown cannot fire on the full query, two optimizations still apply:

- **Condition pushdown** via `cond_push()`: WHERE conditions are pushed into the DuckDB scan query, filtering rows before they reach MariaDB.
- **Column subset scan**: `rnd_init()` reads `table->read_set` and emits `SELECT col1, col2` instead of `SELECT *`, reducing data transfer.

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

Macros live in `sql/holyduck_duckdb_extensions.sql` — edit and redeploy without recompiling.

### Data Type Support
| MariaDB Type | DuckDB Type |
|---|---|
| TINYINT, SMALLINT, INT, MEDIUMINT | INTEGER |
| BIGINT | BIGINT |
| FLOAT | FLOAT |
| DOUBLE | DOUBLE |
| DECIMAL | DECIMAL |
| DATE | DATE |
| TIME | TIME |
| DATETIME, TIMESTAMP | TIMESTAMP |
| VARCHAR, TEXT, BLOB, etc. | VARCHAR |

Type mapping happens in two places in `src/ha_duckdb.cc`:

- **DDL mapping (~line 387)** — converts `MYSQL_TYPE_*` constants to DuckDB type name strings
  when `CREATE TABLE ... ENGINE=DUCKDB` is executed. This is the source of truth for what
  DuckDB column type gets created on disk.
- **Value mapping (~lines 661, 773, 1304, 1415)** — converts field values between MariaDB and
  DuckDB representations at query time (reads and writes).

Note that several MariaDB integer types (TINYINT, SMALLINT, MEDIUMINT) all map to DuckDB INTEGER.
DuckDB stores them efficiently regardless; the distinction only matters at the MariaDB display layer.

---

## Architecture

```
[Client / DBeaver / R / Python]
       │
       ▼
[MariaDB 11.8.3]  ← standard SQL interface
       │
       ├─ Pure-DUCKDB SELECT ──► ha_duckdb_select_handler   (PUSHED SELECT)
       ├─ Pure-DUCKDB UNION  ──► ha_duckdb_select_handler   (PUSHED UNION)
       ├─ DuckDB CTE/subquery ─► ha_duckdb_derived_handler  (PUSHED DERIVED)
       │
       └─ Cross-engine join  ──► ha_duckdb::rnd_init()      (batched scan)
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
`index_flags()` returns no read-capability bits. MariaDB cannot plan row-at-a-time
index scans against DuckDB tables. This is deliberate:
- Exposing index capability would allow the optimizer to drive index scans through the
  handler API, bypassing DuckDB's vectorised execution entirely.
- DuckDB's own planner uses indexes internally on pushed-down queries.
- Cross-engine joins use `type=ALL` + a single batched `rnd_init()` call, which is correct.
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
but is potentially messy — you now have two control planes accessing the same data, which requires
care around coordination. Read-only mode is required; MariaDB holds the write lock.

For administrative work — bulk loads, schema changes, repairs, or copying data between DuckDB
databases — stop MariaDB first, do the work with any DuckDB client, then bring MariaDB back up.

---

## Known Limitations

| Area | Status |
|---|---|
| Column type changes | Not supported — use add/populate/rename/drop pattern |
| `INSERT ... ON DUPLICATE KEY UPDATE` | Not supported |
| Aggregation pushdown (cross-engine) | Use CTE pattern above — `PUSHED DERIVED` handles it |
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
│   ├── holyduck_duckdb_extensions.sql   # DuckDB extensions, macros, and views (loaded into DuckDB at startup)
│   ├── holyduck_mariadb_functions.sql   # MariaDB stored functions (install once per database)
│   └── holyduck_mariadb_tables.sql      # MariaDB table stubs for DuckDB-native views (install once per database)
├── docker/
│   ├── base-ubuntu.dockerfile
│   ├── base-oracle8.dockerfile
│   ├── base-oracle9.dockerfile
│   └── base-debian12.dockerfile
├── scripts/
│   ├── fetch-deps.sh             # Download MariaDB source + DuckDB library
│   ├── build-base.sh             # Build Docker base image
│   ├── docker-run.sh             # Start dev container
│   ├── cmake-setup.sh            # Build MariaDB + configure plugin cmake
│   └── deploy.sh                 # Build plugin + deploy into container
└── lib/                          # gitignored — populated by fetch-deps.sh
    ├── libduckdb.so
    ├── duckdb.hpp
    └── duckdb.h
```
