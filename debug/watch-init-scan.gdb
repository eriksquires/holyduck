# watch-init-scan.gdb
# Breaks just before DuckDB executes the final SQL in init_scan and prints:
#   - which path was taken (original SQL vs AST printer)
#   - the exact SQL string sent to DuckDB
#
# Usage (inside container):
#   bash /plugin-src/debug/gdb-attach.sh -x /plugin-src/debug/watch-init-scan.gdb

set pagination off
set print thread-events off

# Break at the connection->Query(sql) call — sql is fully built at this point
break ha_duckdb.cc:2945
commands
  silent
  printf "\n=== init_scan fired (thread %d) ===\n", $_thread
  printf "use_original_sql : %d\n", use_original_sql
  printf "SQL              : %s\n", sql._M_dataplus._M_p
  printf "===\n\n"
  continue
end

# Also break on inject_table_into_duckdb to see when InnoDB tables are injected
break ha_duckdb.cc:3143
commands
  silent
  printf "  >> injecting InnoDB table as: %s\n", temp_name._M_dataplus._M_p
  continue
end

continue
