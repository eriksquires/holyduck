# TPC-H Compatibility Notes

This document tracks issues encountered when running TPC-H 3.0.1 queries through
HolyDuck, separated by root cause.

---

## Part 1: HolyDuck fixes (MariaDB printer → DuckDB translation gaps)

These are bugs or gaps in HolyDuck's SQL rewrite layer.  The TPC-H SQL is valid
standard SQL; the problem is that MariaDB's internal query printer emits syntax
that DuckDB rejects.  The fix belongs in `rewrite_mariadb_sql()`.

### `INTERVAL 'N' unit` — FIXED

**Affected queries:** Q1, Q4, Q5, Q6, Q10

MariaDB's query printer emits `INTERVAL 'N' UNIT` (quoted integer, separate unit
keyword).  DuckDB requires number and unit in a single string: `INTERVAL '90 days'`.

**Fix:** Rewrite `interval 'N' unit[s]` → `interval 'N units'` before pushdown.

---

### `DATE'YYYY-MM-DD'` (no space) — FIXED

**Affected queries:** Q1, Q3, Q4, Q5, Q6, Q7, Q8, Q10

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

### `EXISTS (SELECT * ...)` causes SIGSEGV — OPEN BUG

**Affected queries:** Q4 (and potentially others)

`EXISTS (SELECT * FROM <duckdb_table> WHERE ...)` crashes MariaDB with signal 11.
`EXISTS (SELECT 1 ...)` works correctly.  Both are semantically identical per the SQL
standard.  The crash is in HolyDuck's handling of the expanded column list inside an
EXISTS subquery.

**Workaround:** Use `SELECT 1` inside `EXISTS` in regression test SQL.
**Status:** Root cause under investigation.

---

## Part 2: TPC-H SQL adaptations

These are cases where the raw TPC-H query templates require modification before they
can run against any database, or where the SQL constructs are valid standard SQL but
not accepted by MariaDB's parser.

### `qgen` template syntax must be substituted

The raw files in `TPC_H_3_0_1/dbgen/queries/` are `qgen` templates, not runnable SQL:

- `:x`, `:o`, `:n N` — output/format directives (strip entirely)
- `':1'`, `':2'`, `':3'` — parameter substitution variables

The regression test files in `tests/regression/tpch/` have canonical parameter
values substituted in per the TPC-H specification.

---

### `LIMIT` not in original queries

Several TPC-H queries specify a row limit via `:n N` in the template (e.g. Q2 limits
to 100 rows, Q10 to 20 rows).  These become `LIMIT N` clauses in the regression SQL.

---

## Summary table

| Issue | Root cause | Status |
|---|---|---|
| `INTERVAL 'N' unit` syntax | MariaDB printer | Fixed in rewrite pass |
| `DATE'...'` missing space | MariaDB printer | Fixed in rewrite pass |
| `<in_optimizer>` wrapper | MariaDB optimizer | Fixed in rewrite pass |
| `EXISTS (SELECT *)` crash | HolyDuck bug | Open — workaround: use `SELECT 1` |
| `qgen` template variables | TPC-H template format | Substituted in test SQL |
