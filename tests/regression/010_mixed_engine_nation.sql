-- Mixed-engine join: InnoDB nation dimension + DuckDB supplier facts
-- nation lives in InnoDB; supplier is a DuckDB table copied from tpch.
-- Tests that the cross-engine injection path works correctly for a
-- realistic TPC-H-style dimension/fact join.
USE hd_regression;

SELECT n.n_name, COUNT(*) AS num_suppliers, ROUND(AVG(s.s_acctbal), 2) AS avg_acctbal
FROM supplier s
JOIN nation n ON s.s_nationkey = n.n_nationkey
GROUP BY n.n_name
ORDER BY n.n_name;
