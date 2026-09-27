#!/bin/sh
# Native reference build: Azahar's own CMake, host toolchain, the same option
# set as the guest. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -B "$root/build/native" $AZAHAR_OPTS "$root/extern/azahar"
make -C "$root/build/native" -j"${JOBS:-$(nproc)}" citra_core video_core audio_core "$@"
