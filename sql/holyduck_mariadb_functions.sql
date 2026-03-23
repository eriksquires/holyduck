-- holyduck_mariadb_functions.sql
--
-- MariaDB stored functions that pair with HolyDuck's DuckDB macros.
--
-- These functions serve two purposes:
--   1. Satisfy MariaDB's parser so queries using these function names
--      are accepted and pushed down to DuckDB.
--   2. Provide correct fallback behavior when queries run against
--      non-DuckDB (e.g. InnoDB) tables.
--
-- Install once per MariaDB instance:
--   mariadb -uroot -p < holyduck_mariadb_functions.sql
--
-- The matching DuckDB macros are installed automatically at plugin
-- startup from duckdb_mariadb_compat.sql.
--

-- ---------------------------------------------------------------------------
-- RoundDateTime(dt, bucket_secs)
--
-- Truncates a datetime to the nearest multiple of bucket_secs seconds.
--
-- On DuckDB tables: pushdown fires, DuckDB macro uses time_bucket() (fast).
-- On InnoDB tables: this stored function runs using unix timestamp math.
--
-- Example:
--   SELECT RoundDateTime(ts, 300) FROM my_table;  -- 5-minute buckets
-- ---------------------------------------------------------------------------

CREATE FUNCTION IF NOT EXISTS RoundDateTime(dt DATETIME, bucket_secs INT)
RETURNS DATETIME DETERMINISTIC
RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);
