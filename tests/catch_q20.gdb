set pagination off
set confirm off
handle SIGSEGV stop print nopass

# Catch crashes anywhere in inject_table_into_duckdb
break inject_table_into_duckdb

commands 1
  echo === inject_table_into_duckdb called ===\n
  print duck_name
  print push_conds.size()
  continue
end

catch signal SIGSEGV
commands 2
  echo === SIGSEGV caught ===\n
  backtrace 30
  info locals
  quit
end

continue
