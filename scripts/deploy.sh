#!/bin/bash
# deploy.sh — Build ha_duckdb plugin and deploy it into a target container.
#
# Usage:
#   ./scripts/deploy.sh [container-name]
#
# Defaults to container "duckdb-plugin-dev-ubuntu".
# Detects plugin directory and restart method from the target container.
# Handles the full lifecycle: build → copy → restart → wait → install → verify.
#
# Prerequisite: run cmake-setup.sh once per container before first deploy.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${1:-duckdb-plugin-dev-ubuntu}"
MARIADB_OPTS="-uroot -ptestpass --ssl=0"

# ── Colours ──────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

# ── 1. Check container is running ────────────────────────────────────────────
info "Checking container '${CONTAINER}'..."
docker inspect --format '{{.State.Running}}' "${CONTAINER}" 2>/dev/null \
    | grep -q true || die "Container '${CONTAINER}' is not running. Start it first."
ok "Container is running."

# ── 2. Detect distro-specific settings from the container ─────────────────────
info "Detecting container environment..."

# Plugin directory — query MariaDB directly so it works on any distro
PLUGIN_DEST=$(docker exec "${CONTAINER}" mariadb ${MARIADB_OPTS} \
    --skip-column-names -e "SELECT @@plugin_dir;" 2>/dev/null | tr -d '[:space:]')
[[ -n "${PLUGIN_DEST}" ]] || die "Could not determine plugin_dir from container."
ok "Plugin directory: ${PLUGIN_DEST}"

# Build directory — detect from distro version
OS_RELEASE=$(docker exec "${CONTAINER}" cat /etc/os-release 2>/dev/null)
if echo "${OS_RELEASE}" | grep -qi "oracle linux 9\|rhel.*9\|centos.*9"; then
    BUILD_DIR="${PLUGIN_DIR}/build-oracle9"
    RESTART_CMD="mysqladmin -uroot -ptestpass shutdown; sleep 2; mysqld_safe --user=mysql &"
elif echo "${OS_RELEASE}" | grep -qi "oracle\|rhel\|centos"; then
    BUILD_DIR="${PLUGIN_DIR}/build-oracle8"
    RESTART_CMD="mysqladmin -uroot -ptestpass shutdown; sleep 2; mysqld_safe --user=mysql &"
else
    BUILD_DIR="${PLUGIN_DIR}/build"
    RESTART_CMD="service mariadb restart"
fi
ok "Build directory: ${BUILD_DIR}"

# ── 3. Build ──────────────────────────────────────────────────────────────────
info "Building ha_duckdb..."
# All distros build inside the container — cmake was configured with container-internal paths
BUILD_SUBDIR="$(basename "${BUILD_DIR}")"
docker exec "${CONTAINER}" bash -c \
    "make -j\$(nproc) -C /plugin-src/${BUILD_SUBDIR} 2>&1" \
    | grep -E "error:|warning:|Built target|Error" || true
[[ -f "${BUILD_DIR}/libha_duckdb.so" ]] || die "Build failed — libha_duckdb.so not found."
ok "Build complete: $(ls -sh "${BUILD_DIR}/libha_duckdb.so" | awk '{print $1}')"

# ── 4. Copy plugin and companion SQL file ─────────────────────────────────────
info "Copying plugin into container..."
docker cp "${BUILD_DIR}/libha_duckdb.so"            "${CONTAINER}:${PLUGIN_DEST}/ha_duckdb.so"
docker cp "${BUILD_DIR}/duckdb_mariadb_compat.sql"  "${CONTAINER}:${PLUGIN_DEST}/duckdb_mariadb_compat.sql"
ok "Files copied."

# ── 5. Restart MariaDB ────────────────────────────────────────────────────────
info "Restarting MariaDB..."
docker exec "${CONTAINER}" bash -c "${RESTART_CMD}" 2>&1 | grep -v "^$" || true

# ── 6. Wait for MariaDB to be ready ──────────────────────────────────────────
info "Waiting for MariaDB to be ready..."
for i in $(seq 1 30); do
    if docker exec "${CONTAINER}" mysqladmin ${MARIADB_OPTS} ping --silent 2>/dev/null; then
        ok "MariaDB is ready (${i}s)."
        break
    fi
    sleep 1
    if [[ $i -eq 30 ]]; then die "MariaDB did not start within 30 seconds."; fi
done

# ── 7. Ensure plugin is installed ────────────────────────────────────────────
info "Checking plugin registration..."
LOADED=$(docker exec "${CONTAINER}" mariadb ${MARIADB_OPTS} \
    -e "SELECT COUNT(*) FROM information_schema.ENGINES WHERE ENGINE='DUCKDB' AND SUPPORT IN ('YES','DEFAULT');" \
    --skip-column-names 2>/dev/null || echo "0")

if [[ "${LOADED}" == "0" ]]; then
    info "Plugin not registered — installing..."
    docker exec "${CONTAINER}" mariadb ${MARIADB_OPTS} \
        -e "INSTALL PLUGIN duckdb SONAME 'ha_duckdb.so';" 2>/dev/null \
        || info "Install returned an error (may already be registered)."
fi

# ── 8. Verify ─────────────────────────────────────────────────────────────────
RESULT=$(docker exec "${CONTAINER}" mariadb ${MARIADB_OPTS} \
    -e "SELECT ENGINE, SUPPORT, COMMENT FROM information_schema.ENGINES WHERE ENGINE='DUCKDB';" \
    2>/dev/null)

if echo "${RESULT}" | grep -q "DUCKDB"; then
    ok "DuckDB engine is loaded and ready."
    echo
    echo "${RESULT}"
else
    die "DuckDB engine did not load. Check: docker exec ${CONTAINER} tail /var/lib/mysql/*.err"
fi
