# TPC-H Compatibility Notes

This document tracks SQL compatibility issues encountered when running TPC-H 3.0.1
queries through HolyDuck, and how they are resolved.

---

## Rewrites added to `rewrite_mariadb_sql`

### `INTERVAL 'N' unit` → `INTERVAL 'N units'`

**Affected queries:** Q1, Q4, Q5, Q6, Q10 (any query using interval arithmetic)

MariaDB's query printer emits `INTERVAL 'N' UNIT` (quoted integer, separate unit
keyword).  DuckDB requires the number and unit in a single string: `INTERVAL '90 days'`.
The bare-integer form `INTERVAL N UNIT` is also rejected by DuckDB.

HolyDuck rewrites `interval 'N' unit[s]` → `interval 'N units'` in the SQL rewrite
pass before pushdown.

---

### `DATE'YYYY-MM-DD'` → `DATE 'YYYY-MM-DD'`

**Affected queries:** Q1, Q3, Q4, Q5, Q6, Q7, Q8, Q10 (ANSI date literals)

MariaDB's query printer emits ANSI date literals without a space between the `DATE`
keyword and the string (e.g. `DATE'1998-12-01'`).  DuckDB requires a space:
`DATE '1998-12-01'`.  Without the space, DuckDB misparsed the expression and
arithmetic on the result failed with a type error.

---

### `<in_optimizer>(val, expr)` → `expr`

**Affected queries:** Q4 and any query using `EXISTS` subqueries

MariaDB's optimizer wraps `IN`/`EXISTS` subqueries in an internal `Item_in_optimizer`
node.  The printed form is `<in_optimizer>(val, expr)`.  DuckDB does not recognise
this syntax.  HolyDuck strips the wrapper and emits just the inner expression.

---

## Query-level notes

### Q4 — `EXISTS (SELECT * ...)` crashes MariaDB

**Status:** Known bug — avoid `SELECT *` inside `EXISTS`

`EXISTS (SELECT * FROM lineitem WHERE ...)` causes a SIGSEGV in MariaDB when the
subquery table is a DuckDB engine table. Use `EXISTS (SELECT 1 FROM ...)` instead.
The semantics are identical per SQL standard.  The TPC-H test for Q4 uses `SELECT 1`.

**Root cause:** Under investigation.

### Q8 — zero results at sf=0.01

The canonical Q8 parameters (BRAZIL, AMERICA, ECONOMY ANODIZED STEEL) return
`mkt_share = 0` at scale factor 0.01.  This is a data sparsity issue, not a bug —
DuckDB produces the same result directly.  The test captures this as the expected
output.

---

## Raw TPC-H query files

The raw query files in `TPC_H_3_0_1/dbgen/queries/` are `qgen` templates, not
runnable SQL.  They contain:

- `:x`, `:o`, `:n N` — directives (skip limit, output format, row limit)
- `':1'`, `':2'`, `':3'` — substitution variables replaced by `qgen` at runtime

The regression test SQL files in `tests/regression/tpch/` are cleaned versions
with canonical parameter values substituted in.
