#!/bin/bash
# build-base.sh — Build a MariaDB DuckDB base Docker image for a target distro.
#
# Usage:
#   ./scripts/build-base.sh <distro>
#
# Supported distros:
#   ubuntu    — Ubuntu 22.04  (docker/base-ubuntu.dockerfile)
#   oracle8   — Oracle Linux 8 / RHEL 8  (docker/base-oracle8.dockerfile)
#
# Output image tag: mariadb-duckdb-base:<distro>

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── Colour helpers ────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓${NC} $*"; }
info() { echo -e "${YELLOW}  →${NC} $*"; }
die()  { echo -e "${RED}  ✗${NC} $*" >&2; exit 1; }

# ── Argument ──────────────────────────────────────────────────────────────────
DISTRO="${1:-}"
if [[ -z "${DISTRO}" ]]; then
    die "Usage: $0 <distro>   (supported: ubuntu, oracle8)"
fi

# ── Distro → Dockerfile mapping ───────────────────────────────────────────────
case "${DISTRO}" in
  ubuntu)  DOCKERFILE="docker/base-ubuntu.dockerfile" ;;
  oracle8)  DOCKERFILE="docker/base-oracle8.dockerfile" ;;
  oracle9)  DOCKERFILE="docker/base-oracle9.dockerfile" ;;
  debian12) DOCKERFILE="docker/base-debian12.dockerfile" ;;
  *)
    die "Unknown distro '${DISTRO}'. Supported: ubuntu, oracle8, oracle9, debian12"
    ;;
esac

IMAGE_TAG="mariadb-duckdb-base:${DISTRO}"

info "Building base image for distro : ${DISTRO}"
info "Dockerfile                      : ${DOCKERFILE}"
info "Image tag                       : ${IMAGE_TAG}"
echo

export DOCKER_BUILDKIT=1

docker build \
    --file="${PLUGIN_DIR}/${DOCKERFILE}" \
    --tag="${IMAGE_TAG}" \
    --progress=plain \
    "${PLUGIN_DIR}"

echo
ok "Base image build complete: ${IMAGE_TAG}"
echo
echo "Next steps:"
echo "  Start a dev container:  ./scripts/docker-run.sh ${DISTRO}"
