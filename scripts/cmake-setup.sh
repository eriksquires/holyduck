#!/bin/bash
# cmake-setup.sh — Build MariaDB from source and configure the plugin cmake
#                  inside a target container.
#
# Usage:
#   ./scripts/cmake-setup.sh [container-name]
#
# On first run: builds MariaDB from source inside the container (~20-30 min).
# Subsequent runs: skips MariaDB build, reconfigures plugin cmake only.
#
# MariaDB build artifacts persist via the bind-mounted source directory,
# so each distro builds once and reuses on subsequent runs.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${1:-duckdb-plugin-dev-ubuntu}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

info "Checking container '${CONTAINER}'..."
docker inspect --format '{{.State.Running}}' "${CONTAINER}" 2>/dev/null \
    | grep -q true || die "Container '${CONTAINER}' is not running."
ok "Container is running."

# ── Detect distro ─────────────────────────────────────────────────────────────
OS_RELEASE=$(docker exec "${CONTAINER}" cat /etc/os-release 2>/dev/null)
if echo "${OS_RELEASE}" | grep -qi "oracle linux 9\|rhel.*9\|centos.*9"; then
    PLUGIN_BUILD_SUBDIR="build-oracle9"
    MARIADB_BUILD_SUBDIR="build-oracle9"
elif echo "${OS_RELEASE}" | grep -qi "oracle\|rhel\|centos"; then
    PLUGIN_BUILD_SUBDIR="build-oracle8"
    MARIADB_BUILD_SUBDIR="build-oracle8"
else
    PLUGIN_BUILD_SUBDIR="build"
    MARIADB_BUILD_SUBDIR="build-ubuntu"
fi

ok "Distro build subdirs: plugin=${PLUGIN_BUILD_SUBDIR}  mariadb=${MARIADB_BUILD_SUBDIR}"

# ── 1. Build MariaDB inside container (one-time per distro) ───────────────────
MARIADB_CONFIG_MARKER="/mariadb-src/${MARIADB_BUILD_SUBDIR}/include/my_config.h"

if docker exec "${CONTAINER}" test -f "${MARIADB_CONFIG_MARKER}" 2>/dev/null; then
    ok "MariaDB already built at /mariadb-src/${MARIADB_BUILD_SUBDIR} — skipping."
else
    info "Building MariaDB from source inside container (~20-30 min, one-time per distro)..."
    docker exec "${CONTAINER}" bash -c "
        set -e
        mkdir -p /mariadb-src/${MARIADB_BUILD_SUBDIR}
        cmake -S /mariadb-src -B /mariadb-src/${MARIADB_BUILD_SUBDIR} \
            -DWITH_UNIT_TESTS=OFF \
            -DPLUGIN_ROCKSDB=NO \
            -DPLUGIN_COLUMNSTORE=NO \
            -DPLUGIN_SPIDER=NO \
            -DPLUGIN_CONNECT=NO \
            -DPLUGIN_MROONGA=NO \
            -DPLUGIN_OQGRAPH=NO \
            -DWITH_EMBEDDED_SERVER=OFF \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            2>&1 | tail -5
        make -j\$(nproc) -C /mariadb-src/${MARIADB_BUILD_SUBDIR} 2>&1 | tail -5
    "
    ok "MariaDB built at /mariadb-src/${MARIADB_BUILD_SUBDIR}"
fi

# ── 2. Configure plugin cmake ─────────────────────────────────────────────────
info "Configuring plugin cmake in /plugin-src/${PLUGIN_BUILD_SUBDIR}..."
docker exec "${CONTAINER}" bash -c "
    mkdir -p /plugin-src/${PLUGIN_BUILD_SUBDIR} && \
    cd /plugin-src/${PLUGIN_BUILD_SUBDIR} && \
    cmake ../src \
        -DMARIADB_SOURCE_DIR=/mariadb-src \
        -DMARIADB_BUILD_DIR=/mariadb-src/${MARIADB_BUILD_SUBDIR} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=/usr/lib/mysql/plugin \
        2>&1
"

ok "cmake configured. Run ./scripts/deploy.sh ${CONTAINER} to build and deploy."
