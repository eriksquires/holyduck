-- Join type coverage: verify LEFT, RIGHT, CROSS joins work in mixed-engine
-- and pure-DuckDB contexts.  INNER JOIN is already covered by 003 and 004.
USE hd_regression;

-- LEFT JOIN: InnoDB driving, DuckDB on the right.
-- All categories including any with no matching sales (none missing here,
-- but exercises the LEFT JOIN code path and NULL-safe aggregation).
SELECT c.name, COUNT(s.order_id) AS sales_count, COALESCE(SUM(s.amount), 0) AS total
FROM categories c
LEFT JOIN sales s ON c.id = s.category_id
GROUP BY c.name
ORDER BY c.name;

-- LEFT JOIN: DuckDB driving, InnoDB on the right.
SELECT s.order_id, s.amount, c.name AS category
FROM sales s
LEFT JOIN categories c ON s.category_id = c.id
ORDER BY s.order_id;

-- RIGHT JOIN: all categories even if no sales (same semantic as first query,
-- different join direction — exercises RIGHT JOIN pushdown).
SELECT c.name, COUNT(s.order_id) AS sales_count
FROM sales s
RIGHT JOIN categories c ON s.category_id = c.id
GROUP BY c.name
ORDER BY c.name;

-- CROSS JOIN: every product paired with every region (DuckDB x InnoDB).
-- 4 products x 3 regions = 12 rows.
SELECT p.name AS product, r.name AS region
FROM products p
CROSS JOIN regions r
ORDER BY p.name, r.name;

-- LEFT JOIN inside a CTE (exercises fix_bare_joins in the derived handler).
WITH regional_sales AS (
    SELECT r.name AS region, SUM(s.amount) AS total
    FROM regions r
    LEFT JOIN sales s ON r.id = s.region_id
    GROUP BY r.name
)
SELECT region, total
FROM regional_sales
ORDER BY region;
