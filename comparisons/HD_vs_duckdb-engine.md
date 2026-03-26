# Code Comparison: HolyDuck vs duckdb-engine

March 26, 2026

There has been confusion about [HolyDuck](https://github.com/eriksquires/holyduck)’s provenance relative to [duckdb-engine](https://github.com/drrtuy/duckdb-engine/tree/master), also hosted on GitHub and authored by drrtuy. This may be in part due to the creation of the two on GitHub were very close in time. In addition, [Drrtuy has also submitted a pull request (PR) to MariaDB](https://github.com/MariaDB/server/pull/4830) to include his work. We were not aware of either the repository or PR during development and we take no position on either. 

To address a broad, non-specific claim efficiently, we asked ChatGPT to perform a line-by-line inspection of both repositories. The findings are included below. We also wanted to rule out the possibility that the use of AI coding tools such as Claude might have introduced material from another project without our knowledge, so this independent review was worthwhile for that reason as well.

Relatedly, drrtuy has suggested that HolyDuck should acknowledge both his project and AliSQL as predecessors. We do not agree, and we discuss that in detail in [HD_vs_AliSQL](HD_vs_AliSQL.md).

Anyone is free to inspect the code directly, but the major structural differences should already be enough to convince a casual reader that HolyDuck is not a clone of either project. The single most important point is this:

- HolyDuck fully supports cross-engine joins between InnoDB and DuckDB.
- AliSQL and, by extension, duckdb-engine expect all tables to be in DuckDB. 

For anyone familiar with MariaDB storage engines, that point alone should be highly informative. 

**HolyDuck** did not intentionally take code from AliSQL, duckdb-engine, or any other project that is not already attributed. ChatGPT’s inspection also concluded that we did not accidentally incorporate material from duckdb-engine. Where HolyDuck uses external code or libraries, we attempt to provide all necessary attributions and  notices. If we are mistaken, please point to the specific code sections and we will review and correct the issue with due diligence. We are happy to correct the record and/or enhance missing copyright notices where appropriate. 

---

## Summary

A detailed inspection of both code bases shows:

> **There is no evidence of meaningful code reuse between HolyDuck and duckdb-engine.**

The two projects share only standard MariaDB storage engine boilerplate, which is expected for any independent implementation using the same API.

---

## Repositories Examined

- HolyDuck  
  https://github.com/eriksquires/holyduck

- duckdb-engine (drrtuy)  
  https://github.com/drrtuy/duckdb-engine/tree/master

---

## Methodology

The comparison was performed using the following approach:

### 1. Direct C++ Source Inspection
- Reviewed core implementation files:
  - `ha_duckdb.cc`
  - `ha_duckdb.h`
- Examined:
  - handler lifecycle methods (`open`, `close`, `rnd_init`, `rnd_next`, etc.)
  - condition pushdown logic
  - write/update/delete paths
  - connection and session handling

### 2. Architectural Comparison
- Compared:
  - execution model
  - data flow
  - query handling strategy
  - cross-engine behavior

### 3. Structural Pattern Analysis
Looked for:
- identical helper functions
- shared naming conventions beyond API requirements
- similar SQL construction approaches
- reused logic patterns or comments
- identical edge-case handling

---

## Findings

### 1. Expected Similarities (Non-Suspicious)

Both projects implement the MariaDB storage engine API, resulting in unavoidable similarities:

- GPL license headers
- `duckdb_init_func`
- `create_duckdb_handler`
- `get_share()` pattern:
  - `lock_shared_ha_data()`
  - `get_ha_share_ptr()`
  - `set_ha_share_ptr()`
- Standard handler method names:
  - `rnd_init`, `rnd_next`, `write_row`, etc.
- MariaDB debugging macros:
  - `DBUG_ENTER`, `DBUG_RETURN`

These are **required patterns**, not evidence of reuse.

---

### 2. Key Architectural Differences

#### HolyDuck

- Global DuckDB instance registry (one `duckdb::DuckDB` instance per file, ref-counted)
- Per-session persistent DuckDB connections (reused across queries within a MariaDB session)
- Active query tracking (for interruption via `KILL QUERY`)
- InnoDB → DuckDB table injection with per-session caching and row-count invalidation
- Single-table predicate pushdown into InnoDB injection (`collect_single_table_conds`) — reduces injected row count for filtered dimension table scans
- Condition pushdown for single-table DuckDB scans (`cond_push`) — WHERE clause serialised and appended to the pushed-down SELECT
- Column projection optimization — only referenced columns fetched from DuckDB, mapped back to MariaDB fields via `scan_field_map`
- Direct UPDATE/DELETE pushdown with reconstructed SQL
- Cross-engine join execution inside DuckDB

#### duckdb-engine

- Per-thread context model
- Transaction callback integration
- Use of converter classes:
  - DMLConvertor
  - DDLConvertor
- Executes original SQL directly in DuckDB
- No dynamic injection of InnoDB tables
- No cross-engine execution model
- Minimal predicate rewriting

---

### 3. Execution Model Differences

| Feature                     | HolyDuck                 | duckdb-engine            |
| --------------------------- | ------------------------ | ------------------------ |
| Execution control              | DuckDB-centric                      | MariaDB-driven           |
| Predicate handling             | Pushed into injection + single-scan | Passed through           |
| Mixed-engine joins             | Supported                           | Not supported            |
| Data movement                  | Dynamic InnoDB→DuckDB injection     | None                     |
| Injection caching              | Per-session, row-count invalidation | N/A                      |
| UPDATE/DELETE                  | Statement-level pushdown            | Original query execution |

---

### 4. Design Philosophy Divergence

#### duckdb-engine
> Treat DuckDB as a storage engine replacement

- Minimal transformation
- MariaDB remains in control of execution
- No cross-engine orchestration

#### HolyDuck
> Treat DuckDB as the execution engine

- MariaDB used as planner / metadata layer
- Execution shifted into DuckDB
- Cross-engine joins enabled via data injection

---

### 5. AliSQL Lineage (duckdb-engine)

The duckdb-engine repository includes attribution to:

- Alibaba / AliSQL
- MariaDB Foundation contributors

This suggests:
> The implementation follows an existing lineage of storage engine patterns

HolyDuck does not follow this lineage and was developed independently.

---

## Conclusion

- The shared code is limited to **standard MariaDB handler boilerplate**
- The **execution models, architecture, and implementation details differ significantly**
- No evidence of:
  - copied logic
  - shared helper functions
  - structural reuse beyond API requirements

> **HolyDuck and duckdb-engine are independent implementations solving different problems.**

---

## Final Statement

If needed for public clarification:

> “After reviewing both code bases, they share only standard MariaDB storage engine boilerplate. The execution model, data flow, and implementation are materially different, and there is no evidence of meaningful code reuse.”
