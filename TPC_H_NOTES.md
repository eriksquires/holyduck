# TPC-H Compatibility Notes

This document tracks issues encountered when running TPC-H 3.0.1 queries through
HolyDuck, separated by root cause.

---

## Part 1: HolyDuck fixes (MariaDB printer → DuckDB translation gaps)

These are bugs or gaps in HolyDuck's SQL rewrite layer.  The TPC-H SQL is valid
standard SQL; the problem is that MariaDB's internal query printer emits syntax
that DuckDB rejects.  The fix belongs in `rewrite_mariadb_sql()`.

### `INTERVAL 'N' unit` — FIXED

**Affected queries:** Q1, Q4, Q5, Q6, Q10, Q12, Q14, Q20

MariaDB's query printer emits `INTERVAL 'N' UNIT` (quoted integer, separate unit
keyword).  DuckDB requires number and unit in a single string: `INTERVAL '90 days'`.

**Fix:** Rewrite `interval 'N' unit[s]` → `interval 'N units'` before pushdown.

---

### `DATE'YYYY-MM-DD'` (no space) — FIXED

**Affected queries:** Q1, Q3, Q4, Q5, Q6, Q7, Q8, Q10, Q12, Q14, Q20

MariaDB's query printer emits ANSI date literals without a space between the `DATE`
keyword and the string (e.g. `DATE'1998-12-01'`).  DuckDB requires the space.
Without it, DuckDB misparsed the expression and subsequent arithmetic failed.

**Fix:** Rewrite `DATE'...'` → `DATE '...'` before pushdown.

---

### `<in_optimizer>(val, expr)` — FIXED

**Affected queries:** Q4 and any query using `EXISTS`

MariaDB's optimizer wraps `IN`/`EXISTS` subqueries in an internal `Item_in_optimizer`
node printed as `<in_optimizer>(val, expr)`.  DuckDB rejects this syntax.

**Fix:** Strip the wrapper, emit just the inner `expr`.

---

### `!(<exists>(expr))` and `!exists(expr)` — FIXED

**Affected queries:** Q16 (`NOT IN` subquery), Q21 (`NOT EXISTS`)

MariaDB's optimizer emits two forms of negated subqueries:
- `NOT IN`  → `!(<exists>(select ...))`
- `NOT EXISTS` → `!exists(select ...)`

Both are rejected by DuckDB.

**Fix:** Rewrite `!(<exists>(expr))` → `NOT EXISTS (expr)` and `!exists(expr)` →
`NOT EXISTS (expr)`.

---

### `<expr_cache><key>(expr)` — FIXED

**Affected queries:** Q22 (when `NOT EXISTS` is used inside a derived table)

MariaDB's optimizer emits `<expr_cache><col_ref>(expr)` for cached subquery
lookups during materialization.  DuckDB rejects the `<...>` syntax.

**Fix:** Strip the wrapper, emit just the inner `expr`.

---

### `EXISTS (SELECT * ...)` causes SIGSEGV — OPEN BUG

**Affected queries:** Q4 and any query using `EXISTS (SELECT * ...)`

`EXISTS (SELECT * FROM <duckdb_table> WHERE ...)` crashes MariaDB with signal 11.
`EXISTS (SELECT 1 ...)` works correctly.  Both are semantically identical per the SQL
standard.  The crash is in HolyDuck's handling of the expanded column list inside an
EXISTS subquery.

**Workaround:** Use `SELECT 1` inside `EXISTS` in regression test SQL.
**Status:** Root cause under investigation.

---

### Derived table in FROM crashes MariaDB — OPEN BUG

**Affected queries:** Q15, Q22 (original form)

`SELECT ... FROM (SELECT ... FROM <duckdb_table> ...) AS alias` crashes MariaDB
with signal 11 when the derived table references DuckDB tables and contains certain
subquery patterns (`NOT EXISTS`, correlated subqueries).

The outer derived table triggers MariaDB's subquery materialization path which emits
internal nodes (`<materialize>`, `<primary_index_lookup>`, `<temporary table>`,
`"<subquery3>"`) that cannot be translated to DuckDB SQL.

