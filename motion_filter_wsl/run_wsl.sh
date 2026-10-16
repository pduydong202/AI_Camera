#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 1 ]]; then echo "Usage: $0 <video> [policy.yaml] [camera_id]"; exit 1; fi
exec ./build/motion_filter "$@"
