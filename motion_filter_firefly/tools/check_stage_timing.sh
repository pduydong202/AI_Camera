#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I include tests/test_stage_timing.cpp -o "$temporary_dir/test_stage_timing"
"$temporary_dir/test_stage_timing"
