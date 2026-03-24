#!/bin/bash
# run.sh — Execute HolyDuck regression tests against a running container.
#
# Usage:
#   ./tests/regression/run.sh [--update] [container-name]
#
# --update  Regenerate all .expected files from current output (use when
#           adding new tests or intentionally changing output).
#
# Assumes the container is already running.  Start it with:
#   MARIADB_SRC_DIR=... scripts/docker-run.sh ubuntu

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPDATE=0
CONTAINER="duckdb-plugin-dev-ubuntu"

for arg in "$@"; do
  case "$arg" in
    --update) UPDATE=1 ;;
    *)        CONTAINER="$arg" ;;
  esac
done

MARIADB="docker exec ${CONTAINER} mariadb -uroot -ptestpass --ssl=0 --batch"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GREEN}  PASS${NC}  $*"; }
fail() { echo -e "${RED}  FAIL${NC}  $*"; }
info() { echo -e "${YELLOW}  ----${NC}  $*"; }

# ── Setup ────────────────────────────────────────────────────────────────────
info "Setting up regression database..."
${MARIADB} < "${SCRIPT_DIR}/setup.sql" 2>/dev/null
echo

# ── Run tests ────────────────────────────────────────────────────────────────
PASS=0; FAIL=0; NEW=0

for sql_file in $(ls "${SCRIPT_DIR}"/*.sql | grep -v 'setup.sql\|teardown.sql' | sort); do
  name="$(basename "${sql_file}" .sql)"
  expected_file="${SCRIPT_DIR}/${name}.expected"

  actual=$(${MARIADB} < "${sql_file}" 2>&1)

  if [[ "${UPDATE}" -eq 1 ]]; then
    echo "${actual}" > "${expected_file}"
    info "${name}  (updated)"
    (( NEW++ )) || true
    continue
  fi

  if [[ ! -f "${expected_file}" ]]; then
    echo "${actual}" > "${expected_file}"
    info "${name}  (no .expected found — created from current output)"
    (( NEW++ )) || true
    continue
  fi

  expected=$(cat "${expected_file}")
  if [[ "${actual}" == "${expected}" ]]; then
    pass "${name}"
    (( PASS++ )) || true
  else
    fail "${name}"
    echo "  Expected:"
    echo "${expected}" | sed 's/^/    /'
    echo "  Got:"
    echo "${actual}" | sed 's/^/    /'
    (( FAIL++ )) || true
  fi
done

# ── Teardown ─────────────────────────────────────────────────────────────────
echo
info "Tearing down regression database..."
${MARIADB} < "${SCRIPT_DIR}/teardown.sql" 2>/dev/null

# ── Summary ──────────────────────────────────────────────────────────────────
echo
if [[ "${UPDATE}" -eq 1 ]]; then
  echo -e "${YELLOW}Updated ${NEW} expected file(s).${NC}"
elif [[ "${FAIL}" -eq 0 ]]; then
  echo -e "${GREEN}All tests passed (${PASS} passed, ${NEW} new).${NC}"
  exit 0
else
  echo -e "${RED}${FAIL} test(s) failed, ${PASS} passed, ${NEW} new.${NC}"
  exit 1
fi
