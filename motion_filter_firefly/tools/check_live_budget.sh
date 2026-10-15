#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
budget_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/motion_filter_live_budget.XXXXXX")
trap 'rm -rf -- "$budget_test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -Iinclude tests/test_live_budget.cpp -o "$budget_test_dir/test_live_budget"
"$budget_test_dir/test_live_budget"
