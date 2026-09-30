#!/bin/sh
# Build a self-contained Uno Q test executable without a Yocto SDK/image build.
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../meta-project/recipes-core/matrix-console/files" && pwd)
output_dir=${1:-/tmp/matrix-framebuffer-build}
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
docker run --rm -v "$source_dir:/src:ro" -v "$output_dir:/out" \
    debian:trixie-slim sh -c '
        apt-get update -qq &&
        apt-get install -y -qq --no-install-recommends \
            gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user >/out/compiler-install.log 2>&1 &&
        aarch64-linux-gnu-gcc -std=c11 -O2 -static -Wall -Wextra -Werror \
            /src/matrix-render.c -o /out/matrix-render-aarch64 &&
        chmod 755 /out/matrix-render-aarch64 &&
        qemu-aarch64 /out/matrix-render-aarch64 --help
    '
echo "Static ARM64 executable: $output_dir/matrix-render-aarch64"
