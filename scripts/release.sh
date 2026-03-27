#!/bin/bash
# release.sh — Build ha_duckdb.so for all distros and package release artifacts.
#
# Usage:
#   ./scripts/release.sh <version>
#
# Example:
#   ./scripts/release.sh v0.4.0
#
# Prerequisites:
#   - Base images built:  ./scripts/build-base.sh <distro>  (each distro once)
#   - Containers exist and cmake is configured:
#       MARIADB_SRC_DIR=... ./scripts/docker-run.sh <distro>
#       ./scripts/cmake-setup.sh duckdb-plugin-dev-<distro>
#     Containers do not need to be running — this script starts and stops them.
#
# Output:
#   release/<version>/
#     ha_duckdb-<version>-ubuntu.so
#     ha_duckdb-<version>-oracle8.so
#     ha_duckdb-<version>-oracle9.so
#     ha_duckdb-<version>-debian12.so
#     holyduck_duckdb_extensions.sql
#     holyduck_mariadb_functions.sql
#     ha_duckdb-<version>-ubuntu.tar.gz      (so + sql files)
#     ha_duckdb-<version>-oracle8.tar.gz
#     ha_duckdb-<version>-oracle9.tar.gz
#     ha_duckdb-<version>-debian12.tar.gz

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

# ── Argument ──────────────────────────────────────────────────────────────────
VERSION="${1:-}"
[[ -n "${VERSION}" ]] || die "Usage: $0 <version>   e.g. $0 v0.4.0"

RELEASE_DIR="${PLUGIN_DIR}/release/${VERSION}"
mkdir -p "${RELEASE_DIR}"

info "Building HolyDuck ${VERSION} for all distros"
info "Output: ${RELEASE_DIR}"
echo

# ── Distro config ─────────────────────────────────────────────────────────────
declare -A BUILD_SUBDIR=(
  [ubuntu]="build"
  [oracle8]="build-oracle8"
  [oracle9]="build-oracle9"
  [debian12]="build"
)
DISTROS=(ubuntu oracle8 oracle9)

# ── Build each distro sequentially (one container at a time — shared port 3306)
for DISTRO in "${DISTROS[@]}"; do
  CONTAINER="duckdb-plugin-dev-${DISTRO}"
  BUILD_SUB="${BUILD_SUBDIR[$DISTRO]}"
  echo "━━━  ${DISTRO}  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

  # Check container exists
  docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER}$" \
    || die "Container '${CONTAINER}' does not exist. Run:
    MARIADB_SRC_DIR=... ./scripts/docker-run.sh ${DISTRO}
    ./scripts/cmake-setup.sh ${CONTAINER}"

  # Check cmake has been configured for this distro
  [[ -d "${PLUGIN_DIR}/${BUILD_SUB}" ]] \
    || die "cmake not configured for ${DISTRO}. Run:
    MARIADB_SRC_DIR=... ./scripts/docker-run.sh ${DISTRO}
    ./scripts/cmake-setup.sh ${CONTAINER}"

  # Stop any other running dev container (only one can bind port 3306)
  for running in $(docker ps --format '{{.Names}}' | grep '^duckdb-plugin-dev-' | grep -v "^${CONTAINER}$" || true); do
    info "Stopping ${running} to free port 3306..."
    docker stop "${running}" > /dev/null
  done

  # Start this container if not running
  if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER}$"; then
    info "Starting ${CONTAINER}..."
    docker start "${CONTAINER}" > /dev/null
    # Wait for MariaDB to be ready
    for i in $(seq 1 30); do
      docker exec "${CONTAINER}" mysqladmin -uroot -ptestpass --ssl=0 ping --silent 2>/dev/null \
        && break || true
      sleep 1
      [[ $i -lt 30 ]] || die "MariaDB in ${CONTAINER} did not start within 30s"
    done
  fi
  ok "Container running."

  # Build
  info "Building ha_duckdb (${DISTRO})..."
  BUILD_OUTPUT=$(docker exec "${CONTAINER}" bash -c \
    "make -j\$(nproc) -C /plugin-src/${BUILD_SUB} 2>&1")
  BUILD_EXIT=$?
  echo "${BUILD_OUTPUT}" | grep -E "(error:|Built target|Linking)" || true
  if [[ ${BUILD_EXIT} -ne 0 ]]; then
    echo "${BUILD_OUTPUT}"
    die "Build failed for ${DISTRO} (exit ${BUILD_EXIT})"
  fi

  # Locate ha_duckdb.so inside the container — don't assume plugin dir path
  SO_CONTAINER=$(docker exec "${CONTAINER}" \
    find /usr /plugin-src/"${BUILD_SUB}" -name "ha_duckdb.so" 2>/dev/null | head -1)
  [[ -n "${SO_CONTAINER}" ]] || die "Build succeeded but ha_duckdb.so not found inside ${CONTAINER}"
  ok "Built: ${SO_CONTAINER} ($(docker exec "${CONTAINER}" ls -sh "${SO_CONTAINER}" | awk '{print $1}'))"

  # Extract artifact via docker cp — works regardless of bind-mount layout
  SO_DEST="${RELEASE_DIR}/ha_duckdb-${VERSION}-${DISTRO}.so"
  docker cp "${CONTAINER}:${SO_CONTAINER}" "${SO_DEST}"
  ok "Copied to release dir."

  echo
done

# ── Copy SQL files (distro-independent) ───────────────────────────────────────
info "Copying SQL files..."
cp "${PLUGIN_DIR}/sql/holyduck_duckdb_extensions.sql" "${RELEASE_DIR}/"
cp "${PLUGIN_DIR}/sql/holyduck_mariadb_functions.sql" "${RELEASE_DIR}/"
ok "SQL files copied."
echo

# ── Package per-distro tarballs ───────────────────────────────────────────────
info "Creating tarballs..."
for DISTRO in "${DISTROS[@]}"; do
  TARBALL="${RELEASE_DIR}/ha_duckdb-${VERSION}-${DISTRO}.tar.gz"
  tar -czf "${TARBALL}" \
    -C "${RELEASE_DIR}" \
    "ha_duckdb-${VERSION}-${DISTRO}.so" \
    "holyduck_duckdb_extensions.sql" \
    "holyduck_mariadb_functions.sql"
  ok "${DISTRO}: $(ls -sh "${TARBALL}" | awk '{print $1}')  →  ha_duckdb-${VERSION}-${DISTRO}.tar.gz"
done

echo
ok "Release ${VERSION} complete."
echo
echo "Artifacts:"
ls -lh "${RELEASE_DIR}"
