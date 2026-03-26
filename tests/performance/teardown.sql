-- Performance test teardown.
-- Drops tpch_sm and tpch_mm. Does NOT touch the functional tpch database.

SET GLOBAL duckdb_execute_sql = 'DROP SCHEMA IF EXISTS tpch_sm CASCADE';
DROP DATABASE IF EXISTS tpch_sm;

SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS tpch_mm.lineitem';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS tpch_mm.orders';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS tpch_mm.partsupp';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS tpch_mm.customer';
DROP DATABASE IF EXISTS tpch_mm;

SELECT 'tpch_sm and tpch_mm dropped.' AS status;
