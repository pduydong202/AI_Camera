#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
gst_options=$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 libcurl)
read -r -a gst_flags <<< "$gst_options"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I include tests/test_recorder.cpp src/recorder.cpp src/http_upload.cpp "${gst_flags[@]}" \
    -o "$temporary_dir/test_recorder"
"$temporary_dir/test_recorder" "$PWD/tests/fixtures/recorder_h264.mp4" "$temporary_dir/output"
if command -v ffmpeg >/dev/null; then
    ffmpeg -v error -xerror -i "$temporary_dir/output/good/clips/event_000001.mp4" -f null -
    printf 'PASS: generated clip decoded completely\n'
    ffmpeg -v error -xerror -i "$temporary_dir/output/good_tiny/clips/event_000001.mp4" -vsync 0 -enc_time_base -1 -f null -
    printf 'PASS: tiny-gap file clip decoded completely\n'
fi
