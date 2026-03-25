-- Regression test database setup.
-- Run once before the test suite; teardown.sql drops it.
--
-- DuckDB tables live in global.duckdb and survive DROP DATABASE.
-- DROP TABLE IF EXISTS only works when a MariaDB frm file is present, so
-- we drop them directly via DuckDB to handle stale state from prior runs.
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."macro_inputs"';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."products"';
SET GLOBAL duckdb_execute_sql = 'DROP TABLE IF EXISTS "hd_regression"."sales"';
DROP DATABASE IF EXISTS hd_regression;  -- InnoDB tables (nation, region, part, supplier) dropped here
FLUSH TABLES;

CREATE DATABASE hd_regression;
USE hd_regression;

-- HolyDuck MariaDB-side stored functions (needed for parser acceptance + InnoDB fallback)
CREATE FUNCTION IF NOT EXISTS RoundDateTime(dt DATETIME, bucket_secs INT)
RETURNS DATETIME DETERMINISTIC
RETURN FROM_UNIXTIME(FLOOR(UNIX_TIMESTAMP(dt) / bucket_secs) * bucket_secs);

-- InnoDB dimension tables
CREATE TABLE categories (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=InnoDB;
CREATE TABLE regions    (id INT PRIMARY KEY, name VARCHAR(50)) ENGINE=InnoDB;

-- InnoDB copies of TPC-H dimension tables (for mixed-engine join tests)
CREATE TABLE nation    AS SELECT * FROM tpch.nation;
CREATE TABLE region    AS SELECT * FROM tpch.region;
CREATE TABLE part      AS SELECT * FROM tpch.part;
CREATE TABLE supplier  AS SELECT * FROM tpch.supplier;

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

-- Single-row DuckDB table for macro regression tests.
-- Values chosen to give deterministic, easy-to-verify outputs.
CREATE TABLE macro_inputs (
    dt        DATETIME,
    dt2       DATETIME,
    str1      VARCHAR(50),
    str2      VARCHAR(50),
    csv_list  VARCHAR(50),
    n         INT
) ENGINE=DUCKDB;

INSERT INTO macro_inputs VALUES (
    '2026-03-15 10:30:00',  -- dt:  reference datetime
    '2026-01-01 00:00:00',  -- dt2: for datediff (73 days before dt)
    'Hello World',          -- str1
    'abcdef',               -- str2
    'apple,banana,cherry',  -- csv_list
    300                     -- n: bucket size in seconds / space count
);
