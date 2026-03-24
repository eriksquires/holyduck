-- Macro regression: verify every macro in holyduck_duckdb_extensions.sql
-- produces the correct result when pushed to DuckDB.
-- Each SELECT is a PUSHED SELECT against the macro_inputs DuckDB table.
USE hd_regression;

SELECT
    -- General
    ifnull(NULL, 'fallback')            AS ifnull_null,
    ifnull('value', 'fallback')         AS ifnull_notnull,

    -- Date formatting
    date_format(dt, '%Y-%m-%d')         AS date_format_ymd,

    -- Unix epoch
    unix_timestamp('2026-01-01 00:00:00') AS unix_ts,
    DATE(from_unixtime(1735689600))     AS from_unixtime_date,

    -- Date arithmetic
    datediff(dt, dt2)                   AS datediff_days,
    last_day('2026-02-10')              AS last_day_feb,

    -- String functions
    locate('World', str1)               AS locate_pos,
    mid(str2, 2, 3)                     AS mid_bcd,
    char_length(space(5))               AS space_len,
    strcmp('abc', 'abd')                AS strcmp_lt,
    strcmp('abc', 'abc')                AS strcmp_eq,
    strcmp('abd', 'abc')                AS strcmp_gt,
    regexp_substr(str1, '[A-Z][a-z]+')  AS regexp_first_word,
    find_in_set('banana', csv_list)     AS find_banana,
    find_in_set('mango',  csv_list)     AS find_missing,

    -- Time bucketing (10:30:00 rounded to 300s bucket = 10:30:00)
    rounddatetime(dt, 300)              AS round_300s,

    -- Control flow
    if(1 = 1, 'yes', 'no')             AS if_true,
    if(1 = 2, 'yes', 'no')             AS if_false

FROM macro_inputs;
