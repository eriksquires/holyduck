-- Performance test setup.
-- Creates tpch_sm (all DuckDB, SF1) and tpch_mm (mixed engine, SF1).
-- Safe to re-run: skips data generation if tpch_sm.lineitem already exists.
-- Does NOT touch the functional test database (tpch at SF0.01).
--
-- Run inside the container:
--   mariadb -uroot -ptestpass --ssl=0 < /plugin-src/tests/performance/setup.sql

-- ── 1. Generate SF1 data into DuckDB schema tpch_sm ──────────────────────────

SET @sm_exists = (SELECT COUNT(*) FROM information_schema.TABLES
                  WHERE TABLE_SCHEMA = 'tpch_sm' AND TABLE_NAME = 'lineitem');

SET GLOBAL duckdb_execute_sql = IF(@sm_exists > 0,
    'SELECT ''tpch_sm data already present''',
    'SET home_directory=''/var/lib/mysql''; INSTALL tpch; LOAD tpch; CREATE SCHEMA IF NOT EXISTS tpch_sm; CALL dbgen(sf=1, schema=''tpch_sm'')'
);

SELECT @@GLOBAL.duckdb_last_result AS setup_sm;

-- ── 2. Create tpch_sm as a MariaDB database so queries can USE it ─────────────

CREATE DATABASE IF NOT EXISTS tpch_sm;

-- ── 3. Create tpch_mm database (mixed engine) ────────────────────────────────

CREATE DATABASE IF NOT EXISTS tpch_mm;

-- DuckDB fact tables — CTAS from tpch_sm
CREATE TABLE IF NOT EXISTS tpch_mm.lineitem  ENGINE=DUCKDB AS SELECT * FROM tpch_sm.lineitem;
CREATE TABLE IF NOT EXISTS tpch_mm.orders    ENGINE=DUCKDB AS SELECT * FROM tpch_sm.orders;
CREATE TABLE IF NOT EXISTS tpch_mm.partsupp  ENGINE=DUCKDB AS SELECT * FROM tpch_sm.partsupp;
CREATE TABLE IF NOT EXISTS tpch_mm.customer  ENGINE=DUCKDB AS SELECT * FROM tpch_sm.customer;

-- InnoDB dimension tables with PKs
CREATE TABLE IF NOT EXISTS tpch_mm.nation ENGINE=InnoDB AS SELECT * FROM tpch_sm.nation;
CREATE TABLE IF NOT EXISTS tpch_mm.region ENGINE=InnoDB AS SELECT * FROM tpch_sm.region;
CREATE TABLE IF NOT EXISTS tpch_mm.part   ENGINE=InnoDB AS SELECT * FROM tpch_sm.part;
CREATE TABLE IF NOT EXISTS tpch_mm.supplier ENGINE=InnoDB AS SELECT * FROM tpch_sm.supplier;

-- Add PKs to InnoDB dimension tables (if not already present)
ALTER TABLE tpch_mm.nation   ADD PRIMARY KEY IF NOT EXISTS (n_nationkey);
ALTER TABLE tpch_mm.region   ADD PRIMARY KEY IF NOT EXISTS (r_regionkey);
ALTER TABLE tpch_mm.part     ADD PRIMARY KEY IF NOT EXISTS (p_partkey);
ALTER TABLE tpch_mm.supplier ADD PRIMARY KEY IF NOT EXISTS (s_suppkey);

SELECT 'tpch_sm and tpch_mm ready.' AS status;
