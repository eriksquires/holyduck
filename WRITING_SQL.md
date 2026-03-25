# Writing SQL with HolyDuck
---
Please note that HolyDuck is under very active development. SQL compatibility and performance
are evolving rapidly, and this guide may already be out of date.

---

## Introduction

We chose not to rewrite a SQL parser or cost-based optimizer. For our use cases we felt we
could achieve our major goals with macros and a little manual SQL rewriting. Here we'll cover:

- How to write SQL which doesn't fail
- How cross-engine joins work, and when to think about query shape
- Extending HolyDuck with custom features
- Leveraging views

## SQL Dialect

**All SQL passes through MariaDB's parser first**. If MariaDB doesn't recognize the syntax or
function, it will never reach DuckDB — not even inside a subquery or CTE. This has practical
consequences for what you can and cannot write. For overcoming functional incompatibility, see
the **Extending HolyDuck** section below.

### What MariaDB blocks

DuckDB has powerful SQL extensions that MariaDB's parser will reject outright:

- `SELECT * EXCLUDE (col)` — MariaDB doesn't know `EXCLUDE`
- `PIVOT` / `UNPIVOT` — not in MariaDB's grammar
- `QUALIFY` clause (window function filtering) — MariaDB doesn't know it
- DuckDB column types in DDL: `LIST`, `STRUCT`, `MAP` — MariaDB won't parse them
- `SELECT * REPLACE (expr AS col)` — DuckDB extension, unknown to MariaDB

We'll cover working around these limitations below in **Extending HolyDuck.**

### What gets through

SQL that is valid MariaDB syntax will reach DuckDB, whether or not DuckDB can execute it.
DuckDB may then throw an error.

---

## Cross-Engine Joins

When your query uses tables in both DuckDB and InnoDB, HolyDuck handles it by injecting the
InnoDB tables as temporary tables inside DuckDB, then pushing the entire query to DuckDB as a
single unit (`PUSHED SELECT`). DuckDB executes the full join and aggregation natively — the
same as if both tables were in DuckDB to begin with.

For the typical HolyDuck workload — large DuckDB fact tables joined against small InnoDB
dimension tables — this just works. Write your queries naturally.

```sql
SELECT c.name AS category,
       r.name AS region,
       SUM(s.amount) AS total_sales,
       COUNT(*) AS order_count
FROM sales s
   JOIN categories c ON s.category_id = c.id
   JOIN regions r ON s.region_id = r.id
WHERE s.ts BETWEEN '2026-01-01' AND '2026-03-31'
GROUP BY c.name, r.name
ORDER BY total_sales DESC;
```

HolyDuck injects `categories` and `regions` into DuckDB and DuckDB runs the whole thing.
When you `EXPLAIN` a cross-engine query, `PUSHED SELECT` confirms DuckDB is doing the work.

### When to Think About Query Shape

The one case worth considering is a genuinely large InnoDB table. Injection reads the entire
InnoDB table and copies it into DuckDB. If that table has millions of rows, pre-aggregating the
DuckDB side first via a CTE can help — DuckDB produces a small result set, and MariaDB joins
that against InnoDB using its own indexes rather than copying the whole table across.

```sql
WITH sales_summary AS (
    SELECT category_id,
           region_id,
           SUM(amount) AS total_sales,
           COUNT(*) AS order_count
    FROM sales
    WHERE ts BETWEEN '2026-01-01' AND '2026-03-31'
    GROUP BY category_id, region_id
)
SELECT c.name AS category,
       r.name AS region,
       ss.total_sales,
       ss.order_count
FROM sales_summary ss
JOIN categories c ON ss.category_id = c.id
JOIN regions r ON ss.region_id = r.id
ORDER BY ss.total_sales DESC;
```

Here `PUSHED DERIVED` fires on `sales_summary` — DuckDB aggregates to ~50 rows, and MariaDB
joins those against InnoDB. No large injection needed.

Wait until you feel actual performance pain before restructuring queries. Most workloads won't
need it.


---

## Extending HolyDuck

HolyDuck can be extended by editing `holyduck_duckdb_extensions.sql`. This file is executed
directly inside DuckDB at MariaDB startup, so any valid DuckDB SQL works here — including
macros and view creation.

### Function Translation — MariaDB to DuckDB

Where MariaDB functions have no DuckDB equivalent, we provide DuckDB macros in
`holyduck_duckdb_extensions.sql`:

- `DATE_FORMAT`
- `IFNULL`
- `DATEDIFF`
- `RoundDateTime` (not a standard function, but it should be)
- etc.

### Function Pass-Through — DuckDB to MariaDB

Sometimes you need to call a DuckDB function that has no equivalent in MariaDB. The problem
is that MariaDB's parser will reject any unknown function before the SQL is pushed down to
DuckDB.

