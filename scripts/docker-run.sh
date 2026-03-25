#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISTRO=${1:-ubuntu}

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
die() { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

# MariaDB 11.8.3 source tree — required for building the plugin inside the container
MARIADB_SRC_DIR="${MARIADB_SRC_DIR:-}"
[[ -n "${MARIADB_SRC_DIR}" ]] || die "MARIADB_SRC_DIR is not set. Export the path to your MariaDB 11.8.3 source tree:
  export MARIADB_SRC_DIR=/path/to/mariadb-11.8.3"
[[ -d "${MARIADB_SRC_DIR}" ]] || die "MARIADB_SRC_DIR does not exist: ${MARIADB_SRC_DIR}"
CONTAINER_NAME="duckdb-plugin-dev-${DISTRO}"

# Host directory for MariaDB data — bind-mounted to avoid Docker overlay
# filesystem overhead (overlay2 costs ~3-5x on write-heavy workloads).
# Each distro gets its own data directory to avoid UID/permission conflicts.
DATA_DIR="${PLUGIN_DIR}/data-${DISTRO}"
mkdir -p "${DATA_DIR}"

# Per-distro plugin output directory — mounted directly onto MariaDB's plugin dir
# so cmake --build is the only step needed; no cp required.
PLUGIN_OUT_DIR="${PLUGIN_DIR}/plugin-out-${DISTRO}"
mkdir -p "${PLUGIN_OUT_DIR}"

# Detect the plugin directory path inside the image (differs by distro).
PLUGIN_SYSTEM_DIR=$(docker run --rm "mariadb-duckdb-base:${DISTRO}" \
    bash -c "mariadbd --verbose --help 2>/dev/null | awk '/^plugin.dir/{print \$2; exit}'" 2>/dev/null \
    || echo "/usr/lib/mysql/plugin")

# Pre-populate with the image's stock plugins if empty (first run only).
# Without this the volume mount would hide InnoDB, Aria, etc.
if [ -z "$(ls -A "${PLUGIN_OUT_DIR}" 2>/dev/null)" ]; then
    echo "Populating ${PLUGIN_OUT_DIR} from image (${PLUGIN_SYSTEM_DIR})..."
    docker run --rm "mariadb-duckdb-base:${DISTRO}" \
        tar -C "${PLUGIN_SYSTEM_DIR}" -cf - . \
        | tar -C "${PLUGIN_OUT_DIR}" -xf -
    echo "Done."
fi

echo "Starting MariaDB DuckDB Plugin development environment ($DISTRO)..."

# Only one duckdb-plugin-dev-* container runs at a time (port 3306 is shared).
# Stop any other running dev container before starting the requested one.
for running in $(docker ps --format '{{.Names}}' | grep '^duckdb-plugin-dev-' | grep -v "^${CONTAINER_NAME}$"); do
    echo "Stopping ${running} to free port 3306..."
    docker stop "${running}" > /dev/null
done

# Check if container already exists
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "Starting existing container..."
    docker start "$CONTAINER_NAME"
else
    echo "Creating and starting new container..."
    docker run -d \
        -v "${PLUGIN_DIR}:/plugin-src" \
        -v "${MARIADB_SRC_DIR}:/mariadb-src:ro" \
        -v "${DATA_DIR}:/var/lib/mysql" \
        -v "${PLUGIN_OUT_DIR}:${PLUGIN_SYSTEM_DIR}" \
        -p 3306:3306 \
        --cap-add=SYS_PTRACE \
        --name "$CONTAINER_NAME" \
        "mariadb-duckdb-base:${DISTRO}" \
        bash -c "/usr/local/bin/start-mariadb.sh && sleep infinity"
fi

echo "Container ${CONTAINER_NAME} is running. Connect with:"
echo "  docker exec -it ${CONTAINER_NAME} mariadb -u root -ptestpass"