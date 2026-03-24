# HolyDuck vs AliSQL DuckDB Plugin

This document compares HolyDuck (`ha_duckdb` for MariaDB) with the DuckDB storage
engine plugin included in Alibaba's AliSQL (a MySQL fork). It was written in response
to a claim that HolyDuck is a clone of the AliSQL implementation.

**Conclusion up front: they are not clones.** They are two independent implementations
of the same concept — DuckDB as a MySQL/MariaDB storage engine — with fundamentally
different architectures and different design goals.

---

## Unavoidable Similarities

The following similarities exist because they are mandated by the MySQL/MariaDB
storage engine plugin API. Any two independent implementations would share them:

- **File names** `ha_duckdb.h` / `ha_duckdb.cc` — the `ha_<engine>.{h,cc}` naming
  convention is required by MariaDB/MySQL for all storage engine handlers.
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

### Query Execution Model

**HolyDuck** uses MariaDB's `select_handler` and `derived_handler` APIs to push
entire SQL statements — including `JOIN`, `GROUP BY`, `ORDER BY`, `HAVING`, window
functions, and CTEs — directly into DuckDB. MariaDB hands off the query; DuckDB
executes it and returns a result set. MariaDB's row-by-row execution engine is
bypassed entirely for pushed queries.

**AliSQL** does not use `select_handler` or `derived_handler` at all. MySQL's
optimizer drives all query execution. DuckDB acts as a storage backend that MySQL
scans row-by-row via `rnd_next()`. DuckDB's analytical capabilities are not
leveraged for query execution — only for storage and retrieval.

### Mixed-Engine Query Handling

**HolyDuck** handles mixed DuckDB + InnoDB queries by injecting InnoDB tables into
DuckDB as temporary tables at query time, then pushing the entire query — including
the join, aggregation, and ordering — to DuckDB. The result is that mixed-engine
analytical queries run at full DuckDB speed.

**AliSQL** handles mixed-engine queries through MySQL's standard join machinery.
MySQL fetches rows from DuckDB tables one at a time and joins them with InnoDB rows
in its own execution engine.

### Index and Row-Position Operations

**HolyDuck** deliberately hides index scan capability from the MariaDB optimizer.
`index_flags()` returns no read-scan bits; `rnd_pos()` is not implemented. This
forces the optimizer to either push the whole query to DuckDB or do a single batched
full scan — preventing naive row-at-a-time index scan plans that would bypass
DuckDB's vectorised execution.

**AliSQL** fully exposes index scans to MySQL. It implements `index_read_map()`,
`index_next()`, `index_prev()`, `index_first()`, `index_last()`, and `rnd_pos()`.
MySQL's optimizer can plan range scans and ref joins against DuckDB tables directly.

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
| Query execution | Whole-query pushdown (`select_handler`, `derived_handler`) | Row-at-a-time; MySQL drives all execution |
| Mixed-engine joins | Inject InnoDB as DuckDB temp tables; push full query to DuckDB | MySQL row-by-row join machinery |
| Index scans | Hidden from optimizer — no `rnd_pos`, no `index_read_map` | Fully exposed; MySQL can plan range/ref joins |
| Transactions | Not supported (DuckDB limitation) | Full 2PC, binlog, replication |
| Write buffering | Direct DuckDB Appender | DeltaAppender with transaction-aware rollback |
| SQL rewriting | Macro system + targeted rewrite pass | Explicit DDL/DML convertor classes |
| Multiple databases | Yes — registry keyed on file path | No — single global DuckDB instance |
| Primary use case | Analytics pushdown; mixed OLAP/OLTP queries | Full MySQL replacement with columnar storage |

---

## On Copyright

AliSQL's DuckDB plugin files carry Alibaba's standard GPL v2 header. Notably,
DuckDB's MIT license copyright notice does not appear in AliSQL's source headers —
the DuckDB library lives in `extra/duckdb/` and carries its own license there.

HolyDuck includes `THIRD_PARTY_NOTICES.md` with DuckDB's MIT license reproduced in
full, as required by the MIT license terms. The source files carry an explicit
pointer to that notice. This is more strictly compliant with DuckDB's MIT license
attribution requirement than AliSQL's approach.

---

## Verdict

The AliSQL and HolyDuck DuckDB plugins share a class name and file names because
the MySQL/MariaDB plugin API requires them. Beyond that, they make opposite
architectural choices: AliSQL builds a complete OLTP engine that uses DuckDB as
a row store with index support and full transactional integrity; HolyDuck builds
an analytics pushdown engine that hands entire queries to DuckDB and stays out of
the way. A developer building either from scratch, given the same API constraints,
would inevitably produce files named `ha_duckdb.cc` with a class called `ha_duckdb`
— that is not evidence of copying.
