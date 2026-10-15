#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
options=$(pkg-config --cflags --libs opencv4)
read -r -a flags <<< "$options"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/test_config.cpp src/config.cpp "${flags[@]}" -o "$temporary_dir/check_config"
"$temporary_dir/check_config" config.yaml "$temporary_dir/fixtures"