The solution is a **dual implementation** across both engines:

**`sql/holyduck_duckdb_extensions.sql`** — loaded into DuckDB automatically at plugin startup.
DuckDB sees this, bypassing MariaDB entirely.

**`sql/holyduck_mariadb_functions.sql`** — installed by the user into each MariaDB database.
MariaDB sees this. It satisfies the parser and provides a fallback for non-DuckDB tables.

### RoundDateTime()

We do a lot of time-series analysis and downsampling of metrics, so we use `RoundDateTime()`
to round timestamps to arbitrary second-size buckets: 60, 300, 600, etc. We'll use it here
as an example.

`holyduck_duckdb_extensions.sql` contains the DuckDB macro:
```sql
CREATE OR REPLACE MACRO rounddatetime(dt, bucket_secs) AS
    time_bucket(to_seconds(CAST(bucket_secs AS BIGINT)), dt::TIMESTAMP);
```

MariaDB won't pass `RoundDateTime` to DuckDB until it recognizes it. We get around this by
also creating it as a MariaDB stored function.

The user installs these functions per-database as needed — HolyDuck does not install them
automatically (unlike the DuckDB macros).

`holyduck_mariadb_functions.sql` contains the MariaDB stored function:
```sql
CREATE FUNCTION IF NOT EXISTS RoundDateTime(dt DATETIME, bucket_secs INT)
RETURNS DATETIME DETERMINISTIC
RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);
```

The DuckDB macro and MariaDB stored function are functionally equivalent but independently
optimized for their respective engines.

### Execution of RoundDateTime() in DuckDB
Query against a DuckDB table:
```sql
SELECT RoundDateTime(ts, 300) FROM duckdb_metrics;
```
MariaDB recognizes `RoundDateTime` via the stored function, suppressing the parser error, then
pushdown fires. DuckDB receives the query and resolves `rounddatetime` against its own macro —
`time_bucket()` runs. The stored function is never called.

### Execution of RoundDateTime() in InnoDB

```sql
SELECT RoundDateTime(ts, 300) FROM innodb_events;
```
No pushdown — this is an InnoDB table. MariaDB calls the stored function directly.
`FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs)` executes in MariaDB.
The DuckDB macro is never involved.

---

This pattern works for any function that has equivalent logic in both engines. If a correct
MariaDB fallback is not feasible, the stored function can raise an explicit error with
`SIGNAL SQLSTATE '45000'` rather than returning silently wrong results.

Note that dual entries are only needed for functions that are new to MariaDB. When the issue
is the reverse — DuckDB lacks a function that exists in MariaDB — you only need to create a
macro in `holyduck_duckdb_extensions.sql` and reload it.

## Views

You may create standard MariaDB views over DuckDB tables — HolyDuck pushes queries through
them the same as direct queries. Alternatively, creating DuckDB views inside
`holyduck_duckdb_extensions.sql` lets you use the full DuckDB SQL dialect.

HolyDuck exposes DuckDB views as queryable tables. They cannot be written to.

There is no inherent performance benefit from DuckDB views vs. MariaDB views — the benefit is
access to DuckDB's extended syntax.

### Views as Language Extensions

DuckDB expressions that are not functions can't use the macro approach above. In these cases
you can create a view inside DuckDB using the full DuckDB SQL dialect — HolyDuck makes it
queryable through MariaDB automatically.

**Step 1** — define the view in `holyduck_duckdb_extensions.sql`:
```sql
CREATE OR REPLACE VIEW mydb.v_my_view AS
    SELECT id, val * 2 AS double_val FROM mydb.my_table;
```

**Step 2** — reload the extensions file:
```sql
SET GLOBAL duckdb_reload_extensions = 1;
```
That's it — `v_my_view` is now usable from MariaDB. HolyDuck discovers the view automatically
the first time it's queried.

To remove a view, add a `DROP VIEW IF EXISTS` line to `holyduck_duckdb_extensions.sql` and
delete the `CREATE OR REPLACE VIEW` for it, then reload:
```sql
SET GLOBAL duckdb_reload_extensions = 1;
```

### Views for BI Tools

BI tools (Tableau, Grafana, Power BI, etc.) generate their own SQL and have no awareness of
the underlying engines. A MariaDB view over a DuckDB table works transparently — the tool
queries it like any other table and HolyDuck handles pushdown automatically.

Views are also useful for exposing a pre-shaped interface. For example, if a tool always needs
aggregated sales by category and region, a view locks in that shape:

```sql
CREATE VIEW sales_summary AS
    SELECT category_id, region_id,
           SUM(amount) AS total_sales,
           COUNT(*) AS order_count
    FROM sales
    GROUP BY category_id, region_id;
```

The tool queries `sales_summary` like a plain table; the aggregation runs inside DuckDB.
