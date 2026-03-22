#!/bin/bash
set -e

# Configuration
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="~/shared/docker_bk_cache"

echo "Building MariaDB DuckDB Base Image with BuildKit..."
echo "Plugin source: $PLUGIN_DIR"
echo "Cache directory: $CACHE_DIR"

# Enable BuildKit
export DOCKER_BUILDKIT=1

# Build the base image with cache mounts
echo "Building base image..."
docker build \
    --file="${PLUGIN_DIR}/docker/base-ubuntu.dockerfile" \
    --tag="mariadb-duckdb-base:ubuntu" \
    --progress=plain \
    "$PLUGIN_DIR"

echo ""
echo "Base image build complete!"
echo "Image: mariadb-duckdb-base:ubuntu"
echo ""
echo "To test the base image:"
echo "  docker run -it --rm -p 3306:3306 mariadb-duckdb-base:ubuntu"