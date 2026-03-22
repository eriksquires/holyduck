-- duckdb_mariadb_compat.sql
--
-- MariaDB-compatibility macros for the DuckDB storage engine.
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

-- MariaDB: LAST_DAY(date)  →  the last calendar day of that month
CREATE OR REPLACE MACRO last_day(d) AS
    (date_trunc('month', d::DATE) + INTERVAL 1 MONTH - INTERVAL 1 DAY)::DATE;

-- ---------------------------------------------------------------------------
-- Control flow
-- ---------------------------------------------------------------------------

-- MariaDB: IF(condition, true_val, false_val)
CREATE OR REPLACE MACRO if(cond, a, b) AS CASE WHEN cond THEN a ELSE b END;
