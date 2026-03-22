#!/bin/bash
# fetch-deps.sh — Download build dependencies for ha_duckdb.
#
# Downloads:
#   1. MariaDB source tree (cloned, no build — build happens inside container)
#   2. DuckDB libduckdb.so + headers
#
# Versions are configurable via environment variables:
#   MARIADB_VERSION=11.8.3          (default)
#   DUCKDB_VERSION=v1.5.0           (default)
#   MARIADB_SRC_DIR=/path/to/clone  (default: ../mariadb-<version>)

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MARIADB_VERSION="${MARIADB_VERSION:-11.8.3}"
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.0}"
MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-$(dirname "${PLUGIN_DIR}")/mariadb-${MARIADB_VERSION}}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

# ── 1. MariaDB source ─────────────────────────────────────────────────────────
if [[ -d "${MARIADB_SRC_DIR}/sql" ]]; then
    ok "MariaDB ${MARIADB_VERSION} source already present at ${MARIADB_SRC_DIR}"
else
    info "Cloning MariaDB ${MARIADB_VERSION} source (this may take a few minutes)..."
    mkdir -p "$(dirname "${MARIADB_SRC_DIR}")"
    git clone \
        --branch "mariadb-${MARIADB_VERSION}" \
        --depth=1 \
        https://github.com/MariaDB/server.git \
        "${MARIADB_SRC_DIR}"
    ok "MariaDB ${MARIADB_VERSION} cloned to ${MARIADB_SRC_DIR}"
fi

# ── 2. DuckDB libduckdb.so + headers ─────────────────────────────────────────
LIB_DIR="${PLUGIN_DIR}/lib"
mkdir -p "${LIB_DIR}"

if [[ -f "${LIB_DIR}/libduckdb.so" ]]; then
    ok "libduckdb.so already present in lib/"
else
    info "Downloading DuckDB ${DUCKDB_VERSION} libduckdb.so..."
    TMP=$(mktemp -d)
    trap 'rm -rf "${TMP}"' EXIT
    curl -fsSL \
        "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/libduckdb-linux-amd64.zip" \
        -o "${TMP}/libduckdb.zip"
    unzip -q "${TMP}/libduckdb.zip" "libduckdb.so" "duckdb.hpp" "duckdb.h" -d "${LIB_DIR}"
    ok "DuckDB ${DUCKDB_VERSION} installed to lib/"
fi

echo
ok "Dependencies ready."
echo
echo "  export MARIADB_SRC_DIR=\"${MARIADB_SRC_DIR}\""
echo
echo "Next steps:"
echo "  1. ./scripts/build-base.sh <distro>      # ubuntu | oracle8 | oracle9"
echo "  2. ./scripts/docker-run.sh <distro>"
echo "  3. ./scripts/cmake-setup.sh <container>  # builds MariaDB + configures plugin"
echo "  4. ./scripts/deploy.sh <container>"
