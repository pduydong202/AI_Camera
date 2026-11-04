#!/usr/bin/env bash
set -euo pipefail

source_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
build_dir="$source_dir/build"
cache_file="$build_dir/CMakeCache.txt"

# Keep cache replacement and the build in the same lock.
exec {build_lock}<"$source_dir"
flock "$build_lock"

if [[ -f "$cache_file" ]]; then
    cached_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache_file")
    cached_build=$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache_file")
    if [[ "$cached_source" != "$source_dir" || "$cached_build" != "$build_dir" ]]; then
        backup_dir=$(mktemp -d -- "$source_dir/build_stale.XXXXXX")
        mv -T -- "$build_dir" "$backup_dir/build"
        printf 'Moved stale CMake build to %s\n' "$backup_dir/build"
    fi
fi

cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j"$(nproc)"
