#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MARIADB_SRC_DIR="${PLUGIN_DIR}/../server-mariadb-11.8.3"
DISTRO=${1:-ubuntu}

echo "Building MariaDB DuckDB Plugin for $DISTRO..."
echo "Plugin source: $PLUGIN_DIR"
echo "MariaDB source: $MARIADB_SRC_DIR"

# Verify MariaDB source exists
if [ ! -d "$MARIADB_SRC_DIR" ]; then
    echo "Error: MariaDB source not found at $MARIADB_SRC_DIR"
    echo "Please ensure the fresh MariaDB install exists"
    exit 1
fi

# Build the Docker image
echo "Building Docker image..."
docker build \
    -f "${PLUGIN_DIR}/docker/${DISTRO}.dockerfile" \
    -t "mariadb-duckdb-plugin:${DISTRO}" \
    --build-arg MARIADB_SRC="$MARIADB_SRC_DIR" \
    "$PLUGIN_DIR"

echo ""
echo "Build complete! To run development container:"
echo ""
echo "  docker run -it --rm \\"
echo "    -v \"${PLUGIN_DIR}:/plugin-src\" \\"
echo "    -v \"${MARIADB_SRC_DIR}:/mariadb-src\" \\"
echo "    -p 3306:3306 \\"
echo "    --name duckdb-plugin-dev \\"
echo "    mariadb-duckdb-plugin:${DISTRO}"
echo ""
echo "Or use: ./scripts/docker-run.sh $DISTRO"