#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MARIADB_SRC_DIR="${PLUGIN_DIR}/../server-mariadb-11.8.3"
DISTRO=${1:-ubuntu}
CONTAINER_NAME="duckdb-plugin-dev-${DISTRO}"

# Host directory for MariaDB data — bind-mounted to avoid Docker overlay
# filesystem overhead (overlay2 costs ~3-5x on write-heavy workloads)
DATA_DIR="${PLUGIN_DIR}/data"
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