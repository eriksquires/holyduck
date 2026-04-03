# Fix: Stream DuckDB query results instead of materializing

## Problem

During CTAS (CREATE TABLE AS SELECT) from a DuckDB table to another engine,
HolyDuck materializes the **entire result set** in memory before returning
any rows. For SF=10 lineitem (60M rows), this consumes 20+ GB and OOM-kills
the server.

InnoDB doesn't trigger this because its slow `write_row()` provides natural
backpressure. Fast engines (IntensityDB) consume rows instantly, so DuckDB
reads ahead aggressively.

## Root cause

Three code paths use `connection->Query(sql)` which returns a
`MaterializedQueryResult` (all rows in memory):

1. **`ha_duckdb::rnd_init()`** — full table scan (CTAS source reads use this)
2. **`ha_duckdb_select_handler::init_scan()`** — pushed SELECT queries
3. **`ha_duckdb_derived_handler::init_scan()`** — pushed CTE/subquery queries

## Fix

Replace `connection->Query(sql)` with `connection->SendQuery(sql)` which
returns a `StreamQueryResult`. Then fetch `DataChunk` objects on demand
(~2048 rows each) instead of indexing into the full materialized result.

### API change

```cpp
// Before:
auto r = connection->Query(sql);           // MaterializedQueryResult
val = result->GetValue(col, row);          // random access by row index

// After:
auto r = connection->SendQuery(sql);       // StreamQueryResult
auto chunk = result->Fetch();              // DataChunk (~2048 rows)
val = chunk->GetValue(col, chunk_row);     // access within chunk
```

## Changes in progress (partially done)

### ha_duckdb.h

- `ha_duckdb::scan_result` changed from `MaterializedQueryResult*` to `void*`
  (cast to `QueryResult*` in .cc)
- `ha_duckdb::scan_chunk` added as `void*` (cast to `DataChunk*`)
- `ha_duckdb::scan_chunk_row` replaces `scan_row`
- `ha_duckdb::convert_row_from_duckdb()` signature changed to take `void*`
- `ha_duckdb_select_handler`: renamed `result` → `duck_result` (avoids
  shadowing `select_handler::result`), added `duck_chunk`, `chunk_row`
- `ha_duckdb_derived_handler`: same renames

### ha_duckdb.cc — DONE

- `ha_duckdb::rnd_init()` — uses `SendQuery()`, stores as `void*`
- `ha_duckdb::rnd_next()` — chunk-based iteration with Fetch()
- `ha_duckdb::rnd_end()` — deletes chunk and result with casts
- `ha_duckdb::convert_row_from_duckdb()` — takes `void*`, casts to DataChunk*
- Constructor initializer list updated

### ha_duckdb.cc — REMAINING (not yet updated)

All remaining references need mechanical renaming `result` → `duck_result`
and `chunk` → `duck_chunk` with proper void* casts:

1. **Line 952**: `delete scan_result;` in destructor — needs cast
2. **Line 1047**: `delete scan_result; scan_result= nullptr;` — needs cast
3. **select_handler constructor** (lines 3049, 3058): `result(nullptr)` → `duck_result(nullptr)`
4. **select_handler destructor** (line 3091): `delete result;` → cast delete duck_result
5. **select_handler::init_scan()** (line 3351): `result= r.release();` → `duck_result=`
6. **select_handler::next_row()**: all `result->` → cast `duck_result`, `chunk->` → cast `duck_chunk`
7. **select_handler::end_scan()** (line 3495): delete with casts
8. **derived_handler constructor** (line 4037): same renames
9. **derived_handler destructor** (line 4056): cast delete
10. **derived_handler::init_scan()** (lines 4118-4119): same pattern
11. **derived_handler::next_row()**: chunk-based iteration with casts
12. **derived_handler::end_scan()** (line 4222): cast delete

### Pattern for each handler

```cpp
// In next_row():
auto *qr = static_cast<duckdb::QueryResult*>(duck_result);
auto *ck = static_cast<duckdb::DataChunk*>(duck_chunk);

while (!ck || chunk_row >= ck->size()) {
    delete ck;
    auto fetched = qr->Fetch();
    ck = fetched.release();
    duck_chunk = ck;
    if (!ck || ck->size() == 0) { duck_chunk = nullptr; return HA_ERR_END_OF_FILE; }
    chunk_row = 0;
}

// Use ck->GetValue(col, chunk_row) instead of result->GetValue(col, current_row)
// For select_handler, result->types[i] is still needed for type dispatch —
// use static_cast<duckdb::QueryResult*>(duck_result)->types[i]
```

## Testing

After completing the changes:
1. Build via `scripts/deploy.sh duckdb-plugin-dev-ubuntu`
2. Run HolyDuck regression tests
3. Test CTAS: `CREATE TABLE t ENGINE=IntensityDB AS SELECT * FROM tpch_sm.lineitem LIMIT 5000000`
4. Monitor RSS — should plateau at ~4 GB (DuckDB memory_limit) instead of growing linearly
5. Scale to 60M rows
