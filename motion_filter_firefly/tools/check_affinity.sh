#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/test_main_affinity.cpp -o "$temporary_dir/check_affinity"
"$temporary_dir/check_affinity"
