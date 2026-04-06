# Design: smart foreign table injection

**Date:** 2026-04-06
**Status:** Design / discussion
**Scope:** HolyDuck select_handler, shared learnings for IntensityDB

## Problem

When HolyDuck claims a mixed-engine query via select_handler, it
injects non-DuckDB tables by scanning every row of every column from
the foreign engine into a DuckDB temp table. This is fine for small
dimension tables (nation=25 rows) but breaks down when the foreign
table is large, remote, or both.

Costs of the current approach:
- **Column waste:** all columns are read and appended, even if the
  query only references 2 of 50. For remote engines this is pure wire
  bandwidth waste.
- **Row waste:** the full table is scanned even when a join predicate
  would limit the needed rows to a tiny subset.
- **No engine cooperation:** predicates are evaluated in MariaDB's
  memory after reading the row. The source engine (Snowflake, BigQuery,
  IntensityDB) never gets a chance to filter server-side.

## Design

Two independent improvements, each useful on its own:

### 1. Column masking (projection pushdown)

**Current code:**
```cpp
t->read_set = &t->s->all_set;   // reads ALL columns
// ... appends every column to DuckDB temp table
```

**Change:** Before scanning, determine which columns of the foreign
table the query actually references. Set `read_set` to just those
columns. Create the DuckDB temp table with only those columns. Append
only those columns.

**How to determine needed columns:** Walk the query's Item trees
(SELECT list, JOIN ON, WHERE, GROUP BY, ORDER BY, HAVING) and collect
which `Field` objects belong to the foreign table being injected. Since
HD already has access to the `SELECT_LEX` and all `Item` references,
this is a tree walk — no parsing needed.

Alternatively: since HD sends the original SQL to DuckDB, DuckDB needs
to see the column names it expects. The temp table schema must include
every column the SQL text references for this table. Walking the Items
gives us exactly this set.

**Applies to:** both shared (unfiltered) and per-session (filtered)
injection paths.

**Risk:** Low. Projection pushdown is a pure narrowing — if we miss a
column, DuckDB will error on the query (fail-safe, not fail-silent).
Worst case: fall back to `all_set`.

**Cross-engine benefit:** Remote engines (Snowflake, BigQuery) transmit
less data. Column-store engines (IntensityDB) skip reading unused
column segments entirely.

### 2. Adaptive row injection (IN-list pushdown)

**Concept:** Instead of scanning the entire foreign table, first
execute the DuckDB-local portion of the query to determine which join
key values are actually needed, then inject only matching rows.

**Phases:**

```
Phase 1: stats.records on foreign table          (free — metadata only)
Phase 2: run DuckDB-local side                   (fast — already in DuckDB)
         extract SELECT DISTINCT join_key         
         → IN-list cardinality = K
Phase 3: decide strategy
         if foreign_rows < SMALL_TABLE_THRESHOLD  → full scan (not worth optimizing)
         if K / foreign_rows < RATIO_THRESHOLD    → push WHERE key IN (...)
         else                                     → full scan (IN-list too large)
Phase 4: inject with chosen strategy
```

**Row count sources (all fast/free):**
- InnoDB: `handler::info(HA_STATUS_VARIABLE)` → `stats.records`
- IntensityDB: fast metadata count
- Snowflake: `INFORMATION_SCHEMA.TABLES.ROW_COUNT` (estimated)
- BigQuery: `__TABLES__` metadata
- Any engine: MariaDB's `handler::records()` or `stats.records`

**Pushing the IN-list to the source engine:**
Once we decide to use the IN-list strategy, we need the source engine
to actually filter. Options:

a) **cond_push():** Call `t->file->cond_push(in_list_condition)` before
   `ha_rnd_init`. Engines that support it (InnoDB ICP, remote engines)
   will filter server-side. Engines that don't will ignore it, and our
   existing MariaDB-side push_conds filtering still applies as fallback.

b) **Index scan:** If the join key has an index on the foreign table,
   use `ha_index_read` instead of `ha_rnd_next` to fetch only matching
   rows. More complex but avoids full scan entirely for indexed tables.

