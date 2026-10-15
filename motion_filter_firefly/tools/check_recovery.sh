#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I include tests/test_supervised_process.cpp -o build/test_supervised_process
./build/test_supervised_process
