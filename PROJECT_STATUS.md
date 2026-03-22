# MariaDB DuckDB Storage Engine - Project Status

## What Works (as of 2026-03-21)

### ✅ Plugin Compiles and Loads
- Standalone build against MariaDB 11.8.3 source tree (`mariadb-11.8.3-git/build/`)
- Configurable via `-DMARIADB_SOURCE_DIR` and `-DMARIADB_BUILD_DIR` cmake vars
- Loads cleanly into MariaDB 11.8.3 — `SHOW ENGINES` lists DUCKDB

### ✅ Core SQL Operations
- `CREATE TABLE ... ENGINE=DUCKDB` — creates a per-table `.duckdb` file on disk
- `INSERT INTO` — rows written via DuckDB Appender API
- `SELECT *` — full table scan via DuckDB result set
- `DROP TABLE` — removes `.duckdb` and `.duckdb.wal` files
- `RENAME TABLE` — renames the `.duckdb` file

### ✅ Analytics Queries Work
MariaDB pushes GROUP BY, SUM, COUNT, AVG down through the engine:
```sql
SELECT region, COUNT(*), SUM(revenue), AVG(revenue)
FROM sales
GROUP BY region
ORDER BY total DESC;
```
Results are correct.

### ✅ Data Type Support
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

### ✅ Build Environment
- Build dir: `/home/shared/mariadb/mariadb-duckdb-plugin/build/`
- Source dir: `/home/shared/mariadb/mariadb-duckdb-plugin/src/`
- DuckDB library: `/home/shared/mariadb/mariadb-duckdb-plugin/lib/`
- MariaDB source: `/home/erik/shared/mariadb/mariadb-11.8.3-git/`

### ✅ Docker Environment
- Base image `mariadb-duckdb-base:ubuntu` pinned to MariaDB **11.8.3**
- Saved to `~/shared/docker_images/mariadb-duckdb-base-ubuntu.tar`
- Plugin source mounted at `/plugin-src/`, MariaDB source at `/mariadb-src/`
- Goal: test on other OS distros (Rocky Linux etc.) — not yet attempted

## Known Limitations / Not Yet Implemented

### ❌ Data Does Not Persist Across Server Restarts
- The `.duckdb` file is created correctly on disk
- However, when MariaDB reopens the table after restart, the `DuckDB_share`
  is re-initialised but the table schema may not be re-created in the
  in-memory DuckDB session — needs investigation
- Workaround: `CREATE TABLE IF NOT EXISTS` in `build_create_sql` handles
  re-creation, but this needs end-to-end testing after a restart

### ❌ No UPDATE or DELETE
- Both return `HA_ERR_WRONG_COMMAND`
- Acceptable for an append-only analytics engine initially
- Could be added later via DuckDB UPDATE/DELETE SQL

### ❌ No Index Support
- `max_supported_keys()` returns 0
- MariaDB query optimiser falls back to full table scans for all queries
- Acceptable for OLAP workloads

### ❌ No SQL Pushdown to DuckDB
- Complex WHERE, GROUP BY, and aggregations are evaluated by MariaDB
  after fetching all rows via `rnd_next()`
- True pushdown (sending the full query to DuckDB) would give much better
  performance on large tables
- Requires implementing the `engine_push()` / condition pushdown API

### ❌ NULL Handling Not Fully Tested
- NULL bits are cleared and set in `convert_row_from_duckdb()`
- Not yet tested with nullable columns

### ❌ No DECIMAL / DATE Type Round-Trip Testing
- Written as strings, read back as strings via `val.ToString()`
- Needs testing to confirm MariaDB accepts the format DuckDB returns

### ❌ Docker Multi-OS Testing Not Done
- Base image exists for Ubuntu 22.04 only
- Rocky Linux / Debian images not yet built

## Architecture

```
[Client / DBeaver]
       │
       ▼
[MariaDB 11.8.3]  ← standard SQL interface
       │
       ▼
[ha_duckdb storage engine]  ← our plugin (ha_duckdb.so)
       │  CREATE/INSERT via Appender
       │  SELECT via MaterializedQueryResult
       ▼
[DuckDB embedded engine]  ← libduckdb.so
       │
       ▼
[per-table .duckdb files]  ← on-disk columnar storage
```

## File Layout

```
mariadb-duckdb-plugin/
├── src/
│   ├── ha_duckdb.h          # Storage engine header
│   ├── ha_duckdb.cc         # Storage engine implementation
│   └── CMakeLists.txt       # Standalone build (points at MariaDB source)
├── lib/
│   ├── libduckdb.so         # Prebuilt DuckDB library
│   ├── duckdb.h             # DuckDB C header
│   └── duckdb.hpp           # DuckDB C++ header
├── build/                   # cmake output (gitignored)
│   └── libha_duckdb.so      # Compiled plugin
├── docker/
│   ├── base-ubuntu.dockerfile   # MariaDB 11.8.3 + dev tools (Ubuntu)
│   └── dev-ubuntu.dockerfile    # Development image
└── scripts/
    └── docker-run.sh            # Start dev container
```

## Next Steps (Priority Order)

1. **Test persistence** — restart MariaDB, verify existing DUCKDB tables are
   accessible and data survives
2. **Test NULL columns** — create table with nullable fields, insert NULLs,
   verify round-trip
3. **Test DECIMAL and DATE round-trips** — confirm string-based storage gives
   correct values back
4. **Implement SQL pushdown** — route full SELECT queries to DuckDB for
   performance on large tables
5. **Rocky Linux Docker image** — build and test on a second OS
6. **Parquet import/export** — allow `SELECT * FROM 'file.parquet'` style
   queries via DuckDB's Parquet support
