# Writing SQL with HolyDuck

## Introduction

The guidance in this document comes from our choice not to re-write a SQL parser or cost-based optimizer.   For our use cases we felt we could achieve our major goals with macros and a little manual SQL re-writing.  Here we'll discuss:

- How to write SQL which runs
- Optimizing SQL queries for cross-engine joins
- Extending HolyDuck with custom features
- Leveraging views

## SQL Dialect

It is important to know that **all SQL passes through MariaDB's parser first**. If MariaDB doesn't recognize the syntax or function, it will never reach DuckDB — not even inside a subquery or CTE. This has practical consequences for what you can and cannot write. For overcoming functional incompatibility, see the **Extending HolyDuck** section below.

### What MariaDB blocks

DuckDB has powerful SQL extensions that MariaDB's parser will reject outright:

- `SELECT * EXCLUDE (col)` — MariaDB doesn't know `EXCLUDE`
- `PIVOT` / `UNPIVOT` — not in MariaDB's grammar
- `QUALIFY` clause (window function filtering) — MariaDB doesn't know it
- DuckDB column types in DDL: `LIST`, `STRUCT`, `MAP` — MariaDB won't parse them
- `SELECT * REPLACE (expr AS col)` — DuckDB extension, unknown to MariaDB

We'll cover working around these limitations below in **Extending HolyDuck.**

### What gets through

SQL that is valid MariaDB syntax and verbs will reach DuckDB, whether or not DuckDB can execute it or not.  DuckDB may then throw an error. 

---

## Optimizing Queries for HolyDuck
When your query uses tables which are both in and out of DuckDB some attention to the query shape will yield big performance improvements.

Single engine queries will run as you expect, either all in MariaDB or all in DuckDB.

Generally speaking, MariaDB can only push down WHERE conditions which are entirely in one
engine or another but filters that depend on values from external (non-duck) tables
(e.g. `WHERE s.id = c.id`) won't be.  Instead MariaDB will push down all the filters it can,
retrieve the rows and then apply any missing filter conditions.  Honestly this is not deathly
slow, but also not optimal.

Fortunately we _can_ write SQL in a way that works around this bottleneck.

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
Think of the CTE as defining the execution boundary: everything inside runs in DuckDB.

What happens: `PUSHED DERIVED` fires on `sales_summary`. DuckDB scans `sales`, applies the
date filter, aggregates by region_id and category_id and returns one row per (category_id,
region_id) pair — perhaps 50 rows for 5 categories × 10 regions. MariaDB then joins those
50 rows against the small InnoDB tables.

When you explain your queries look for `select_type: PUSHED DERIVED` — that confirms DuckDB is doing the work.

Honestly though, often you won't even feel this pain, so we suggest waiting until you feel it before you optimize.
In our testing, even when DuckDB brought back half a million rows performance was still usable.  Say 4 seconds vs.
under 1.  I mean, yes, faster is good, but don't derail your train of thought until you have to.

### The General Rule

Whenever you have a large DuckDB table joining against InnoDB:

1. Move all DuckDB scanning, filtering, and aggregation into a CTE or subquery (same benefit)
2. Join the CTE result (small) against InnoDB (also small)
3. MariaDB only touches small row counts on both sides

This pattern works for any depth of aggregation — daily buckets, percentiles, window functions,
`RoundDateTime` time bucketing — as long as the CTE references only DuckDB tables.


---

## Extending HolyDuck

HolyDuck can be extended by editing `holyduck_duckdb_extensions.sql`.  This file runs when MariaDB starts and runs it directly in DuckDB, so any SQL that runs in DuckDB will work here, which includes macros and view creation.

### Function Translation - MariaDB to DuckDB

Where possible we put common MariaDB functions which don't exist in DuckDB in `holyduck_duckdb_extensions.sql` which includes functions such as:

