# MariaDB DuckDB Storage Engine — Project Status

## What Works (as of 2026-03-22)

### DDL
- `CREATE TABLE ... ENGINE=DUCKDB` — creates a shared `#duckdb/global.duckdb` file per database
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
- **Full SELECT pushdown** via `ha_duckdb_select_handler`: GROUP BY, SUM, COUNT, AVG,
  ORDER BY executed entirely inside DuckDB when all tables in the query are DUCKDB engine.
  EXPLAIN shows `PUSHED SELECT` with all-NULL access details.
- **Condition pushdown** via `cond_push()`: WHERE conditions on cross-engine joins are pushed
  into the DuckDB scan query.  EXPLAIN shows `Using where with pushed condition`.
  Verified: 1-month date filter returns 12,488 rows instead of 300,000.
- **Column subset scan**: `rnd_init()` reads `table->read_set` and emits `SELECT col1, col2`
  instead of `SELECT *`, reducing data transfer for cross-engine joins.

### Concurrency
- Write locks upgraded to `TL_WRITE` in `store_lock()` — MariaDB serializes concurrent writers
  at the table level (DuckDB only supports one writer at a time).
- Concurrent readers work via DuckDB's MVCC — readers never blocked by other readers.
- Readers briefly blocked during active writes (acceptable; writes are infrequent).

### MariaDB Function Compatibility
Macros installed into DuckDB at startup translate MariaDB function names:
- `DATE_FORMAT`, `UNIX_TIMESTAMP`, `FROM_UNIXTIME`, `LAST_DAY`
- `LOCATE`, `MID`, `SPACE`, `STRCMP`, `REGEXP_SUBSTR`, `FIND_IN_SET`
- `IF`
- `CHAR(n)` rewritten to `chr(n)` in the SQL rewrite pass
- `<cache>(expr)` wrappers from MariaDB's AST printer stripped automatically

Macros live in `sql/duckdb_mariadb_compat.sql` — edit and redeploy without recompiling.

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

### Cross-Engine Joins
MariaDB can join DUCKDB tables with InnoDB tables.  The optimizer uses `type=ALL` for the
DuckDB table (intentional — see Architecture below) and `eq_ref` for InnoDB lookups.
Condition pushdown reduces the rows DuckDB returns before the join.

## Architecture

```
[Client / DBeaver]
       │
       ▼
[MariaDB 11.8.3]  ← standard SQL interface
       │
       ├─ Pure-DUCKDB query ──► ha_duckdb_select_handler  (full pushdown, ~0.005s)
       │
       └─ Cross-engine join ──► ha_duckdb::rnd_init()      (batched scan)
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

## Known Limitations

| Area | Status |
|---|---|
| Column type changes | Not supported — use add/populate/rename/drop pattern |
| `INSERT ... ON DUPLICATE KEY UPDATE` | Not supported |
| `ALTER TABLE ADD/DROP COLUMN` across databases | Not tested |
| Aggregation pushdown | Aggregations still run in MariaDB; only row/column filtering is pushed for cross-engine queries |
| Bulk INSERT constraint errors | Returns error 1030 instead of 1022 (ugly but correct — batch rejected) |

## File Layout

```
mariadb-duckdb-plugin/
├── src/
│   ├── ha_duckdb.cc         # Storage engine implementation
│   ├── ha_duckdb.h          # Header
│   └── CMakeLists.txt       # Standalone build (points at MariaDB source tree)
├── sql/
│   └── duckdb_mariadb_compat.sql  # MariaDB→DuckDB function macros (deployment artifact)
├── lib/
│   ├── libduckdb.so         # DuckDB v1.5.0
│   ├── duckdb.h / duckdb.hpp
├── build/                   # cmake output — gitignored
│   └── ha_duckdb.so
├── data/                    # MariaDB data dir (bind-mounted into container) — gitignored
├── docker/
│   └── base-ubuntu.dockerfile
└── scripts/
    ├── deploy.sh            # Build + deploy into test container (USE THIS)
    └── docker-run.sh        # Start dev container
```

## Development Workflow

See `DOCKER_WORKFLOW.md` for the full workflow.  The short version:

```bash
# Edit source locally
vim src/ha_duckdb.cc

# Build, deploy, restart MariaDB, verify — all in one step
./scripts/deploy.sh

# Connect and test
docker exec -it duckdb-plugin-test mariadb -uroot -ptestpass
```

## Environment

| Component | Location |
|---|---|
| Plugin source | `/home/shared/mariadb/mariadb-duckdb-plugin/src/` |
| Build output | `/home/shared/mariadb/mariadb-duckdb-plugin/build/` |
| MariaDB source | `/home/erik/shared/mariadb/mariadb-11.8.3-git/` |
| DuckDB library | `/home/shared/mariadb/mariadb-duckdb-plugin/lib/` |
| Test container | `duckdb-plugin-test` (Ubuntu 22.04, MariaDB 11.8.3) |
| MariaDB data | `/home/shared/mariadb/mariadb-duckdb-plugin/data/` (bind-mounted) |
