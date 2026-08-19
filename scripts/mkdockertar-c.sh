#!/bin/bash
# Build a classic "docker save" tar for scratch + single C binary image.
# Uses Docker buildx for cross-compilation. Requires: docker, tar, sha256sum.
#
# Usage: scripts/mkdockertar-c.sh <GOOS> <GOARCH> <GOARM> <TAG> <OUTPUT>
# Example: scripts/mkdockertar-c.sh linux arm64 "" awg-proxy:arm64 builds/out.tar.gz
set -euo pipefail

GOOS="$1"
GOARCH="$2"
GOARM="$3"
TAG="$4"
OUTPUT="$5"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

BUILD_VERSION="${VERSION:-dev}"

# --- 1. Cross-compile binary via Docker buildx ---
PLATFORM="linux/$GOARCH"
if [ -n "$GOARM" ]; then PLATFORM="$PLATFORM/v$GOARM"; fi

# debian:bookworm-slim no longer lists linux/arm/v5 (see the BASE arg in the
# Dockerfile), so that platform builds from the per-arch repo.
BASE="debian:bookworm-slim"
if [ "$PLATFORM" = "linux/arm/v5" ]; then BASE="arm32v5/debian:bookworm-slim"; fi

docker buildx build --platform "$PLATFORM" \
    --build-arg VERSION="$BUILD_VERSION" \
    --build-arg BASE="$BASE" \
    --output "type=local,dest=$WORK/docker-out" \
    .
cp "$WORK/docker-out/awg-proxy" "$WORK/awg-proxy"

# --- 2. Create layer tar (--mode=0755: NTFS has no execute bit) ---
tar cf "$WORK/layer.tar" --mode=0755 -C "$WORK" awg-proxy
LAYER_SHA=$(sha256sum "$WORK/layer.tar" | cut -d' ' -f1)
LAYER_DIR="$WORK/out/$LAYER_SHA"
mkdir -p "$LAYER_DIR"
mv "$WORK/layer.tar" "$LAYER_DIR/layer.tar"
echo '1.0' > "$LAYER_DIR/VERSION"
printf '{"id":"%s"}' "$LAYER_SHA" > "$LAYER_DIR/json"

# --- 3. Create image config ---
CREATED=$(date -u +"%Y-%m-%dT%H:%M:%SZ" 2>/dev/null || date -u +"%Y-%m-%dT%H:%M:%SZ")
ARCH="$GOARCH"
VARIANT=""
if [ "$GOARCH" = "arm" ] && [ -n "$GOARM" ]; then VARIANT="$GOARM"; fi

CONFIG="{\"architecture\":\"$ARCH\""
if [ -n "$VARIANT" ]; then
    CONFIG="$CONFIG,\"variant\":\"v$VARIANT\""
fi
CONFIG="$CONFIG,\"os\":\"$GOOS\",\"created\":\"$CREATED\""
CONFIG="$CONFIG,\"config\":{\"Entrypoint\":[\"/awg-proxy\"]}"
CONFIG="$CONFIG,\"rootfs\":{\"type\":\"layers\",\"diff_ids\":[\"sha256:$LAYER_SHA\"]}}"
printf '%s' "$CONFIG" > "$WORK/config.json"
CONFIG_SHA=$(sha256sum "$WORK/config.json" | cut -d' ' -f1)
mv "$WORK/config.json" "$WORK/out/${CONFIG_SHA}.json"

# --- 4. Create manifest.json ---
printf '[{"Config":"%s.json","RepoTags":["%s"],"Layers":["%s/layer.tar"]}]' \
    "$CONFIG_SHA" "$TAG" "$LAYER_SHA" > "$WORK/out/manifest.json"

# --- 5. Create repositories ---
REPO="${TAG%%:*}"
RTAG="${TAG#*:}"
printf '{"%s":{"%s":"%s"}}' "$REPO" "$RTAG" "$CONFIG_SHA" > "$WORK/out/repositories"

# --- 6. Pack final tar.gz ---
ABS_OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
(cd "$WORK/out" && tar czf "$ABS_OUTPUT" *)

echo "Created $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