c) **MariaDB-side only:** Evaluate the IN-list in our existing
   push_conds loop. Avoids any engine API complexity but still reads
   every row from the source. Only saves the DuckDB Appender cost.

Option (a) is the sweet spot — minimal API surface, engines that
support it benefit, others fall back gracefully.

**Thresholds (initial, tunable):**
```
SMALL_TABLE_THRESHOLD  = 10,000 rows    # below this, just pull everything
RATIO_THRESHOLD        = 0.05           # IN-list < 5% of foreign table → push
MAX_IN_LIST_SIZE       = 100,000        # cap for SQL text size / engine limits
```

### Interaction between the two

Column masking and IN-list pushdown are orthogonal:

| Strategy       | Columns | Rows          |
|----------------|---------|---------------|
| Today          | all     | all           |
| Column masking | needed  | all           |
| IN-list only   | all     | filtered      |
| Both           | needed  | filtered      |

"Both" is the target. Column masking can land first (lower risk, always
a win), IN-list pushdown second (requires the two-phase execution).

## Implementation plan

### Phase A: column masking

1. Write `collect_referenced_fields(SELECT_LEX *sel, TABLE *t)` that
   walks the Item trees and returns a bitmap of which fields of `t`
   are referenced by the query.
2. In `inject_table_into_duckdb()`, call it and set
   `t->read_set` to the result instead of `all_set`.
3. Build the DuckDB CREATE (TEMP) TABLE with only the referenced
   columns.
4. Appender loop iterates over referenced columns only.
5. Fallback: if the walk fails or returns empty, use `all_set`
   (current behavior).

### Phase B: adaptive IN-list injection

1. In `init_scan()`, before injecting foreign tables, identify join
   predicates between DuckDB-local tables and each foreign table.
2. For each foreign table with a join predicate:
   a. Get `stats.records` (free).
   b. If above SMALL_TABLE_THRESHOLD, run a DuckDB query for
      `SELECT DISTINCT join_key FROM local_table [WHERE ...]`.
   c. If result cardinality < RATIO_THRESHOLD * foreign_rows and
      < MAX_IN_LIST_SIZE, build an IN-list Item and add to push_conds
      (or call cond_push on the foreign handler).
3. Proceed with injection using the augmented push_conds.

### Phase C: cond_push integration (optional enhancement)

1. Before `ha_rnd_init`, call `t->file->cond_push()` with whatever
   predicates we've collected (both original WHERE predicates and
   synthesized IN-lists).
2. After `ha_rnd_end`, call `t->file->cond_pop()`.
3. This gives engines like InnoDB and remote connectors a chance to
   filter server-side.

## Cross-engine value

This work directly benefits IntensityDB:
- When IDB claims a query that joins IDB tables with DuckDB tables,
  it faces the same injection problem in reverse.
- The column masking and IN-list logic can be extracted into
  `MariaDB::Dev::Inject` (shared module) once the patterns stabilize.
- Row count APIs are universal — every engine exposes them.
- The decision thresholds can be shared configuration in
  `projects.yaml` or a dedicated `injection.yaml`.

## Open questions

1. **Column walk completeness:** Can we rely on the `SELECT_LEX` Item
   trees having all column references, or do we need to also walk
   `thd->lex->query_tables`? Need to verify with subqueries and CTEs.

2. **Two-phase execution overhead:** Running the DuckDB-local side
   first adds a query execution. For simple queries where the foreign
   table is small, this overhead may exceed the savings. The
   SMALL_TABLE_THRESHOLD guards against this but needs tuning.

3. **IN-list SQL size limits:** Some engines may have SQL text limits.
   MAX_IN_LIST_SIZE should be conservative initially.

4. **Multi-table joins:** If a query joins 3 foreign tables, each
   needs its own analysis. The join order within DuckDB affects which
   join keys are available. Start with the single-foreign-table case.

5. **Caching interaction:** The current injection cache keys on
   (connection, table_name, predicate_hash). IN-list predicates change
   per query, so cached entries won't match. May need a cardinality-
   based cache eviction policy.
