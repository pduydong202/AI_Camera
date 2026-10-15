#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
compiler="${CXX:-g++}"
"$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
    tests/test_l2_capture.cpp -o "$temporary_dir/test_capture"
"$temporary_dir/test_capture" "$temporary_dir"
mkdir -p build
"$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I include \
    tools/l2_capture_inspect.cpp -o build/l2_capture_inspect
