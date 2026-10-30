#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
python3 tools/build_rockchip_plugin.py
if [[ ! -f .env ]]; then install -m 600 .env.example .env; fi
chmod 600 .env
./build/motion_filter --camera-list config.yaml
printf 'Build complete. Run bash check.sh, then bash run.sh --dry-run.\n'
