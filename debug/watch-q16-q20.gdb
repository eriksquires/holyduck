# Watch Q16 / Q20 failures
# Break at extract_missing_table to see the raw DuckDB error message,
# and at ha_duckdb::rnd_init just before Query() to see the SQL.

# --- extract_missing_table ---
break extract_missing_table
commands
  silent
  printf "\n[extract_missing_table] errmsg = %s\n", errmsg._M_dataplus._M_p
  continue
end

# --- ha_duckdb::rnd_init: print sql just before connection->Query(sql) ---
# Line 1686: auto r= connection->Query(sql);
break ha_duckdb::rnd_init
commands
  silent
  printf "\n[rnd_init] duckdb_table_name = %s\n", duckdb_table_name._M_dataplus._M_p
  continue
end

printf "Breakpoints set. Run your query now.\n"
continue
