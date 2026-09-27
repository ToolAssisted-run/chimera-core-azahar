#!/bin/sh
# Guest (waterbox) build: Azahar's own CMake and options, under the miniBox
# musl toolchain. Artifacts land in build/guest. Only the libraries the core
# links are built - Azahar's CMake would otherwise build helper tools too.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" $AZAHAR_OPTS "$root/extern/azahar"
make -C "$root/build/guest" -j"${JOBS:-$(nproc)}" citra_core video_core audio_core "$@"
