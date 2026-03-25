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

### `EXISTS (SELECT * ...)` — FIXED

**Affected queries:** Q4, Q21, and any query using `EXISTS (SELECT * ...)`

Previously caused a SIGSEGV when MariaDB's AST printer expanded `SELECT *` inside
an EXISTS subquery into an internal column list that HolyDuck could not handle.

**Fix:** The original-SQL path passes `EXISTS (SELECT * ...)` directly to DuckDB,
which handles it natively.  No rewrite or workaround needed.

---

### Derived table in FROM with `NOT EXISTS` — FIXED

**Affected queries:** Q22

`NOT EXISTS` inside a derived table caused the derived_handler's AST printer to
emit three chained artifacts that DuckDB rejects:

1. `<materialize>(SELECT ...)` — materialized subquery wrapper
2. `, <primary_index_lookup>(col IN <temporary table>)` — key-lookup shortcut
   appended to the IN list (MariaDB sees the DuckDB table's primary key and
   plans an index lookup)
3. `!(expr)` — C-style logical NOT instead of SQL `NOT`

**Fix:** Added three strippers to `rewrite_mariadb_sql()`:
- Strip `<materialize>` prefix (keeping the parenthesised subquery)
- Strip `, <primary_index_lookup>(...)` list element (keeping the real subquery)
- Rewrite `!(` → `NOT (` (avoiding `!=`)

---

### CTE references stripped from pushed queries — FIXED

**Affected queries:** Q15

When a CTE is defined at the top level and referenced in a join, MariaDB's
`select_handler` previously received only the outer query — the `WITH` clause
was not passed through via the AST printer.

**Fix:** The original-SQL path (`thd->query()`) includes the full `WITH ... SELECT`
as submitted by the client.  CTE-backed leaves in `leaf_tables` are treated as
DuckDB-compatible (they don't set `all_duckdb=false`), so the query routes to the
original-SQL path and DuckDB receives the complete CTE definition.

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
HolyDuck does not support DDL in the middle of a query sequence.  The CTE rewrite
(`WITH revenue AS (...)`) is the natural replacement and now works correctly: the
original-SQL path passes the full `WITH ... SELECT` to DuckDB unchanged.

---

### Q22 — original form now works

The standard Q22 wraps its filter logic in a derived table `custsale`.  Three
chained MariaDB AST artifacts (`<materialize>`, `<primary_index_lookup>`, `!(`)
are now stripped in `rewrite_mariadb_sql()`.  The verbatim TPC-H SQL runs correctly.

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
| `EXISTS (SELECT *)` crash | HolyDuck bug | Fixed |
| Derived table in FROM (derived_handler) | HolyDuck bug | Fixed |
| CTE references stripped | HolyDuck bug | Fixed |
| Q15 `CREATE VIEW` form | TPC-H template | Use CTE form (now works) |
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
| Q15 | Pass | CTE form; original-SQL path includes WITH clause |
| Q16 | Pass | |
| Q17 | Pass | NULL result at sf=0.01 (correct) |
| Q18 | Pass | |
| Q19 | Pass | |
| Q20 | Pass | |
| Q21 | Pass | `SELECT 1` in EXISTS |
| Q22 | Pass | Original SQL; derived_handler artifacts stripped |
