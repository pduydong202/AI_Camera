#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs opencv4)"
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/test_algorithm_config.cpp src/config.cpp src/motion_pipeline.cpp "${flags[@]}" \
    -o "$temporary_dir/check_algorithm_config"
"$temporary_dir/check_algorithm_config" config.yaml
