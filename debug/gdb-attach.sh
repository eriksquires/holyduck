#!/bin/bash
# Attach GDB to the running MariaDB process and load HolyDuck plugin symbols.
# Run this INSIDE the container: bash /plugin-src/debug/gdb-attach.sh
#
# Usage:
#   bash gdb-attach.sh              # interactive session
#   bash gdb-attach.sh -x cmd.gdb  # run a GDB script

set -e

SO=/usr/lib/mysql/plugin/ha_duckdb.so
MARIADB_PID=$(pgrep -x mariadbd | head -1)

if [ -z "$MARIADB_PID" ]; then
  echo "No mariadbd process found." >&2
  exit 1
fi

# Find the load address of ha_duckdb.so in the running process
LOAD_ADDR=$(grep ha_duckdb /proc/$MARIADB_PID/maps 2>/dev/null \
  | grep -m1 'r-xp\|r--p' | awk '{print "0x"$1}' | cut -d- -f1)

if [ -z "$LOAD_ADDR" ]; then
  echo "ha_duckdb.so not found in /proc/$MARIADB_PID/maps" >&2
  echo "Is the plugin loaded? Try: SHOW PLUGINS;" >&2
  exit 1
fi

echo "Attaching to mariadbd PID=$MARIADB_PID, ha_duckdb.so @ $LOAD_ADDR"
echo "Useful breakpoints:"
echo "  break duckdb_discover_table"
echo "  break ha_duckdb_select_handler::init_scan"
echo "  break ha_duckdb::write_row"
echo ""

gdb -q \
  -ex "set confirm off" \
  -ex "attach $MARIADB_PID" \
  -ex "add-symbol-file $SO $LOAD_ADDR" \
  -ex "set print pretty on" \
  -ex "set print object on" \
  "$@"