**Workaround for Q15:** Use CTE form — but CTE references are also stripped before
pushdown (tracked as separate issue; see Q15 notes below).  Q15 is blocked.

**Workaround for Q22:** Rewrite to eliminate the outer derived table — push the
aggregation directly into the outer SELECT.

**Status:** Root cause under investigation.

---

### CTE references stripped from pushed queries — OPEN BUG

**Affected queries:** Q15

When a CTE is defined at the top level and referenced in a join, MariaDB's
`select_handler` receives the outer query with the CTE reference as a plain table
name — the `WITH` clause is not passed through.  DuckDB reports
"Table with name revenue does not exist".

**Status:** Q15 is currently blocked by this bug.

---

## Part 2: TPC-H SQL adaptations

These are cases where the raw TPC-H query templates require modification before they
can run, or where the SQL constructs are valid standard SQL but require adjustment
for the HolyDuck environment.

### `qgen` template syntax must be substituted

The raw files in `TPC_H_3_0_1/dbgen/queries/` are `qgen` templates, not runnable SQL:

- `:x`, `:o`, `:n N` — output/format directives (strip entirely)
- `':1'`, `':2'`, `':3'` — parameter substitution variables

The regression test files in `tests/regression/tpch/` have canonical parameter
values substituted in per the TPC-H specification.

---

### Q15 — uses `CREATE VIEW` / `DROP VIEW`

The standard Q15 template creates a view `revenue`, queries it, then drops it.
HolyDuck does not support DDL in the middle of a query sequence through the rewrite
layer.  A CTE rewrite is the natural replacement, but is blocked by the CTE
reference bug above.  Q15 is currently skipped.

---

### Q22 — outer derived table rewritten

The standard Q22 wraps its filter logic in a derived table `custsale` and aggregates
the outer result.  This triggers the derived-table crash bug.  The query is rewritten
to inline the aggregation directly, eliminating the outer derived table.

---

### Q8 — zero results at sf=0.01

The canonical Q8 parameters (BRAZIL, AMERICA, ECONOMY ANODIZED STEEL) return
`mkt_share = 0` at scale factor 0.01.  This is data sparsity, not a bug.

---

## Summary table

| Issue | Root cause | Status |
|---|---|---|
| `INTERVAL 'N' unit` syntax | MariaDB printer | Fixed |
| `DATE'...'` missing space | MariaDB printer | Fixed |
| `<in_optimizer>` wrapper | MariaDB optimizer | Fixed |
| `!(<exists>)` / `!exists()` wrappers | MariaDB optimizer | Fixed |
| `<expr_cache><key>()` wrapper | MariaDB optimizer | Fixed |
| `EXISTS (SELECT *)` crash | HolyDuck bug | Open — use `SELECT 1` |
| Derived table in FROM crash | HolyDuck bug | Open — Q15 blocked, Q22 rewritten |
| CTE references stripped | HolyDuck bug | Open — Q15 blocked |
| Q15 `CREATE VIEW` form | TPC-H template | Blocked (see above) |
| Q22 outer derived table | TPC-H structure | Rewritten to avoid |
| Q8 zero results at sf=0.01 | Data sparsity | Expected |

## Query status

| Query | Status | Notes |
|---|---|---|
| Q1  | Pass | |
| Q2  | Pass | |
| Q3  | Pass | |
| Q4  | Pass | `SELECT 1` in EXISTS |
| Q5  | Pass | |
| Q6  | Pass | |
| Q7  | Pass | |
| Q8  | Pass | Zero results at sf=0.01 (correct) |
| Q9  | Pass | |
| Q10 | Pass | |
| Q11 | Pass | |
| Q12 | Pass | |
| Q13 | Pass | |
| Q14 | Pass | |
| Q15 | Blocked | CTE ref stripped + derived table crash |
| Q16 | Pass | |
| Q17 | Pass | NULL result at sf=0.01 (correct) |
| Q18 | Pass | |
| Q19 | Pass | |
| Q20 | Pass | |
| Q21 | Pass | `SELECT 1` in EXISTS |
| Q22 | Pass | Outer derived table rewritten |
