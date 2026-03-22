#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MARIADB_SRC_DIR="${PLUGIN_DIR}/../server-mariadb-11.8.3"
DISTRO=${1:-ubuntu}
CONTAINER_NAME="duckdb-plugin-dev-${DISTRO}"

echo "Starting MariaDB DuckDB Plugin development environment ($DISTRO)..."

# Check if container already exists
if docker ps -a --format 'table {{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "Starting existing container..."
    docker start "$CONTAINER_NAME"
    docker exec -it "$CONTAINER_NAME" /bin/bash
else
    echo "Creating and starting new container..."
    docker run -it \
        -v "${PLUGIN_DIR}:/plugin-src" \
        -v "${MARIADB_SRC_DIR}:/mariadb-src:ro" \
        -p 3306:3306 \
        --name "$CONTAINER_NAME" \
        "mariadb-duckdb-plugin:${DISTRO}"
fi