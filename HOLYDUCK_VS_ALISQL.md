# HolyDuck Plugin vs AliSQL

This document compares HolyDuck with [Alibaba's AliSQL (a MySQL fork)](https://github.com/alibaba/AliSQL). It was written in response to a suggestion that HolyDuck is a clone of the AliSQL implementation.

**HolyDuck is an independent implementation and has no code derived from AliSQL.** They are two independent implementations in a similar problem space — DuckDB as a MySQL/MariaDB storage engine — with fundamentally different architectures and different design goals and no shared code.  

In addition, at almost every important place HolyDuck behaves fundamentally differently and the opportunity to cut/paste code from AliSQL just isn't there.  Not only is the code completely unique but the problems we solve and their solutions are entirely different. 

## TL;DR
The best way I know of to describe how these two implement DuckDB differently is with these two facts: 

**AliSQL** prevents mixed engine table joins.

**HolyDuck** tackles them. 

That really explains everything about these two projects.  From this point on we'll get into the details of  each repo and solution. 

## Code Base

**AliSQL** is a MySQL 8 fork with DuckDB inside it.

**HolyDuck** is a MariaDB 11.8.3 storage engine plugin with a DuckDB back-end.

One big obvious way to see differences is in what is in the public repo of each. As of March 24, 2026: 

|                               | AliSQL            | HolyDuck                           |
| ----------------------------- | ----------------- | ---------------------------------- |
| Base                          | MySQL 8.0.44 fork | MariaDB plugin (standalone)        |
| Source Files to Build         | ~ 49,000          | 2                                  |
| Source code lines             | ~12.4 million     | 3,271                              |
| DuckDB-specific source files  | 35                | 2                                  |
| DuckDB-specific lines of code | 9,244             | 3,271                              |
| DuckDB code as % of total     | < 0.1%            | 100% — it's the whole project      |
| Fork required                 | Yes               | No — drops into unmodified MariaDB |

---

## Fundamentally Different Operation and Goals

**AliSQL's DuckDB integration is HTAP** — Hybrid Transactional/Analytical Processing.
The design goal is to give you real-time analytical reads on your *existing* InnoDB data
without changing your schema. InnoDB remains the source of truth; DuckDB receives a
live mirror via binlog replication. You don't choose which tables are DuckDB — AliSQL
can convert all your InnoDB tables automatically and replay writes into DuckDB at
~300K rows/s. It supports three deployment models: standalone DuckDB instance,
one-click InnoDB-to-DuckDB conversion, and a dedicated DuckDB replica node that
mirrors a standard MySQL primary via binlog.

**HolyDuck is an explicit analytical store.** You decide which tables are `ENGINE=DUCKDB`
and which are `ENGINE=InnoDB`. There is no automatic mirroring or binlog replication.
The value proposition is different: rather than giving you analytics on data you already
have in InnoDB, HolyDuck gives you a high-performance columnar store for data you
*choose* to land there — ETL outputs, aggregated results, large fact tables — while
letting you join freely against InnoDB dimension tables without leaving MariaDB.

In short: AliSQL asks *"how do we make your InnoDB data queryable analytically?"*
HolyDuck asks *"how do we make DuckDB a first-class citizen inside MariaDB?"*
These are different questions with different answers.

---

## Unavoidable Similarities

The following similarities exist because they are mandated by the MySQL/MariaDB
storage engine plugin API. Any two independent implementations would share them:

- **File names** `ha_duckdb.h` / `ha_duckdb.cc` — the `ha_<engine>.{h,cc}` naming convention is required by MariaDB/MySQL for all storage engine handlers.  
- **Class declaration** `class ha_duckdb : public handler` — the base class and name
  are dictated by the plugin API.
- **Standard handler methods** (`rnd_init`, `rnd_next`, `write_row`, `create`,
  `external_lock`, etc.) — every storage engine must implement these interfaces.
- **`handlerton` registration boilerplate** — plugin init/deinit, sysvar registration,
  and engine capability declarations follow a fixed structure.

These are equivalent to two people writing separate "Hello World" programs and both
typing `int main()`. It is not copying — it is the interface.

---

## Architectural Differences

The two implementations differ fundamentally in how they use DuckDB.

### Mixed-Engine Query Handling

As we alluded to at the start, the choice of whether to support mixed-engine joins or not drives most of the feature divergence between the two projects.  How we handle these cases is where all the interesting parts of HolyDuck are.

**HolyDuck** handles mixed DuckDB + InnoDB queries by injecting InnoDB tables into
DuckDB as temporary tables at query time, then pushing the entire query — including
the join, aggregation, and ordering — to DuckDB. Mixed-engine analytical queries
run at full DuckDB speed.

**AliSQL** does not support mixed-engine joins — by design. At startup, a
`convert_all_to_duckdb_at_start` option permanently converts every user table to
DuckDB. InnoDB on the replica holds only system metadata (accounts, configuration).
Since all user data is in DuckDB, mixed joins never arise.

### Query Execution Model

**HolyDuck** uses MariaDB's `select_handler` and `derived_handler` APIs to push
entire SQL statements — including `JOIN`, `GROUP BY`, `ORDER BY`, `HAVING`, window
functions, and CTEs — directly into DuckDB. MariaDB hands off the query; DuckDB
executes it and returns a result set. MariaDB's row-by-row execution engine is
bypassed entirely for pushed queries.

**AliSQL** does not use `select_handler` or `derived_handler` at all. Instead it uses
a full SQL string pass-through: MySQL parses and validates the query, then hands the
entire SQL string directly to DuckDB's native engine via `duckdb_query_and_send()`.
DuckDB does all the execution. This works because on an AliSQL analytical replica,
all user data tables have been converted to DuckDB — InnoDB remains but holds only
system metadata (accounts, configuration), so there is nothing to join across engines.

### Index and Row-Position Operations

**HolyDuck** deliberately hides index scan capability from the MariaDB optimizer.
`index_flags()` returns no read-scan bits; `rnd_pos()` is not implemented. This
forces the optimizer to either push the whole query to DuckDB or do a single batched
full scan — preventing naive row-at-a-time index scan plans that would bypass
DuckDB's vectorised execution.

**AliSQL** fully exposes index scans to MySQL. It implements `index_read_map()`,
`index_next()`, `index_prev()`, `index_first()`, `index_last()`, and `rnd_pos()`.
These are used for single-table row-level access (e.g. DML via binlog replay);
analytical queries go through the SQL pass-through path instead.

### Write Path and Transaction Support

**HolyDuck** acknowledges DuckDB's single-writer limitation and does not implement
distributed transaction support. Writes go directly through DuckDB's `Appender` API.

**AliSQL** implements a full transactional write path including:
- A `DeltaAppender` that buffers INSERT/UPDATE/DELETE operations per transaction
- Full two-phase commit (`prepare` / `commit` / `rollback`)
- Binlog integration and idempotent binlog replay
- Replication support with reported throughput of ~300K rows/s

### SQL Rewriting

**HolyDuck** uses a DuckDB macro system (`holyduck_duckdb_extensions.sql`) to map
MariaDB function names to DuckDB equivalents at the DuckDB level, supplemented by
a SQL rewrite pass in the engine for constructs macros cannot handle (e.g.
`CHAR()`, `octet_length`, bare `JOIN` syntax from MariaDB's query printer).

**AliSQL** uses explicit convertor classes (`DdlConvertor`, `DmlConvertor`) that
translate MySQL DDL and DML SQL strings into DuckDB-compatible SQL before execution.

### Connection and Lifecycle Management

**HolyDuck** uses a registry keyed on the DuckDB database file path, allowing
multiple independent DuckDB databases to coexist (one per MariaDB schema). Each
pushed query opens a fresh connection.

**AliSQL** uses a singleton `DuckdbManager` with a single global DuckDB database
instance and per-thread connections maintained in the MySQL `THD` context
(`thd->get_duckdb_context()`).

---

## Summary Table

| Aspect | HolyDuck | AliSQL |
|---|---|---|
| Query execution | Whole-query pushdown (`select_handler`, `derived_handler`) | Full SQL string pass-through to DuckDB's native engine |
| Mixed-engine joins | Inject InnoDB as DuckDB temp tables; push full query to DuckDB | Not supported — all user tables are DuckDB on analytical replica |
| Index scans | Hidden from optimizer — no `rnd_pos`, no `index_read_map` | Fully exposed; used for row-level DML (binlog replay) |
| Transactions | Not supported (DuckDB limitation) | Full 2PC, binlog, replication |
| Write buffering | Direct DuckDB Appender | DeltaAppender with transaction-aware rollback |
| SQL rewriting | Macro system + targeted rewrite pass | Explicit DDL/DML convertor classes |
| Multiple databases | Yes — registry keyed on file path | No — single global DuckDB instance |
| Primary use case | Analytics pushdown; mixed OLAP/OLTP queries | Full MySQL replacement with columnar storage |

---

## Verdict

The AliSQL and HolyDuck DuckDB plugins share a class name and file names because
the MySQL/MariaDB plugin API requires them. Beyond that, they solve fundamentally
different problems: AliSQL builds a dedicated analytical replica where all user data
is converted to DuckDB and SQL is passed through directly to DuckDB's native engine —
mixed joins are a non-issue because InnoDB user tables don't exist on that node.
HolyDuck lives alongside InnoDB on the same instance, uses MariaDB's `select_handler`
and `derived_handler` APIs for pushdown, and solves the hard problem of mixed-engine
joins by injecting InnoDB tables into DuckDB at query time. A developer building
either from scratch, given the same API constraints, would inevitably produce files
named `ha_duckdb.cc` with a class called `ha_duckdb` — that is not evidence of copying.
