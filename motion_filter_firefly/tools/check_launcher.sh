#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
temporary_dir=$(mktemp -d)
active_launcher=
cleanup_test() {
    if [[ -n $active_launcher ]]; then
        kill -TERM "$active_launcher" 2>/dev/null || true
        wait "$active_launcher" 2>/dev/null || true
    fi
    rm -rf -- "$temporary_dir"
}
trap cleanup_test EXIT
options=$(pkg-config --cflags --libs opencv4)
read -r -a flags <<< "$options"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -I include \
    tests/fake_camera.cpp src/config.cpp "${flags[@]}" -o "$temporary_dir/fake_camera"
# Exercise the real launcher in an isolated copy. Self-tests must not leave
# simulated logs/evidence in the production project's run directories.
mkdir -p "$temporary_dir/project"
cp -a run.sh tools src include config.yaml "$temporary_dir/project/"
cd "$temporary_dir/project"
export MOTION_FILTER_BIN="$temporary_dir/fake_camera"
export MOTION_ENV_FILE="$temporary_dir/.env"
export MOTION_TEST_ARGS_DIR="$temporary_dir"
export MOTION_CONFIG_FILE="$temporary_dir/config.yaml"
sed -e "s#^output_root:.*#output_root: \"$temporary_dir/output\"#" \
    -e 's/^status_interval_sec:.*/status_interval_sec: 1/' \
    -e 's/^system_sample_interval_sec:.*/system_sample_interval_sec: 1/' \
    -e 's/^affinity_snapshot_after_sec:.*/affinity_snapshot_after_sec: 0/' \
    -e 's/^launcher_poll_ms:.*/launcher_poll_ms: 100/' \
    -e 's/^evidence_sample_clip_count:.*/evidence_sample_clip_count: 0/' \
    config.yaml > "$MOTION_CONFIG_FILE"
literal_url='rtsp://user:literal$word@host/cam?x=$(touch SHOULD_NOT_EXIST)&y=a=b#fragment'
printf 'CAM01_RTSP="%s"\n' "$literal_url" > "$MOTION_ENV_FILE"
bash run.sh --replicas 3 --dry-run > "$temporary_dir/plan.txt"
[[ ! -e SHOULD_NOT_EXIST ]]
if grep -Fq "$literal_url" "$temporary_dir/plan.txt"; then printf 'URL leaked in plan.\n' >&2; exit 1; fi
bash run.sh --replicas 3 --seconds 3 > "$temporary_dir/run.txt" 2>&1
grep -q '\[RUN\] elapsed=1s' "$temporary_dir/run.txt"
first_log=$(sed -n 's/.* logs=//p' "$temporary_dir/run.txt" | head -1)
grep -q '^SNAPSHOT elapsed_sec=1$' "$first_log/process_samples.txt"
grep -q $'^launcher_poll_ms\t100$' "$first_log/runtime_settings.tsv"
grep -q $'^evidence_sample_clip_count\t0$' "$first_log/runtime_settings.tsv"
for id in cam01_r01 cam01_r02 cam01_r03; do
    IFS= read -r actual < "$temporary_dir/$id.args"
    [[ $actual == "$literal_url" ]]
    pid=$(sed -n '2p' "$temporary_dir/$id.args")
    if kill -0 "$pid" 2>/dev/null; then printf 'Launcher left a camera running.\n' >&2; exit 1; fi
done
[[ ! -e SHOULD_NOT_EXIST ]]
printf 'PASS: launcher replicas, literal .env values, credential-safe plan, YAML cadence, bounded run and child cleanup\n'

mkdir -p "$temporary_dir/cancel_args"
export MOTION_TEST_ARGS_DIR="$temporary_dir/cancel_args"
bash run.sh --replicas 2 --seconds 30 > "$temporary_dir/cancel.txt" 2>&1 &
active_launcher=$!
for ((attempt=0;attempt<50;++attempt)); do
    [[ -s $MOTION_TEST_ARGS_DIR/cam01_r02.args ]] && break
    sleep 0.1
done
[[ -s $MOTION_TEST_ARGS_DIR/cam01_r02.args ]]
if bash run.sh --seconds 1 > "$temporary_dir/locked.txt" 2>&1; then
    printf 'Overlapping launcher accepted.\n' >&2; exit 1
fi
kill -TERM "$active_launcher"
wait "$active_launcher"
active_launcher=
grep -q '\[EVIDENCE\]' "$temporary_dir/cancel.txt"
cancel_log=$(sed -n 's/.* logs=//p' "$temporary_dir/cancel.txt" | head -1)
grep -q '^SNAPSHOT elapsed_sec=' "$cancel_log/process_samples.txt"
for id in cam01_r01 cam01_r02; do
    pid=$(sed -n '2p' "$MOTION_TEST_ARGS_DIR/$id.args")
    if kill -0 "$pid" 2>/dev/null; then printf 'Cancelled camera still running.\n' >&2; exit 1; fi
done
printf 'PASS: signal cancellation finalizes evidence; overlapping launcher rejected\n'
export MOTION_TEST_ARGS_DIR="$temporary_dir"

printf '\n  - { id: cam02, url_env: CAM02_RTSP, codec: h264, gop_hint_sec: 0.0 }\n' >> "$MOTION_CONFIG_FILE"
if bash run.sh --dry-run > "$temporary_dir/missing.txt" 2>&1; then printf 'Missing URL accepted.\n' >&2; exit 1; fi
if bash run.sh --replicas 2 --dry-run > "$temporary_dir/multi.txt" 2>&1; then printf 'Ambiguous replication accepted.\n' >&2; exit 1; fi
sed "s#^output_root:.*#output_root: \"$temporary_dir/output\"#" config.yaml > "$MOTION_CONFIG_FILE"
printf 'CAM01_RTSP=rtsp://host/fatal\n' > "$MOTION_ENV_FILE"
if bash run.sh --replicas 2 --seconds 3 > "$temporary_dir/fatal.txt" 2>&1; then printf 'Early camera failure was hidden.\n' >&2; exit 1; fi
printf 'PASS: missing camera URL, ambiguous replication and fatal child failure rejected\n'
