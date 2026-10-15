#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
for check in config affinity recovery recorder sparse_l3 algorithm_config l2_capture live_budget pts_diagnostics stage_timing log_line launcher; do
    printf '\n[CHECK] %s\n' "$check"
    bash "tools/check_$check.sh"
done
