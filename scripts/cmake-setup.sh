#!/bin/bash
# cmake-setup.sh — Configure the cmake build directory inside a container.
#
# Usage:
#   ./scripts/cmake-setup.sh [container-name]
#
# Defaults to "duckdb-plugin-dev-oracle8".
# Run this once after creating a new container before using deploy.sh.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${1:-duckdb-plugin-dev-oracle8}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

info "Checking container '${CONTAINER}'..."
docker inspect --format '{{.State.Running}}' "${CONTAINER}" 2>/dev/null \
    | grep -q true || die "Container '${CONTAINER}' is not running."

# Detect build dir name from distro
OS_RELEASE=$(docker exec "${CONTAINER}" cat /etc/os-release 2>/dev/null)
if echo "${OS_RELEASE}" | grep -qi "oracle linux 9\|rhel.*9\|centos.*9"; then
    BUILD_SUBDIR="build-oracle9"
elif echo "${OS_RELEASE}" | grep -qi "oracle\|rhel\|centos"; then
    BUILD_SUBDIR="build-oracle8"
else
    BUILD_SUBDIR="build"
fi

info "Configuring cmake in /plugin-src/${BUILD_SUBDIR}..."
docker exec "${CONTAINER}" bash -c "
    mkdir -p /plugin-src/${BUILD_SUBDIR} && \
    cd /plugin-src/${BUILD_SUBDIR} && \
    cmake ../src \
        -DMARIADB_SOURCE_DIR=/mariadb-src \
        -DMARIADB_BUILD_DIR=/mariadb-src/build \
        2>&1
"

ok "cmake configured. Run ./scripts/deploy.sh ${CONTAINER} to build and deploy."
