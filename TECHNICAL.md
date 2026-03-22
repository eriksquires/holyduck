# MariaDB DuckDB Storage Engine — Technical Reference

## Feature Status

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
- **UNION / INTERSECT / EXCEPT pushdown** via `create_unit`: When all SELECT arms reference
  only DUCKDB tables, the entire set operation runs inside DuckDB.
  EXPLAIN shows `PUSHED UNION`. Includes `UNION ALL` and deduplicating `UNION`.
- **Derived table / CTE pushdown** via `create_derived`: When a subquery in the FROM clause
  (or a CTE) references only DUCKDB tables, it runs entirely inside DuckDB before the result
  is returned to MariaDB. EXPLAIN shows `PUSHED DERIVED`.
  Key pattern: pre-aggregate DuckDB data in a CTE, then join the small result with InnoDB.
  ```sql
  WITH agg AS (
      SELECT sensor_id, AVG(val) AS avg_val FROM duckdb_metrics GROUP BY sensor_id
  )
  SELECT s.name, a.avg_val FROM innodb_sensors s JOIN agg a ON s.id = a.sensor_id;
  ```
- **Condition pushdown** via `cond_push()`: For cross-engine joins, WHERE conditions are pushed
  into the DuckDB scan query. EXPLAIN shows `Using where with pushed condition`.
- **Column subset scan**: For cross-engine joins, `rnd_init()` reads `table->read_set` and emits
  `SELECT col1, col2` instead of `SELECT *`, reducing data transfer.

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
- `RoundDateTime(dt, bucket_secs)` — wraps DuckDB's native `time_bucket()` for time bucketing
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
MariaDB can join DUCKDB tables with InnoDB tables. The optimizer uses `type=ALL` for the
DuckDB table (intentional — see Architecture below) and `eq_ref` for InnoDB lookups.
Condition pushdown reduces the rows DuckDB returns before the join.

## Architecture

```
[Client / DBeaver]
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

## Known Limitations

| Area | Status |
|---|---|
| Column type changes | Not supported — use add/populate/rename/drop pattern |
| `INSERT ... ON DUPLICATE KEY UPDATE` | Not supported |
| Aggregation pushdown (cross-engine) | Aggregations run in MariaDB; workaround: wrap DuckDB aggregation in a CTE — `PUSHED DERIVED` fires and runs it in DuckDB |
| Bulk INSERT constraint errors | Returns error 1030 instead of 1022 (batch rejected, correct behavior) |

## Repository Layout

```
├── src/
│   ├── ha_duckdb.cc              # Storage engine implementation
│   ├── ha_duckdb.h               # Header
│   └── CMakeLists.txt            # Standalone cmake build
├── sql/
│   └── duckdb_mariadb_compat.sql # MariaDB→DuckDB function macros
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
