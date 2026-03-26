-- Mixed-engine UNION ALL: one arm is DuckDB (products), one is InnoDB (categories).
-- Verifies that InnoDB tables are injected and the UNION executes inside DuckDB.
USE hd_regression;
SELECT 'duck' AS engine, name AS label FROM products WHERE category_id = 1
UNION ALL
SELECT 'innodb', name FROM categories WHERE id <= 2
ORDER BY engine DESC, label;
