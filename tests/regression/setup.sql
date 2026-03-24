-- Regression test database setup.
-- Run once before the test suite; teardown.sql drops it.

DROP DATABASE IF EXISTS hd_regression;
CREATE DATABASE hd_regression;
USE hd_regression;

-- InnoDB dimension tables
CREATE TABLE categories (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=InnoDB;
CREATE TABLE regions    (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=InnoDB;

INSERT INTO categories VALUES (1,'Electronics'),(2,'Clothing'),(3,'Food');
INSERT INTO regions    VALUES (1,'North'),(2,'South'),(3,'East');

-- DuckDB fact table
CREATE TABLE sales (
    order_id    INT,
    category_id INT,
    region_id   INT,
    amount      DECIMAL(10,2),
    ts          DATETIME
) ENGINE=DUCKDB;

INSERT INTO sales VALUES (1,1,1,100.00,'2026-01-15 10:00:00');
INSERT INTO sales VALUES (2,1,2,200.00,'2026-02-10 11:00:00');
INSERT INTO sales VALUES (3,2,1,150.00,'2026-02-20 09:00:00');
INSERT INTO sales VALUES (4,2,3,300.00,'2026-03-05 14:00:00');
INSERT INTO sales VALUES (5,3,2,250.00,'2026-03-18 16:00:00');
INSERT INTO sales VALUES (6,1,3,175.00,'2026-01-22 08:00:00');

-- Second DuckDB table for multi-table pushdown tests
CREATE TABLE products (
    id       INT,
    name     VARCHAR(50),
    category_id INT
) ENGINE=DUCKDB;

INSERT INTO products VALUES (1,'Laptop',1),(2,'T-Shirt',2),(3,'Apple',3),(4,'Phone',1);
