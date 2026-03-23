-- holyduck_duckdb_extensions.sql
--
-- DuckDB extensions and MariaDB-compatibility macros for the HolyDuck storage engine.
-- Installed into global.duckdb at plugin startup via CREATE OR REPLACE MACRO,
-- so pushed-down queries using MariaDB function names work transparently.
--
-- This file is a deployment artifact: edit and restart MariaDB to update
-- macros without recompiling ha_duckdb.so.
--
-- Rules:
--   * One statement per entry, terminated by ';'
--   * Single-line comments (--) are supported
--   * Blank lines are ignored

-- ---------------------------------------------------------------------------
-- General SQL language
-- ---------------------------------------------------------------------------

CREATE OR REPLACE MACRO ifnull(a, b) AS coalesce(a, b);
-- ---------------------------------------------------------------------------
-- Date/time formatting
-- ---------------------------------------------------------------------------

-- MariaDB: DATE_FORMAT(date, format)
-- DuckDB:  strftime(format, timestamp)  — note reversed argument order
CREATE OR REPLACE MACRO date_format(d, fmt) AS strftime(fmt, d::TIMESTAMP);

-- ---------------------------------------------------------------------------
-- Unix epoch conversions
-- ---------------------------------------------------------------------------

-- MariaDB: UNIX_TIMESTAMP(datetime)  →  integer seconds since Unix epoch
-- epoch() is a DuckDB built-in; safe inside macro bodies even though
-- MariaDB would reject it in direct SQL against a non-DuckDB table.
-- Note: do NOT add a 0-arg unix_timestamp() here — DuckDB v1.0 has no
-- macro arity overloading, so a 0-arg macro would shadow this 1-arg one.
-- MariaDB evaluates UNIX_TIMESTAMP() with no args as a server-side constant
-- before pushdown anyway.
CREATE OR REPLACE MACRO unix_timestamp(d) AS epoch(d::TIMESTAMP)::BIGINT;

-- MariaDB: FROM_UNIXTIME(n)  →  DATETIME from integer epoch seconds
-- make_timestamp() takes microseconds, so multiply seconds by 1_000_000.
CREATE OR REPLACE MACRO from_unixtime(n) AS make_timestamp(n::BIGINT * 1000000);

-- ---------------------------------------------------------------------------
-- Date arithmetic
-- ---------------------------------------------------------------------------

-- MariaDB: DATEDIFF(date1, date2)  →  days between date1 and date2
-- DuckDB's datediff() takes an explicit unit and reversed arg order.
CREATE OR REPLACE MACRO datediff(d1, d2) AS datediff('day', d2::DATE, d1::DATE);

-- MariaDB: LAST_DAY(date)  →  the last calendar day of that month
CREATE OR REPLACE MACRO last_day(d) AS
    (date_trunc('month', d::DATE) + INTERVAL 1 MONTH - INTERVAL 1 DAY)::DATE;

-- ---------------------------------------------------------------------------
-- String functions
-- ---------------------------------------------------------------------------

-- MariaDB: LOCATE(needle, haystack)  →  position of needle in haystack
-- DuckDB instr() has the same semantics but reversed argument order.
-- Note: the 3-arg form LOCATE(needle, haystack, start_pos) is not handled
-- here because DuckDB v1.0 has no macro arity overloading — a 3-arg macro
-- would shadow this 2-arg one.
CREATE OR REPLACE MACRO locate(needle, haystack) AS instr(haystack, needle);

-- MariaDB: MID(str, pos, len)  →  alias for SUBSTRING(str, pos, len)
CREATE OR REPLACE MACRO mid(s, pos, len) AS substring(s, pos, len);

-- MariaDB: SPACE(n)  →  string of n space characters
CREATE OR REPLACE MACRO space(n) AS repeat(' ', n);

-- MariaDB: STRCMP(a, b)  →  -1 / 0 / 1
CREATE OR REPLACE MACRO strcmp(a, b) AS
    CASE WHEN a < b THEN -1 WHEN a > b THEN 1 ELSE 0 END;

-- MariaDB: REGEXP_SUBSTR(str, pattern)  →  first match of pattern in str
CREATE OR REPLACE MACRO regexp_substr(s, pat) AS regexp_extract(s, pat);

-- MariaDB: FIND_IN_SET(needle, csv_list)  →  1-based position in comma-separated list
-- Returns 0 if not found (matching MariaDB behaviour).
CREATE OR REPLACE MACRO find_in_set(needle, lst) AS
    COALESCE(list_position(string_split(lst, ','), needle), 0);

-- MariaDB: CHAR(n)  →  chr(n)
-- 'char' is a reserved type keyword in DuckDB so a macro cannot be used.
-- Handled by the rewrite pass in ha_duckdb.cc instead.

-- ---------------------------------------------------------------------------
-- Time bucketing
-- ---------------------------------------------------------------------------

-- RoundDateTime(datetime, bucket_size_in_seconds)
-- Truncates a timestamp to the nearest multiple of bucket_size seconds.
-- Wraps DuckDB's native time_bucket() function.
--
-- This is an example of the dual implementation pattern:
--   1. A DuckDB macro implements the real logic using time_bucket() (runs when pushed down).
--   2. A MariaDB stored function provides the same semantics using unix timestamp math,
--      so RoundDateTime works on ANY table — DuckDB or InnoDB.
--
-- On DuckDB tables: PUSHED SELECT fires and the DuckDB macro runs (vectorised, fast).
-- On InnoDB tables: MariaDB calls the stored function directly (correct result, portable math).
--
-- To install the MariaDB stored function, run the block below once in MariaDB:
--
--   CREATE FUNCTION RoundDateTime(dt DATETIME, bucket_secs INT)
--   RETURNS DATETIME DETERMINISTIC
--   RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);
--
-- The DuckDB macro below is installed automatically at plugin startup:
CREATE OR REPLACE MACRO rounddatetime(dt, bucket_secs) AS
    time_bucket(to_seconds(CAST(bucket_secs AS BIGINT)), dt::TIMESTAMP);

-- ---------------------------------------------------------------------------
-- Control flow
-- ---------------------------------------------------------------------------

-- MariaDB: IF(condition, true_val, false_val)
CREATE OR REPLACE MACRO if(cond, a, b) AS CASE WHEN cond THEN a ELSE b END;