- `DATE_FORMAT`
-  `IFNULL`
- `DATEDIFF` 
- `RoundDateTime` (OK, this isn't a standard function but it should be)
- etc.

### Function Pass Through - DuckDB to MariaDB

In addition to giving DuckDB some MariaDB functions we also sometimes need to be able to call DuckDB functions which have no equivalent in MariaDB. The problem is that MariaDB's parser will reject any unknown function before the SQL is pushed down to DuckDB.

The solution is a **dual implementation** across both engines.  For DuckDB we provide a convenience .sql file:

**`sql/holyduck_duckdb_extensions.sql`** — loaded into DuckDB automatically at plugin startup.
DuckDB sees this. This is loaded directly by HolyDuck into DuckDB, bypassing MariaDB entirely.

To make MariaDB aware of DuckDB functions we provide sample SQL in: 

**`sql/holyduck_mariadb_functions.sql`** — installed by the user into each MariaDB database.
MariaDB sees this. It satisfies the parser and provides a fallback for non-DuckDB tables.

### RoundDateTime()

We do a lot of time series analysis and downsampling of metrics so we use a function RoundDateTime()
that takes arbitrary seconds size buckets to round down to such as 60, 300, 600, etc. We'll use it
here as an example.  Maybe we only care about it in DuckDB but in order for DuckDB to receive the
SQL with it we must make MariaDB aware of it.

`holyduck_duckdb_extensions.sql` contains the DuckDB macro:
```sql
CREATE OR REPLACE MACRO rounddatetime(dt, bucket_secs) AS
    time_bucket(to_seconds(CAST(bucket_secs AS BIGINT)), dt::TIMESTAMP);
```

The problem at this point is that MariaDB won't pass RoundDateTime to DuckDB yet.  It will
see it as a missing or undeclared function.  We get around this by ALSO creating it for
MariaDB.

The user will have to install these functions per DB when needed, HolyDuck does not
automatically install them (unlike the DuckDB macros).

`holyduck_mariadb_functions.sql` contains the MariaDB stored function:
```sql
CREATE FUNCTION IF NOT EXISTS RoundDateTime(dt DATETIME, bucket_secs INT)
RETURNS DATETIME DETERMINISTIC
RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);
```
Notice that the definition is slightly different, as the DuckDB macro is optimized
for DuckDB but they are functionally equivalent.

We'll now go through why and how these two definitions work in different scenarios.

### Execution of RoundDateTime() in DuckDB
Query against a DuckDB table:
```sql
SELECT RoundDateTime(ts, 300) FROM duckdb_metrics;
```
MariaDB recognizes `RoundDateTime` via the stored function, which keeps it from throwing a
SQL error, then the pushdown fires. DuckDB receives the query and resolves `rounddatetime`
against its own macro — `time_bucket()` runs.
The stored function is never called.

### Execution of RoundDateTime() in InnoDB

```sql
SELECT RoundDateTime(ts, 300) FROM innodb_events;
```
No pushdown — this is an InnoDB table. MariaDB calls the stored function directly.
`FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs)` executes in MariaDB.
The DuckDB macro is never involved.

---

This pattern works for any function that has equivalent logic in both engines. If a correct
MariaDB fallback is not feasible, the stored function can instead raise an explicit error with
`SIGNAL SQLSTATE '45000'` rather than returning silently wrong results.

HolyDuck has a sample SQL file with supported MariaDB stored functions: holyduck_mariadb_functions.sql
which includes RoundDateTime()

We should point out that dual entries are only needed for NEW functions or features not in MariaDB.  In cases
when the issue is DuckDB lacks a function which exists in MariaDB you only need to create a new macro and restart
MariaDB.

## Views

You may createn normal MariaDB views via MariaDB and if you follow our guidelines for **Optimizing Queries for HolyDuck** from above you'll reap the same benefits.  On the other hand, if you create DuckDB views in `holyduck_duckdb_extensions.sql` you can take full advantage of DuckDB syntax and features. 

HolyDuck exposes DuckDB views as tables, which is a bit of a lie but so long as you don't accidentally attempt to write to them no errors should occur.

### Views as Language Extensions

DuckDB expressions which are not functions can't use either of the macro approaches above. In these cases you can create a view inside DuckDB itself using the full DuckDB SQL dialect — HolyDuck will make it queryable through MariaDB automatically.

**Step 1** — define the view in `holyduck_duckdb_extensions.sql`:
```sql
CREATE OR REPLACE VIEW mydb.v_my_view AS
    SELECT id, val * 2 AS double_val FROM mydb.my_table;
```

**Step 2** — restart MariaDB, then query it directly:
```sql
SELECT * FROM v_my_view;
```

That's it. HolyDuck discovers the view automatically the first time it's queried — no `CREATE TABLE` stub required. To remove it, delete it from `holyduck_duckdb_extensions.sql` and restart; no MariaDB cleanup needed.

The `v_` naming convention is recommended but not required. It signals to readers that the object is a DuckDB-native view rather than a regular table, and it ensures HolyDuck never overwrites an existing view if a `CREATE TABLE v_name ENGINE=DUCKDB` is ever issued manually.

### Views for BI Tools

BI tools (Tableau, Grafana, Power BI, etc.) generate their own SQL. Even when they do write CTEs, they may not structure them in a way that triggers efficient pushdown. The solution is to pre-bake the CTE pattern into a MariaDB view, so the tool always gets the right behavior regardless of what SQL it
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
