# Feature request: INSERT INTO ... SELECT FROM duckdb_table

**Reported:** 2026-04-05 from IntensityDB development work.
**Fixed:** 2026-04-05 in HolyDuck. See "Resolution" section below.

## Summary

Cross-engine `INSERT INTO other_table SELECT ... FROM duckdb_table ...`
currently fails with:

```
ERROR 1030 (HY000): Got error 1 "Operation not permitted" from storage engine DUCKDB
```

CTAS against DUCKDB works fine:
```sql
CREATE TABLE dest ENGINE=IntensityDB AS
  SELECT * FROM tpch_sm.lineitem ORDER BY l_orderkey LIMIT 60000000;
-- OK
```

But pre-creating the destination and loading via INSERT..SELECT does not:
```sql
CREATE TABLE dest (... explicit schema ...) ENGINE=IntensityDB;
INSERT INTO dest SELECT * FROM tpch_sm.lineitem ORDER BY l_orderkey LIMIT 60000000;
-- ERROR 1030 "Operation not permitted"
```

## Why this matters

The CTAS-derived schema inherits DuckDB's types (e.g. VARCHAR(255)
utf8mb4 for string columns). Users who want a specific target schema
(CHAR(1), narrow VARCHAR, latin1 charsets) cannot populate from a
DUCKDB source directly.

Current workaround is staging through InnoDB, which for TPC-H SF=10
lineitem (60M rows) takes ~30 minutes. That's unusable for
benchmark iteration.

## Expected behavior

`INSERT INTO <other-engine-table> SELECT ... FROM <duckdb-table>`
should stream rows from the DUCKDB source and insert them into the
destination engine's row-by-row path, the same way CTAS does today.

## Guess at root cause

Likely HolyDuck's storage-engine handler rejects `external_lock` or
`rnd_init` when the statement context is `INSERT...SELECT` rather than
`CREATE...SELECT`. The streaming reader used in the CTAS path already
exists (from the streaming fix earlier). Exposing it to the
`INSERT..SELECT` call path should be straightforward.

## Test case

```sql
CREATE TABLE test_dst (k BIGINT, v VARCHAR(50)) ENGINE=IntensityDB;
INSERT INTO test_dst SELECT k, v FROM duckdb_src.tbl;
-- currently fails with ERROR 1030
-- expected: streams rows, inserts them
```

## Context

- Reported by IntensityDB (sibling plugin) during 2026-04-05 benchmarks.
- IDB wanted to pre-declare CHAR(1)/latin1 schema for TPC-H flag
  columns, then load from `tpch_sm.lineitem` (DUCKDB engine, generated
  via `CALL dbgen(sf=10)`).
- See IntensityDB's `docs/sum_query_flamegraph_findings.md` and
  `docs/data_types_and_comparisons.md` for the storage-tuning work
  that motivated this.

## Resolution (2026-04-05)

**Root cause:** `ha_duckdb_select_handler::init_scan()` sent `thd->query()`
— the entire client statement — to DuckDB. For
`INSERT INTO dst SELECT ... FROM duckdb_src`, DuckDB received the full
`INSERT` text and either:
  - errored when `dst` was not visible inside DuckDB (IntensityDB
    destination → the reported ERROR 1030 "Operation not permitted"), or
  - executed its own INSERT against whatever DUCKDB-engine `dst` it could
    see, and returned a single-row "Count = N" result; the plugin then
    streamed that bogus count row back to MariaDB's INSERT loop, producing
    1 garbage row in the destination instead of N correct rows.

CTAS worked because the init_scan code already had a dedicated scan that
stripped "CREATE TABLE ... AS" before sending to DuckDB. INSERT..SELECT
had no equivalent strip.

**Fix** (`src/ha_duckdb.cc`): new helper `strip_to_top_level_select()`
scans for the first paren-depth-zero `SELECT` or `WITH` keyword and strips
everything before it. Replaces both duplicate CTAS-specific strip blocks
and transparently handles `INSERT ... SELECT`, `REPLACE ... SELECT`, and
any future DML-wrapping-a-SELECT form. Called from all three init_scan
SQL-building paths (all-DuckDB, mixed-engine single SELECT, mixed-engine
UNION/INTERSECT/EXCEPT).

**Verified**:
  - `INSERT INTO innodb_dst SELECT * FROM duckdb_src` — N rows, strings intact
  - `INSERT INTO char_dst(CHAR(1)) SELECT ...` — narrow schema (the IDB use case)
  - `INSERT INTO duckdb_dst SELECT ... FROM duckdb_src` — no extra Count row
  - `INSERT ... SELECT ... WHERE`, column lists, expressions — all work
  - All CTAS variants (→InnoDB, →DUCKDB, `AS WITH cte AS ...`) still pass
