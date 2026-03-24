-- Regression: MariaDB CTE passed into DuckDB CTE body.
-- Previously caused "Table does not exist" / JOIN syntax error in DuckDB.
USE hd_regression;
WITH
  cats AS (SELECT id, name FROM categories),
  summary AS (
      SELECT s.order_id, cats.name AS category, s.amount
      FROM sales s
      JOIN cats ON s.category_id = cats.id
  )
SELECT * FROM summary ORDER BY order_id;
