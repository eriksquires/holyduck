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

## How to test

```sql
-- This table is ENGINE=IntensityDB, NOT DuckDB.
-- 60M rows, 8.2 GB on disk.
EXPLAIN SELECT COUNT(*) FROM tpch_idb_sf10.lineitem;
```

**Current (broken):** Shows `PUSHED SELECT`, takes ~30s.

**Expected (fixed):** Should NOT show `PUSHED SELECT`. MariaDB should use
IntensityDB's `records()` method, which returns instantly by summing page
header nrows values.

### Verification after fix

```sql
-- Should NOT be PUSHED SELECT
EXPLAIN SELECT COUNT(*) FROM tpch_idb_sf10.lineitem;
-- Expected: simple SELECT with no pushdown (uses HA_HAS_RECORDS → records())

-- Should complete in < 1 second
SELECT COUNT(*) FROM tpch_idb_sf10.lineitem;
-- Expected: 59986052

-- DuckDB queries should still be pushed (this must NOT break)
EXPLAIN SELECT COUNT(*) FROM tpch_sm.lineitem;
-- Expected: PUSHED SELECT (correct — tpch_sm is a DuckDB table)

-- Mixed-engine queries should not be pushed to DuckDB
EXPLAIN SELECT COUNT(*) FROM tpch_idb_sf10.lineitem l
  JOIN tpch_sm.orders o ON l.l_orderkey = o.o_orderkey;
-- Expected: NOT pushed (mixed engines)
```

### GDB verification

Breakpoint on `create_duckdb_select_handler` should NOT fire for queries
where all leaf tables are non-DuckDB:

```
gdb -batch -p <pid> \
  -ex "break create_duckdb_select_handler" \
  -ex "commands 1" -ex "bt 3" -ex "continue" -ex "end" \
  -ex "continue"
```

Then run: `SELECT COUNT(*) FROM tpch_idb_sf10.lineitem`

If the breakpoint fires and returns non-null, the bug is still present.

## Impact

- `SELECT COUNT(*)` on IntensityDB tables: 30s → should be <1s via `records()`
- Any pushed SELECT on non-DuckDB tables triggers a full table copy into DuckDB
- Memory: DuckDB materializes the full table as a temp table
- This affects ALL queries on non-DuckDB tables that DuckDB's handler claims

---

## Update: 2026-04-04 — DuckDB is not the thief

### GDB findings

A HolyDuck fix was applied to guard `create_duckdb_select_handler()` against
claiming queries with no DuckDB leaf tables (commit af4b595). However, EXPLAIN
still showed `PUSHED SELECT` for `SELECT COUNT(*) FROM tpch_idb_sf10.lineitem`.

GDB breakpoints on all three DuckDB handler factories (`create_duckdb_select_handler`,
`create_duckdb_unit_handler`, `create_duckdb_derived_handler`) were **never hit**
for this query. Yet the EXPLAIN showed `PUSHED SELECT`.

Breaking on `create_intensity_select_handler` instead:

```
Thread 38 "one_connection" hit Breakpoint 1, create_intensity_select_handler
  at ha_intensitydb.cc:1226
#1  find_select_handler_inner at sql_select.cc:5219
```

**IntensityDB's own select handler is claiming the query**, not DuckDB.

### What this means

1. The original bug analysis was wrong — DuckDB was never the thief for this
   query. MariaDB calls `create_select` on each engine in turn; IntensityDB
   gets asked first (it owns the table) and is returning a handler instead of
   nullptr.

2. The bug doc's GDB trace (showing IntensityDB returning nullptr for
   `only_count_star=true`) may reflect an older version of IntensityDB that
   correctly declined. The current version at `ha_intensitydb.cc:1226` does
   not decline — it claims the query and runs a full scan through DuckDB's
   materialized path.

3. The HolyDuck fix (guarding the subquery fallback) is still correct and
   useful — it prevents DuckDB from claiming pure non-DuckDB queries when
   `query_tables` happens to contain DuckDB tables from unrelated parts of
   the query tree. But it's not the fix for this specific bug.

### Actual fix needed

The fix belongs in **IntensityDB**, not HolyDuck:

- `create_intensity_select_handler()` at `ha_intensitydb.cc:1226` should
  detect bare `COUNT(*)` queries (no GROUP BY, no WHERE, single table) and
  return nullptr so MariaDB uses `records()` instead.

- The `only_count_star` detection logic referenced in the original bug doc
  either never landed, was reverted, or has a bug in its condition.
