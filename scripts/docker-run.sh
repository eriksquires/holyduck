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

echo "Starting MariaDB DuckDB Plugin development environment ($DISTRO)..."

# Check if container already exists
if docker ps -a --format 'table {{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "Starting existing container..."
    docker start "$CONTAINER_NAME"
else
    echo "Creating and starting new container..."
    docker run -d \
        -v "${PLUGIN_DIR}:/plugin-src" \
        -v "${MARIADB_SRC_DIR}:/mariadb-src:ro" \
        -v "${DATA_DIR}:/var/lib/mysql" \
        -p 3306:3306 \
        --name "$CONTAINER_NAME" \
        "mariadb-duckdb-base:${DISTRO}" \
        bash -c "/usr/local/bin/start-mariadb.sh && sleep infinity"
fi

echo "Container ${CONTAINER_NAME} is running. Connect with:"
echo "  docker exec -it ${CONTAINER_NAME} mariadb -u root -ptestpass"