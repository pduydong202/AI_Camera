#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
trap 'rm -rf -- "$temporary_dir"' EXIT
read -r -a flags <<< "$(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/test_rtsp_tail_guard.cpp "${flags[@]}" -o "$temporary_dir/test"
# Several NAL slices per picture are essential: a complete early slice plus a
# truncated final FU is what the depayloader otherwise flushes on disconnect.
ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=128x96:rate=20 -frames:v 3 \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -x264-params slices=3:keyint=20:min-keyint=20:scenecut=0:bframes=0 \
    -y "$temporary_dir/multi_slice.mp4"
"$temporary_dir/test" "$temporary_dir/multi_slice.mp4"
