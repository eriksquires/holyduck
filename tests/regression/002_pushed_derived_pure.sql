-- PUSHED DERIVED: CTE referencing only DuckDB tables.
USE hd_regression;
WITH agg AS (
    SELECT category_id, SUM(amount) AS total
    FROM sales
    GROUP BY category_id
)
SELECT category_id, total FROM agg ORDER BY category_id;
