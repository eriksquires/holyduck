-- Pure DuckDB UNION ALL: two arms both reference the same DuckDB fact table.
-- Verifies that create_duckdb_unit_handler intercepts the UNION and executes
-- it entirely inside DuckDB via the original SQL path.
USE hd_regression;
SELECT order_id FROM sales WHERE region_id = 1
UNION ALL
SELECT order_id FROM sales WHERE region_id = 2
ORDER BY order_id;
