-- INSERT INTO <other-engine> SELECT FROM <duckdb-table>
-- Regression for task_insert_select_cross_engine.md:
-- init_scan previously sent the full INSERT statement to DuckDB, which
-- returned a single-row "Count" result mis-streamed back to MariaDB.
USE hd_regression;

-- Case 1: DUCKDB source -> InnoDB destination, full-row copy
CREATE TABLE sales_inno (
    order_id    INT,
    category_id INT,
    region_id   INT,
    amount      DECIMAL(10,2),
    ts          DATETIME
) ENGINE=InnoDB;
INSERT INTO sales_inno SELECT * FROM sales;
SELECT COUNT(*), SUM(amount) FROM sales_inno;
SELECT order_id, category_id, amount FROM sales_inno ORDER BY order_id;

-- Case 2: DUCKDB source -> InnoDB destination with narrow schema + WHERE
CREATE TABLE sales_small (oid INT, amt DECIMAL(10,2)) ENGINE=InnoDB;
INSERT INTO sales_small (oid, amt)
SELECT order_id, amount FROM sales WHERE amount >= 200;
SELECT oid, amt FROM sales_small ORDER BY oid;

-- Case 3: DUCKDB source -> InnoDB destination with expressions
CREATE TABLE sales_expr (label VARCHAR(50), dbl DECIMAL(10,2)) ENGINE=InnoDB;
INSERT INTO sales_expr (label, dbl)
SELECT CONCAT('order-', order_id), amount * 2 FROM sales ORDER BY order_id;
SELECT label, dbl FROM sales_expr ORDER BY label;

-- Case 4: DUCKDB source -> DUCKDB destination (must not pick up a spurious
-- "Count" row on top of the inserted rows)
CREATE TABLE sales_dup ENGINE=DUCKDB AS SELECT * FROM sales WHERE 1=0;
INSERT INTO sales_dup SELECT * FROM sales WHERE order_id <= 3;
SELECT COUNT(*) FROM sales_dup;
SELECT order_id, amount FROM sales_dup ORDER BY order_id;

DROP TABLE sales_inno;
DROP TABLE sales_small;
DROP TABLE sales_expr;
DROP TABLE sales_dup;
