# Performance Notes

This document tracks observed performance characteristics of HolyDuck's three
execution modes and suggests concrete optimisations, ordered roughly by expected
impact.

---

## Benchmark Methodology

All results use the TPC-H benchmark at the stated scale factor.
`tests/performance/run.pl` runs 1 warmup + 3 timed iterations per query and
reports the arithmetic mean.  Three modes are tested:

| Mode | Description |
|------|-------------|
| **standalone** | Host `duckdb` binary, read-only against the perf `.duckdb` file |
| **SM** | All eight TPC-H tables in DuckDB, accessed via MariaDB + the plugin |
| **MM** | Fact tables (lineitem, orders, partsupp, customer) in DuckDB; dimension tables (nation, region, part, supplier) in InnoDB |

Setup: `bash tests/performance/setup.sh` — stops MariaDB, generates data with
the DuckDB CLI directly against `global.duckdb`, restarts MariaDB, creates
InnoDB dimension tables.  Change `SF=` at the top of the script to rescale.

---

## SF=1 Baseline (for reference)

All 22 queries pass in all three modes.  At SF=1 everything fits in cache and
no query exceeds ~0.42 s.  SM is consistently 2–4× faster than standalone due
to the warm, persistent in-process DuckDB connection.

MM queries that inject InnoDB tables show a ~350 ms floor even at SF=1, which
is dominated by the InnoDB→DuckDB bulk-copy cost rather than query execution.

---

## MM Mode: InnoDB Injection Cost

When a query references an InnoDB dimension table, `inject_table_into_duckdb()`
does a full `ha_rnd_init` / `ha_rnd_next` scan and copies every row into a
DuckDB `TEMP TABLE` via the Appender API.  At SF=10 the affected tables are:

| Table | SF=1 rows | SF=10 rows | Injection cost |
|-------|-----------|------------|----------------|
| nation | 25 | 25 | negligible |
| region | 5 | 5 | negligible |
| supplier | 10 K | 100 K | ~50 ms |
| part | 200 K | 2 M | **~3–5 s** |

Queries that inject `part` (Q2, Q14, Q16, Q17, Q19) will dominate at SF=10+.

---

## Optimisation 1 — Predicate pushdown into InnoDB injection (HIGH IMPACT)

**Status:** Not implemented.

**Problem:** Every injection copies the full table regardless of which rows the
query actually needs.  For Q16 at SF=10, `part` has 2 M rows but the WHERE
clause filters to a few thousand:

```sql
WHERE p_brand <> 'Brand#45'
  AND p_type NOT LIKE 'MEDIUM POLISHED%'
  AND p_size IN (49, 14, 23, 45, 19, 3, 36, 9)
```

All three conditions reference only `part` columns — they can be pushed into the
InnoDB scan and evaluated before any row crosses the engine boundary.

**Approach:** In `init_scan`, before calling `inject_table_into_duckdb`, walk
`sel->where` (the MariaDB condition tree) and collect predicates whose
`used_tables()` bitmap intersects only the table being injected.  Convert those
predicates to a SQL WHERE string (the same machinery used by `cond_push`) and
pass it to a filtered scan instead of `ha_rnd_init(full_scan)`.

Single-table equality and range conditions are easy wins.  Cross-table join
conditions (`s_nationkey = n_nationkey`) span two engines and cannot be pushed
unilaterally — skip them.

**Expected gain:** Q14/Q16/Q17/Q19 drop from O(full table) to O(result set).
At SF=10, ~3–5 s injection becomes <100 ms for selective predicates.  At SF=100
the difference is tens of seconds vs milliseconds.

---

## Optimisation 2 — TEMP TABLE caching across queries (MEDIUM IMPACT)

**Status:** Not implemented.

**Problem:** The `IF NOT EXISTS` guard in `inject_table_into_duckdb` prevents
duplicate schema creation, but we still call `ha_rnd_init` + iterate all rows
on every query because there is no per-connection flag tracking whether the data
was already loaded.

