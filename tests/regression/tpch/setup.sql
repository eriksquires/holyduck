-- TPC-H regression setup.
-- Idempotent: only generates data if the tpch database does not already exist.
-- Uses DuckDB's built-in tpch extension at sf=0.01 (functional testing only).

CREATE DATABASE IF NOT EXISTS tpch;

-- Only generate data if lineitem is missing (avoids expensive regeneration on re-runs)
SET @tpch_exists = (SELECT COUNT(*) FROM information_schema.TABLES
                    WHERE TABLE_SCHEMA = 'tpch' AND TABLE_NAME = 'lineitem');

SET GLOBAL duckdb_execute_sql = IF(@tpch_exists > 0,
    'SELECT ''tpch data already present''',
    'SET home_directory=''/var/lib/mysql''; INSTALL tpch; LOAD tpch; CREATE SCHEMA IF NOT EXISTS tpch; CALL dbgen(sf=0.01, schema=''tpch'')'
);
