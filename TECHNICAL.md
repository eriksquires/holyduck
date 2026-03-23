# MariaDB DuckDB Storage Engine — Technical Reference

## SQL Dialect — MariaDB is the Gatekeeper

It is improtant to know that **all SQL passes through MariaDB's parser first**. If 
MariaDB doesn't recognize the syntax, it never reaches DuckDB — not even inside a 
subquery or CTE. This has practical consequences for what you can and cannot write.

### What MariaDB blocks

DuckDB has powerful SQL extensions that MariaDB's parser will reject outright:

- `SELECT * EXCLUDE (col)` — MariaDB doesn't know `EXCLUDE`
- `PIVOT` / `UNPIVOT` — not in MariaDB's grammar
- `QUALIFY` clause (window function filtering) — MariaDB doesn't know it
- DuckDB column types in DDL: `LIST`, `STRUCT`, `MAP` — MariaDB won't parse them
- `SELECT * REPLACE (expr AS col)` — DuckDB extension, unknown to MariaDB

Wrapping these in a subquery does not help — the parser rejects them before pushdown
decisions are made.

### What gets through

Any SQL that is valid MariaDB syntax will reach DuckDB. This includes standard SQL that
both engines understand, plus anything bridged by the compatibility macros in
`duckdb_mariadb_compat.sql` — `DATE_FORMAT`, `IFNULL`, `DATEDIFF`, `RoundDateTime`, etc.

### Who wins on ambiguous syntax

Where both engines accept the same syntax but behave differently, MariaDB evaluates
non-pushed expressions and DuckDB evaluates pushed ones. The compatibility macros exist
precisely to bridge these gaps so the pushed-down SQL means the same thing in DuckDB
as it would have in MariaDB.

### The practical rule

Write SQL that MariaDB accepts. Use the compatibility macros for MariaDB-specific functions.
For anything DuckDB-specific that MariaDB blocks, there is no workaround within HolyDuck —
the parser is the hard boundary.

That said, within valid MariaDB SQL there is still a lot of room to influence *how much work
DuckDB does vs MariaDB* — and that's where query structure matters most.

---

## Optimizing Queries for HolyDuck
Single engine queries will run as you expect, either all in MariaDB or all in DuckDB. 

When your query uses tables inside/outside of DuckDB some attention to the query shape
will yield big performance improvements.  

Generally speaking, MariaDB can only push down WHERE conditions which are entirely in one 
engine or another but filters that depend on values from external (non-duck) tables 
(e.g. `WHERE s.id = c.id`) won't be.  Instead MariaDB will push down all the filters it can, 
retrieve the rows and then apply any missing filter conditions.  Honestly this is not deathly
slow, but also not optimal.  

Fortunately MariaDB with HolyDuck does let us write SQL in a way that works around this bottleneck.

The solution is to restructure the query so the heavy DuckDB work happens in a CTE or
subquery first — then `PUSHED DERIVED` fires and the entire aggregation runs inside DuckDB,
returning a small result for MariaDB to join against InnoDB.

### Example: Sales Analysis

Suppose you have:
- `sales` — DuckDB table, millions of rows (order_id, product_id, category_id, region_id, amount, ts)
- `categories` — InnoDB table, small (id, name)
- `regions` — InnoDB table, small (id, name)

**Naive query — slow:**

```sql
SELECT c.name AS category,
       r.name AS region,
       SUM(s.amount) AS total_sales,
       COUNT(*) AS order_count
FROM sales s  -- DuckDB table
   JOIN categories c ON s.category_id = c.id
   JOIN regions r ON s.region_id = r.id
WHERE
   s.ts BETWEEN '2026-01-01' AND '2026-03-31'
GROUP BY c.name, r.name
ORDER BY total_sales DESC;
```

What happens: MariaDB pushes the only filter it can (date and time) into DuckDB, but the GROUP BY runs in MariaDB after the join, meaning potentially
hundreds of thousands of filtered rows flow across the engine boundary before aggregation happens.

**Optimized with CTE — fast:**

```sql
WITH sales_summary AS (
   -- This happens 100% in DuckDB
    SELECT category_id,
           region_id,
           SUM(amount) AS total_sales,
           COUNT(*) AS order_count
    FROM sales
    WHERE ts BETWEEN '2026-01-01' AND '2026-03-31'
    GROUP BY category_id, region_id
)
-- 
-- sales_summary is a tiny number of rows
--
SELECT c.name AS category,
       r.name AS region,
       ss.total_sales,
       ss.order_count
FROM sales_summary ss
JOIN categories c ON ss.category_id = c.id
JOIN regions r ON ss.region_id = r.id
ORDER BY ss.total_sales DESC;
```