**Approach:** Tag each DuckDB connection (or use a `std::unordered_set` keyed
on connection pointer + table name) to record which TEMP TABLEs have been
populated.  On subsequent queries in the same session, skip the Appender loop
entirely.  Invalidate the cache on any InnoDB write to the table (can use the
handler's `write_row` / `update_row` notification, or simply time-bound the
cache with a sequence number from the InnoDB table's `stats.update_time`).

**Expected gain:** Second and subsequent queries in a session that touch the
same dimension table pay zero injection cost.  Most OLAP workloads run multiple
queries per session so this compounds with optimisation 1.

---

## Optimisation 3 — Connection pool / persistent DuckDB connection (MEDIUM IMPACT)

**Status:** Not implemented.

**Problem:** Each MariaDB connection creates a fresh DuckDB `Connection` object.
DuckDB's own connection setup is cheap, but TEMP TABLEs (from injection) are
connection-scoped and lost when the connection closes.  Long-running analytical
sessions pay the injection cost on every new connection.

**Approach:** Maintain a pool of DuckDB connections associated with each
`duckdb::DuckDB` instance.  Connections in the pool retain their TEMP TABLEs.
When a MariaDB session starts, check out a pooled connection; on session end,
return it.  Pair with optimisation 2 so returned connections retain their
cached data.

---

## Optimisation 4 — Vectorised InnoDB→DuckDB copy (LOW-MEDIUM IMPACT)

**Status:** Not implemented.

**Problem:** `inject_table_into_duckdb` copies rows one at a time via
`ha_rnd_next` + `Appender::BeginRow/EndRow`.  Each call crosses the
MariaDB→DuckDB C++ boundary individually.

**Approach:** Buffer rows in a local `std::vector` (e.g. 10 K rows) and
call `Appender::AppendRow` in bulk, or use DuckDB's `DataChunk` API to
append columnar batches.  Also consider enabling InnoDB's MRR (multi-range
read) interface for the scan to improve InnoDB-side read-ahead.

**Expected gain:** Modest — probably 20–40% reduction in injection time.
The dominant cost at large SF is the volume of data, not per-row overhead,
so this matters less than predicatepushdown (opt 1).

---

## Optimisation 5 — Parallel injection of independent tables (LOW IMPACT)

**Status:** Not implemented.

**Problem:** When multiple InnoDB tables must be injected (e.g. Q8 injects
nation + region + supplier), they are copied sequentially.

**Approach:** Spawn one `std::thread` per table, each appending into the same
DuckDB connection (the Appender API is not thread-safe per connection, so use
separate connections that share the same `DuckDB` instance, then merge via
`INSERT INTO ... SELECT * FROM tmp_conn2.nation`).  Alternatively, use
DuckDB's async query API if exposed.

**Expected gain:** Useful when several large dimension tables must be injected
simultaneously, but at typical TPC-H scale the bottleneck is `part` alone so
parallelism doesn't help the common case.

---

## Known Bugs

### UNION queries broken in mixed-mode (MM/SM)

**Status:** Not fixed.

Queries using `UNION`, `INTERSECT`, or `EXCEPT` go through the AST printer
path (`lex_unit->print()`) instead of the original-SQL path used by plain
`SELECT`.  The AST printer has the same class of bugs that were fixed for
plain SELECTs: it drops correlation predicates, mangles certain join syntax,
and may mis-quote identifiers.

None of the 22 standard TPC-H queries use top-level UNION, so this does not
affect the benchmark.  User queries combining mixed-engine results with set
operations will produce wrong results or errors.

**Approach:** Identify which set-operation queries are affected and apply the
same original-SQL approach where possible.  For cases where the original SQL
cannot be recovered cleanly (e.g. statement-level rewriting), find a safe
AST-printer path or reject the query with a clear error rather than returning
silently wrong results.

---

## SM Mode Observations

SM is already 2–4× faster than standalone on most queries because:

1. The DuckDB connection is persistent and warm (no file open overhead).
2. DuckDB's buffer pool retains hot data between queries.
3. No network or process-spawn overhead vs the `duckdb` CLI.

**Q9 anomaly:** SM Q9 is ~0.6× of standalone at SF=1 but degrades relative
to other SM queries.  Q9 is the most join-heavy TPC-H query (6 tables, a
self-join on nation via supplier/customer).  Worth profiling at higher SF
to see if it is compute-bound (expected) or has a pathological plan.

**Q16 SM overhead:** SM Q16 takes slightly *longer* than standalone at SF=1
(1.13×).  Q16 uses `NOT IN (subquery)` which DuckDB rewrites as an
anti-join.  The persistent connection may be holding a stale plan cache
entry.  Re-test at SF=10 to confirm whether this is noise or structural.
