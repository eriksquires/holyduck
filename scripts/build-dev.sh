#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Building MariaDB DuckDB Development Image with BuildKit..."
echo "Plugin source: $PLUGIN_DIR"

# Enable BuildKit
export DOCKER_BUILDKIT=1

# Build the development image
echo "Building development image..."
docker build \
    --file="${PLUGIN_DIR}/docker/dev-ubuntu.dockerfile" \
    --tag="mariadb-duckdb-plugin:ubuntu" \
    --progress=plain \
    "$PLUGIN_DIR"

echo ""
echo "Development image build complete!"
echo "Image: mariadb-duckdb-plugin:ubuntu"
echo ""
echo "To test the development image:"
echo "  docker run -it --rm -p 3306:3306 mariadb-duckdb-plugin:ubuntu"