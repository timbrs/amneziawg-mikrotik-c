#!/bin/bash
set -e

cd "$(dirname "$0")"

VERSION=${VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
IMAGE=awg-proxy
DIR=builds

rm -rf "$DIR"
mkdir -p "$DIR"

echo "=== C build — Version: $VERSION ==="
echo ""

# --- Linux binaries ---
echo "--- Linux binaries ---"
for spec in "arm64:linux/arm64" "arm:linux/arm/v7" "armv5:linux/arm/v5" "amd64:linux/amd64"; do
  arch="${spec%%:*}"
  platform="${spec#*:}"
  (
    docker buildx build --platform "$platform" \
      --build-arg VERSION="$VERSION" \
      --output "type=local,dest=$DIR/bin-$arch" .
    mv "$DIR/bin-$arch/awg-proxy" "$DIR/$IMAGE-linux-$arch"
    rm -rf "$DIR/bin-$arch"
  ) &
done
wait
echo "Linux binaries done"
echo ""

# --- OCI images ---
echo "--- OCI images ---"
for spec in "arm64:linux/arm64" "arm:linux/arm/v7" "armv5:linux/arm/v5" "amd64:linux/amd64"; do
  arch="${spec%%:*}"
  platform="${spec#*:}"
  (
    docker buildx build --platform "$platform" \
      --build-arg VERSION="$VERSION" \
      --output "type=oci,dest=$DIR/$IMAGE-$arch.tar" \
      -t "$IMAGE:$VERSION-$arch" .
    gzip -f "$DIR/$IMAGE-$arch.tar"
  ) &
done
wait
echo "OCI images done"
echo ""

# --- Classic Docker (RouterOS 7.20 LT) ---
echo "--- Classic Docker (7.20) ---"
declare -A ARMS=([arm64]="" [arm]="7" [armv5]="5" [amd64]="")
for arch in arm64 arm armv5 amd64; do
  VERSION=$VERSION scripts/mkdockertar-c.sh linux "${arch%v5}" "${ARMS[$arch]}" \
    "$IMAGE:$VERSION-$arch" "$DIR/$IMAGE-$arch-7.20-Docker.tar.gz" &
done
wait
echo "Classic Docker images done"
echo ""

# --- Summary ---
echo "=== All 12 artifacts ==="
ls -lh "$DIR/"
