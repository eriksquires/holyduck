-- PUSHED SELECT: pure DuckDB query pushed to DuckDB entirely.
USE hd_regression;
SELECT order_id, amount FROM sales ORDER BY order_id;
