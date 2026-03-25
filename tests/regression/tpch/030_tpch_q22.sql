-- TPC-H Query 22 — Global Sales Opportunity
-- NOTE: Rewritten to avoid outer derived table (workaround for derived_handler
-- emitting <materialize> bug). Original wraps in FROM (...) AS custsale.
USE tpch;
select
    substring(c_phone from 1 for 2) as cntrycode,
    count(*) as numcust,
    sum(c_acctbal) as totacctbal
from customer
where substring(c_phone from 1 for 2) in ('13','31','23','29','30','18','17')
  and c_acctbal > (
      select avg(c_acctbal) from customer
      where c_acctbal > 0.00
        and substring(c_phone from 1 for 2) in ('13','31','23','29','30','18','17')
  )
  and not exists (
      select * from orders where o_custkey = c_custkey
  )
group by cntrycode
order by cntrycode;
