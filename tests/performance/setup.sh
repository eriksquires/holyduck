#!/bin/bash
# Performance test setup.
# Creates tpch_sm (all DuckDB) and tpch_mm (mixed engine) at the configured SF.
# Always drops and recreates — safe to re-run after changing the scale factor.
# Does NOT touch the functional test database (tpch at SF0.01).
#
# Usage (from the repo root):
#   bash tests/performance/setup.sh [container-name]
#
# To change scale factor: edit SF= below.

set -euo pipefail

SF=10
CONTAINER="${1:-holyduck-dev-ubuntu}"
DUCKDB_FILE="/var/lib/mysql/#duckdb/global.duckdb"
PERF_FILE="/home/shared/duckdb/tpch_perf.duckdb"
MARIADB="docker exec -i ${CONTAINER} mariadb -uroot --ssl=0"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
step() { echo -e "${YELLOW}──${NC} $*"; }
ok()   { echo -e "${GREEN}✓${NC}  $*"; }

# ── 1. Stop MariaDB so we can write to global.duckdb directly ─────────────────
step "Stopping MariaDB..."
docker exec "${CONTAINER}" service mariadb stop
ok "MariaDB stopped"

# ── 2. Generate tpch_sm in global.duckdb via DuckDB CLI ───────────────────────
#    Only tpch_sm is generated here.  tpch_mm fact tables are populated later
#    via duckdb_execute_sql INSERT (after MariaDB creates the empty shells),
#    which keeps the data inside DuckDB without routing rows through the plugin.
step "Generating TPC-H SF=${SF} data in DuckDB (this may take a few minutes)..."
docker exec "${CONTAINER}" duckdb "${DUCKDB_FILE}" <<SQL
SET home_directory='/var/lib/mysql';
INSTALL tpch;
LOAD tpch;
DROP SCHEMA IF EXISTS tpch_sm CASCADE;
CREATE SCHEMA tpch_sm;
CALL dbgen(sf=${SF}, schema='tpch_sm');
SQL
ok "tpch_sm data ready"

# ── 3. Build the standalone benchmark DuckDB file ─────────────────────────────
step "Generating standalone benchmark file at ${PERF_FILE}..."
docker exec "${CONTAINER}" bash -c "mkdir -p $(dirname ${PERF_FILE})"
docker exec "${CONTAINER}" duckdb "${PERF_FILE}" <<SQL
INSTALL tpch;
LOAD tpch;
DROP SCHEMA IF EXISTS tpch_sm CASCADE;
CREATE SCHEMA tpch_sm;
CALL dbgen(sf=${SF}, schema='tpch_sm');
SQL
ok "Standalone DuckDB file ready"

# ── 4. Restart MariaDB ────────────────────────────────────────────────────────
step "Starting MariaDB..."
docker exec "${CONTAINER}" service mariadb start
docker exec "${CONTAINER}" bash -c "while ! mysqladmin ping --silent 2>/dev/null; do sleep 1; done"
ok "MariaDB ready"

# ── 5. Register tpch_sm and create tpch_mm skeleton in MariaDB ───────────────
step "Creating MariaDB databases and tpch_mm InnoDB dimension tables..."
${MARIADB} <<SQL
-- tpch_sm: register as a MariaDB database (DuckDB tables already exist)
CREATE DATABASE IF NOT EXISTS tpch_sm;

-- tpch_mm: MariaDB creates empty DuckDB-engine shells via LIKE, then we
--   populate them from tpch_sm entirely inside DuckDB (step 6 below).
DROP DATABASE IF EXISTS tpch_mm;
CREATE DATABASE tpch_mm;

CREATE TABLE tpch_mm.lineitem  LIKE tpch_sm.lineitem;
CREATE TABLE tpch_mm.orders    LIKE tpch_sm.orders;
CREATE TABLE tpch_mm.partsupp  LIKE tpch_sm.partsupp;
CREATE TABLE tpch_mm.customer  LIKE tpch_sm.customer;

-- InnoDB dimension tables (nation=25, region=5, part=2M, supplier=100K at SF10)
CREATE TABLE tpch_mm.nation   ENGINE=InnoDB AS SELECT * FROM tpch_sm.nation;
CREATE TABLE tpch_mm.region   ENGINE=InnoDB AS SELECT * FROM tpch_sm.region;
CREATE TABLE tpch_mm.part     ENGINE=InnoDB AS SELECT * FROM tpch_sm.part;
CREATE TABLE tpch_mm.supplier ENGINE=InnoDB AS SELECT * FROM tpch_sm.supplier;

ALTER TABLE tpch_mm.nation   ADD PRIMARY KEY (n_nationkey);
ALTER TABLE tpch_mm.region   ADD PRIMARY KEY (r_regionkey);
ALTER TABLE tpch_mm.part     ADD PRIMARY KEY (p_partkey);
ALTER TABLE tpch_mm.supplier ADD PRIMARY KEY (s_suppkey);
SQL
ok "MariaDB skeleton ready"

# ── 6. Populate tpch_mm fact tables inside DuckDB ────────────────────────────
#    INSERT via duckdb_execute_sql stays entirely within DuckDB — no rows
#    are routed through the plugin layer regardless of scale factor.
step "Populating tpch_mm fact tables inside DuckDB..."
for tbl in lineitem orders partsupp customer; do
  ${MARIADB} -e "SET GLOBAL duckdb_execute_sql = 'INSERT INTO tpch_mm.${tbl} SELECT * FROM tpch_sm.${tbl}';"
  ok "  ${tbl}"
done

# ── 7. Verify ─────────────────────────────────────────────────────────────────
step "Verifying row counts..."
for tbl in lineitem orders partsupp customer part supplier nation region; do
  count=$(${MARIADB} -sN -e "SELECT COUNT(*) FROM tpch_mm.${tbl};")
  printf "  %-12s %s\n" "${tbl}" "${count}"
done

echo ""
echo -e "${GREEN}Setup complete: tpch_sm and tpch_mm ready at SF=${SF}.${NC}"