What happens: `PUSHED DERIVED` fires on `sales_summary`. DuckDB scans `sales`, applies the
date filter, and returns one row per (category_id, region_id) pair — perhaps 50 rows for
5 categories × 10 regions. MariaDB joins those 50 rows against the small InnoDB tables.

Verify with EXPLAIN:

```sql
EXPLAIN WITH sales_summary AS (
    SELECT category_id, region_id, SUM(amount) AS total_sales, COUNT(*) AS order_count
    FROM sales WHERE ts BETWEEN '2026-01-01' AND '2026-03-31'
    GROUP BY category_id, region_id
)
SELECT c.name, r.name, ss.total_sales
FROM sales_summary ss
JOIN categories c ON ss.category_id = c.id
JOIN regions r ON ss.region_id = r.id\G
```

Look for `select_type: PUSHED DERIVED` — that confirms DuckDB is doing the work.

### The General Rule

Whenever you have a large DuckDB table joining against InnoDB:

1. Move all DuckDB scanning, filtering, and aggregation into a CTE or subquery (same benefit)
2. Join the CTE result (small) against InnoDB (also small)
3. MariaDB only touches small row counts on both sides

This pattern works for any depth of aggregation — daily buckets, percentiles, window functions,
`RoundDateTime` time bucketing — as long as the CTE references only DuckDB tables.

### Using Views for BI Tools

BI tools (Tableau, Grafana, Power BI, etc.) generate their own SQL. Even when they do write CTEs,
they may not structure them in a way that triggers efficient pushdown. The solution is to pre-bake the CTE
pattern into a MariaDB view, so the tool always gets the right behavior regardless of what SQL it
generates on top:

```sql
CREATE VIEW sales_summary AS
    SELECT category_id, region_id,
           SUM(amount) AS total_sales,
           COUNT(*) AS order_count
    FROM sales
    GROUP BY category_id, region_id;
```

The BI tool queries `sales_summary` like a plain table. MariaDB rewrites the query against the
view definition, `PUSHED DERIVED` fires, and the aggregation runs entirely inside DuckDB.
The tool gets fast results without any knowledge of the underlying engine.

---

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

Macros live in `sql/duckdb_mariadb_compat.sql` — edit and redeploy without recompiling.

### Extending with Custom Functions — The Dual Implementation Pattern

MariaDB's parser must recognize every function name in a query before pushdown decisions are made.
This means a DuckDB macro alone is not enough — if MariaDB doesn't know the function, the query
is rejected before it ever reaches DuckDB.

The solution is a **dual implementation**:

1. **DuckDB macro** — implements the function using DuckDB-native logic; runs when the query is
   pushed down to DuckDB.
2. **MariaDB stored function** — satisfies the parser and provides a portable fallback using
   standard MariaDB SQL; runs when the query is not pushed down (e.g., on InnoDB tables).

`RoundDateTime` is the canonical example. The DuckDB macro uses `time_bucket()` for efficient
vectorised time bucketing. The MariaDB stored function uses unix timestamp arithmetic that works
on any table:

```sql
-- MariaDB stored function (install once):
CREATE FUNCTION RoundDateTime(dt DATETIME, bucket_secs INT)
RETURNS DATETIME DETERMINISTIC
RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);
```

With both in place:
- `SELECT RoundDateTime(ts, 300) FROM duckdb_table` — PUSHED SELECT fires, DuckDB macro runs (fast)
- `SELECT RoundDateTime(ts, 300) FROM innodb_table` — MariaDB stored function runs (correct)

This pattern works for any function that has equivalent logic in both engines. If a correct
MariaDB fallback is not feasible, the stored function can instead raise an explicit error with
`SIGNAL SQLSTATE '45000'` rather than returning silently wrong results.

HolyDuck ships a ready-to-run script for all supported MariaDB stored functions:

```bash
mariadb -uroot -p < /path/to/plugin_dir/holyduck_mariadb_functions.sql
```

The file `sql/holyduck_mariadb_functions.sql` uses `CREATE FUNCTION IF NOT EXISTS` so it is safe
to re-run after upgrades.

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
