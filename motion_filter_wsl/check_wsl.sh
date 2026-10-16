#!/usr/bin/env bash
set -euo pipefail
for c in cmake g++ pkg-config; do command -v "$c" >/dev/null || { echo "MISSING: $c"; exit 1; }; done
pkg-config --modversion opencv4
echo READY
