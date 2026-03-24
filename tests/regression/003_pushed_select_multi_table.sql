-- PUSHED SELECT: join across two DuckDB tables.
USE hd_regression;
SELECT s.order_id, p.name AS product, s.amount
FROM sales s
JOIN products p ON s.category_id = p.category_id
WHERE p.id = 1
ORDER BY s.order_id;
