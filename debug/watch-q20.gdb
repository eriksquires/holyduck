# Watch Q20 rnd_init failure
# Break at ha_duckdb::rnd_init just before Query() and print the full SQL

break ha_duckdb::rnd_init
commands
  silent
  printf "\n[rnd_init] table=%s pushed_where=%s\n", \
    duckdb_table_name._M_dataplus._M_p, \
    pushed_where._M_dataplus._M_p
  continue
end

printf "Breakpoints set. Run Q20 now.\n"
continue
