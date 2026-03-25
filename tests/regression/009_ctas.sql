-- CREATE TABLE ... AS SELECT tests
-- Three variants: DuckDB→DuckDB, DuckDB→InnoDB, InnoDB→DuckDB
USE hd_regression;

-- Case 1: DuckDB source → DuckDB target
CREATE TABLE sales_duck ENGINE=DUCKDB AS SELECT * FROM tpch.lineitem WHERE l_quantity < 2;
SELECT COUNT(*), SUM(l_quantity) FROM sales_duck;
DROP TABLE sales_duck;

-- Case 2: DuckDB source → InnoDB target (default engine)
CREATE TABLE nation_copy AS SELECT * FROM tpch.nation;
SELECT COUNT(*), COUNT(DISTINCT n_regionkey) FROM nation_copy ORDER BY 1;
DROP TABLE nation_copy;

-- Case 3: InnoDB source → DuckDB target
CREATE TABLE nation_duck ENGINE=DUCKDB AS SELECT * FROM nation;
SELECT COUNT(*), COUNT(DISTINCT n_regionkey) FROM nation_duck ORDER BY 1;
DROP TABLE nation_duck;
