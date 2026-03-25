# Test Plans

We currently lack any sort of formal SQL testing for either performance or functional completeness. 

We propose leveraging the TPC-H benchmarks and DuckDB's automatic data generation `CALL dbgen(sf=100)` for these benchmarks. 

## Functional Testing

1. Generate tiny TPC-H tables in Duck. CALL dbgen(sf=0.01);
2. Copy dimension tables to MariaDB via HolyDuck
3. Attempt to execute and fix all SQL queries in TPC-H benchmark. 

## Performance Testing 

We need 3 separate benchmarks:

- MariaDB alone
- DuckDB alone
- HolyDuck (see functional testing) with dimension tables in Maria. 

We want to test how long each query takes but also how much storage is needed for the basic data set. 

It's still very useful to create all tables in DuckDB and then using HolyDuck copying them all to Maria. 

It may be good to have several databases, BENCH_SMALL, BENCH_LARGE to enable functional testing, or maybe 1 DB for pure Maria, 1 for mixed. 

Our goal is to get near pure DuckDB speeds. 

Also worth testing is the DuckDB CPU pragma settings.  We should attempt to optimize them before doing mixed testing. 

`PRAGMA threads=6;`

Very slow on row stores:  Q9, 18 and 21 