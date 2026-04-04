# Bug: DuckDB select_handler claims COUNT(*) on non-DuckDB tables

## Problem

`SELECT COUNT(*) FROM tpch_idb_sf10.lineitem` takes 30s even though the table
is IntensityDB (not DuckDB). IntensityDB's `records()` method can return the
count in O(pages) by summing page header nrows.

## Root cause

`create_duckdb_select_handler()` in `ha_duckdb.cc:2997` claims the query
even though the leaf table is an IntensityDB engine table, not DuckDB.

IntensityDB's `create_intensity_select_handler()` correctly returns nullptr
for bare COUNT(*) (line 1272), intending MariaDB to use `records()`. But
MariaDB then asks DuckDB's handler, which claims it and runs a full scan.

## Evidence (GDB trace)

```
Thread 39 "one_connection" hit Breakpoint 1, create_intensity_select_handler
  → returns nullptr (only_count_star=true)

Breakpoint 2 hit: ">>> skipping COUNT(*) to records()"
  → IntensityDB rejected the query

EXPLAIN shows: PUSHED SELECT
  → someone else claimed it (DuckDB)

Query takes 30s (full page scan via DuckDB's materialized path)
```

## Fix

In `create_duckdb_select_handler()`, check that at least one leaf table
uses the DuckDB engine before claiming the query. If all leaf tables are
non-DuckDB, return nullptr.

The check around line 2997-3040 of `ha_duckdb.cc` should verify:
```cpp
// Only claim if at least one leaf table is a DuckDB table
bool has_duckdb_table = false;
for each leaf table:
    if (table->file->ht == duckdb_hton) has_duckdb_table = true;
if (!has_duckdb_table) return nullptr;
```

## What happens after DuckDB claims it

1. `init_scan()` rewrites the SQL and runs it via `SendQuery()` against DuckDB
2. DuckDB doesn't have the IntensityDB table in its catalog
3. The retry loop detects the missing table and calls `inject_table_into_duckdb()`
4. Injection reads the **entire IntensityDB table** via `ha_rnd_next()` into a
   DuckDB temp table — a full 60M row copy through MariaDB's row-at-a-time interface
5. DuckDB then runs `COUNT(*)` on the copy — which is instant, but the damage is done

So it's not just stealing the query — it copies the entire table into DuckDB's
memory first. For 60M rows this takes ~30s and consumes significant memory.

## Impact

- `SELECT COUNT(*)` on IntensityDB tables: 30s → should be <1s via `records()`
- Any pushed SELECT on non-DuckDB tables triggers a full table copy into DuckDB
- Memory: DuckDB materializes the full table as a temp table
- This affects ALL queries on non-DuckDB tables that DuckDB's handler claims
