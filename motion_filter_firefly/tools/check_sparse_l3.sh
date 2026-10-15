#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
command -v pkg-config >/dev/null || { echo "Missing pkg-config/OpenCV SDK." >&2; exit 1; }
pkg-config --exists opencv4 || { echo "OpenCV development files are required for this checker." >&2; exit 1; }
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
read -r -a opencv_flags <<< "$(pkg-config --cflags --libs opencv4)"
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I include -I tests/reference tests/test_sparse_l3.cpp \
    tests/reference/motion_pipeline_reference.cpp src/motion_pipeline.cpp \
    src/config.cpp src/event_manager.cpp "${opencv_flags[@]}" \
    -o "$temporary_dir/test_sparse_l3"
"$temporary_dir/test_sparse_l3" config.yaml "${1:-}"
