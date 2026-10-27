#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 libcurl)"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/test_http_recorder.cpp src/recorder.cpp src/http_upload.cpp src/event_manager.cpp "${flags[@]}" \
    -o "$temporary_dir/test_http_recorder"
PYTHONDONTWRITEBYTECODE=1 python3 ../event_receiver/test_sender.py \
    "$temporary_dir/test_http_recorder" "$PWD/tests/fixtures/recorder_h264.mp4" "${@}"
PYTHONDONTWRITEBYTECODE=1 python3 ../event_receiver/test_receiver.py
PYTHONDONTWRITEBYTECODE=1 python3 ../event_receiver/test_fmp4_timing.py
