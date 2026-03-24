-- Cross-engine join: DuckDB fact table + InnoDB dimension table.
-- No full pushdown; MariaDB drives the scan with condition pushdown.
USE hd_regression;
SELECT s.order_id, c.name AS category, s.amount
FROM sales s
JOIN categories c ON s.category_id = c.id
ORDER BY s.order_id;
