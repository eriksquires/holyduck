# Bug: VARCHAR columns use slow Value::ToString() in streaming next_row()

## Problem

During CTAS from DuckDB tables, the select_handler's `next_row()` calls
`duckdb::Value::ToString()` for every VARCHAR value in every row. This
involves a heap allocation + string conversion + free per cell. For lineitem
with 5 VARCHAR columns and 60M rows, that's 300M malloc/free cycles.

GDB stack trace during CTAS shows the hot path is:
```
#0  _int_free (glibc malloc)
#1  __GI___libc_free
#2  duckdb::Value::DefaultCastAs
#3  duckdb::Value::ToString
#4  ha_duckdb_select_handler::next_row at ha_duckdb.cc:3528
```

## Root cause

The streaming fix (0.4.1) correctly changed `Query()` → `SendQuery()` and
added chunk-based iteration. But the per-value extraction in `next_row()`
still uses the `default:` case for VARCHAR columns:

```cpp
// ha_duckdb.cc line 3526-3530
default: {
    // VARCHAR and other types: use string conversion
    std::string s = val.ToString();    // <-- SLOW: malloc + convert + free
    field->store(s.c_str(), s.length(), system_charset_info);
    break;
}
```

This is called via `chunk->GetValue(i, chunk_row)` which returns a
`duckdb::Value`, then `ToString()` converts it to a string for MariaDB.

## Fix

Extract VARCHAR data directly from the chunk's vector without going through
`Value::ToString()`. DuckDB chunks store strings as `string_t` in the vector
data. The direct path would be:

```cpp
case duckdb::LogicalTypeId::VARCHAR: {
    auto &vec = chunk->data[i];
    auto str = duckdb::FlatVector::GetData<duckdb::string_t>(vec)[chunk_row];
    field->store(str.GetData(), str.GetSize(), system_charset_info);
    break;
}
```

This avoids the `Value` intermediate entirely — no heap allocation, no
`ToString()`, direct pointer to the chunk's string data.

Note: need to handle non-flat vectors (dictionary, constant) by calling
`chunk->Flatten()` once per chunk before iterating rows.

## Testing

```sql
-- Time this with old vs new code
CREATE TABLE test_ctas ENGINE=IntensityDB
  AS SELECT * FROM tpch_sm.lineitem;
```

Before fix: CTAS is CPU-bound on `_int_free` / `ToString()` in single thread.
After fix: should be IO-bound (our write path), not CPU-bound on string conversion.

## Affected paths

1. `ha_duckdb_select_handler::next_row()` — line 3526 default case
2. `ha_duckdb_derived_handler::next_row()` — same pattern
3. `ha_duckdb::rnd_next()` → `convert_row_from_duckdb()` — line 1921 default case

All three have the same `ToString()` bottleneck for non-numeric types.
