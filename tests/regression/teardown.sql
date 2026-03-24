-- Drop DuckDB tables directly via DuckDB (no frm file required), then drop DB.
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."macro_inputs"';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."products"';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."sales"';
DROP DATABASE IF EXISTS hd_regression;
