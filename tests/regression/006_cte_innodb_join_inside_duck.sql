-- Regression: InnoDB dimension tables joined directly inside a DuckDB CTE.
-- Full aggregation including the InnoDB joins runs inside DuckDB.
USE hd_regression;
WITH summary AS (
    SELECT c.name AS category, r.name AS region,
           SUM(s.amount) AS total_sales, COUNT(*) AS orders
    FROM sales s
    JOIN categories c ON s.category_id = c.id
    JOIN regions r ON s.region_id = r.id
    GROUP BY c.name, r.name
)
SELECT * FROM summary ORDER BY total_sales DESC, category, region;
