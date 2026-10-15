#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
project_dir=$PWD
binary=${MOTION_FILTER_BIN:-$project_dir/build/motion_filter}
config_file=${MOTION_CONFIG_FILE:-$project_dir/config.yaml}
env_file=${MOTION_ENV_FILE:-$project_dir/.env}
duration=0
replicas=0
dry_run=0
while (($#)); do
    case "$1" in
        --seconds|--replicas)
            option=$1
            if (($#<2)) || [[ ! $2 =~ ^[0-9]+$ ]]; then printf 'Invalid %s\n' "$option" >&2; exit 1; fi
            if [[ $option == --seconds ]]; then duration=$((10#$2)); else
                replicas=$((10#$2)); ((replicas>0)) || { printf 'Replicas must be >0.\n' >&2; exit 1; }
            fi
            shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        *) printf 'Usage: bash run.sh [--seconds N] [--replicas N] [--dry-run]\n' >&2; exit 1 ;;
    esac
done
[[ -x $binary && -f $config_file && -f $env_file ]] || { printf 'Run bash build.sh first; check config.yaml and .env.\n' >&2; exit 1; }

# Read .env as literal data. Do not source or eval credential values.
declare -A env_values=()
line_number=0
trim() { local text=$1; text="${text#"${text%%[![:space:]]*}"}"; text="${text%"${text##*[![:space:]]}"}"; REPLY=$text; }
while IFS= read -r line || [[ -n $line ]]; do
    ((line_number+=1)); line=${line%$'\r'}; trim "$line"; line=$REPLY
    [[ -z $line || $line == \#* ]] && continue
    if [[ ! $line =~ ^([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=(.*)$ ]]; then
        printf 'Invalid .env syntax at line %s\n' "$line_number" >&2; exit 1
    fi
    key=${BASH_REMATCH[1]}; value=${BASH_REMATCH[2]}; trim "$value"; value=$REPLY
    first=${value:0:1}; last=${value: -1}
    if [[ $first == '"' || $first == "'" ]]; then
        [[ ${#value} -ge 2 && $last == "$first" ]] || { printf 'Unclosed .env quote at line %s\n' "$line_number" >&2; exit 1; }
        value=${value:1:${#value}-2}
    fi
    [[ ! -v env_values[$key] ]] || { printf 'Duplicate .env key: %s\n' "$key" >&2; exit 1; }
    env_values[$key]=$value
done < "$env_file"

inventory=$("$binary" --camera-list "$config_file")
base_output=$("$binary" --output-root "$config_file")
runtime_settings=$("$binary" --runtime-settings "$config_file")
declare -A settings=()
while IFS=$'\t' read -r key value; do settings[$key]=$value; done <<< "$runtime_settings"
poll_seconds=$(awk -v ms="${settings[launcher_poll_ms]}" 'BEGIN {printf "%.3f",ms/1000}')
mapfile -t inventory_rows <<< "$inventory"
if ((replicas>0 && ${#inventory_rows[@]}!=1)); then printf '%s\n' '--replicas requires exactly one configured camera.' >&2; exit 1; fi
declare -a ids=() urls=() codecs=() gops=()
for row in "${inventory_rows[@]}"; do
    IFS=$'\t' read -r id url_key codec gop <<< "$row"
    [[ -n $id && -n $url_key && -n $codec && -n $gop ]] || { printf 'Invalid camera inventory.\n' >&2; exit 1; }
    url=${env_values[$url_key]:-}
    [[ -n $url ]] || { printf 'Missing URL key in .env: %s\n' "$url_key" >&2; exit 1; }
    [[ $url == rtsp://* ]] || { printf 'Expected rtsp:// URL for camera %s\n' "$id" >&2; exit 1; }
    count=$replicas; ((count>0)) || count=1
    for ((i=1;i<=count;++i)); do
        camera=$id
        if ((replicas>0)); then printf -v camera '%s_r%02d' "$id" "$i"; fi
        ids+=("$camera"); urls+=("$url"); codecs+=("$codec"); gops+=("$gop")
        if ((dry_run)); then printf '[PLAN] camera=%s source_key=%s codec=%s gop_hint=%s\n' "$camera" "$url_key" "$codec" "$gop"; fi
    done
done
if ((dry_run)); then printf '[PLAN] cameras=%s seconds=%s; no connections started\n' "${#ids[@]}" "$duration"; exit 0; fi

mkdir -p logs
exec 9>logs/run.lock
flock -n 9 || { printf 'Another launcher from this project is still running.\n' >&2; exit 1; }
if [[ -z ${MOTION_FILTER_BIN:-} ]] && pgrep -x motion_filter >/dev/null; then
    printf 'Another motion_filter test is still running; stop it first.\n' >&2; exit 1
fi
tag="$(date +%Y%m%dT%H%M%S)_pid$$"
log_dir="$project_dir/logs/$tag"
run_output="$base_output/$tag"
mkdir -p "$log_dir/cameras" "$run_output"
cp -- "$config_file" "$log_dir/config.yaml"
printf '%s\n' "$runtime_settings" > "$log_dir/runtime_settings.tsv"
printf '%s\n' "$inventory" > "$log_dir/camera_inventory.tsv"
printf 'run_id=%s\ncameras=%s\nseconds=%s\n' "$tag" "${#ids[@]}" "$duration" > "$log_dir/run.txt"
sha256sum src/*.cpp include/*.hpp > "$log_dir/source_sha256.txt"
sha256sum run.sh tools/system_snapshot.sh tools/collect.sh tools/summarize.sh > "$log_dir/script_sha256.txt"
sha256sum -- "$binary" > "$log_dir/binary_sha256.txt"
date -Ins > "$log_dir/date.txt"
if command -v gst-inspect-1.0 >/dev/null; then
    gst-inspect-1.0 mppvideodec > "$log_dir/mpp_plugin.txt" 2>&1 || true
    plugin_file=$(awk '$1=="Filename" {print $2;exit}' "$log_dir/mpp_plugin.txt")
    if [[ -f $plugin_file ]]; then
        mkdir -p "$log_dir/decoder_stack"
        cp -- "$plugin_file" "$log_dir/decoder_stack/"
        sha256sum -- "$plugin_file" > "$log_dir/decoder_stack/plugin_sha256.txt"
        ldd "$plugin_file" > "$log_dir/decoder_stack/ldd.txt" 2>&1 || true
    fi
fi

declare -a pids=()
stopping=0
cleaned=0
failed=0
on_signal() { stopping=1; }
cleanup() {
    ((cleaned==0)) || return 0
    cleaned=1
    trap - EXIT INT TERM
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for i in "${!pids[@]}"; do
        status=0; wait "${pids[$i]}" || status=$?
        printf 'camera=%s supervisor_pid=%s exit=%s\n' "${ids[$i]}" "${pids[$i]}" "$status" >> "$log_dir/exits.txt"
        if ((status!=0)); then failed=1; fi
    done
}
trap on_signal INT TERM
trap cleanup EXIT
for i in "${!ids[@]}"; do
    MOTION_RUN_ID="$tag" MOTION_LAUNCHER_PID="$$" \
        stdbuf -oL -eL "$binary" "${urls[$i]}" "$config_file" "${ids[$i]}" "${codecs[$i]}" "${gops[$i]}" \
        > "$log_dir/cameras/${ids[$i]}.log" 2>&1 &
    pids+=("$!")
done
started=$SECONDS
last_status=-${settings[status_interval_sec]}
last_sample=-${settings[system_sample_interval_sec]}
snapshot_done=0
printf '[RUN] cameras=%s duration=%ss logs=%s\n' "${#ids[@]}" "$duration" "$log_dir"
while ((stopping==0)); do
    elapsed=$((SECONDS-started))
    for i in "${!pids[@]}"; do
        if ! kill -0 "${pids[$i]}" 2>/dev/null; then
            printf '[RUN] camera=%s exited early; inspect its log.\n' "${ids[$i]}" >&2
            failed=1; stopping=1
        fi
    done
    if ((duration>0 && elapsed>=duration)); then break; fi
    if ((elapsed-last_sample>=settings[system_sample_interval_sec])); then
        bash tools/system_snapshot.sh "$log_dir" "$elapsed" "${pids[@]}"
        last_sample=$elapsed
    fi
    if ((elapsed-last_status>=settings[status_interval_sec])); then
        live=0
        for id in "${ids[@]}"; do if grep -q '\[PERF\]' "$log_dir/cameras/$id.log"; then ((live+=1)); fi; done
        printf '[RUN] elapsed=%ss cameras_with_perf=%s/%s\n' "$elapsed" "$live" "${#ids[@]}"
        last_status=$elapsed
    fi
    if ((elapsed>=settings[affinity_snapshot_after_sec] && snapshot_done==0)); then
        for pid in "${pids[@]}"; do
            for child in $(pgrep -P "$pid" || true); do
                for task in /proc/"$child"/task/*; do
                    if [[ -f $task/status ]]; then
                        awk -v pid="$child" -v task="${task##*/}" '/^Name:|^Cpus_allowed_list:/ {print "pid=" pid, "tid=" task, $0}' "$task/status" >> "$log_dir/thread_affinity.txt" || true
                    fi
                done
            done
        done
        snapshot_done=1
    fi
    # A signal can interrupt sleep. Let the loop observe stopping and run the
    # normal clip-finalization/evidence path instead of exiting via set -e.
    sleep "$poll_seconds" || true
done
printf '[RUN] stopping cameras and finalizing clips...\n'
bash tools/system_snapshot.sh "$log_dir" "$((SECONDS-started))" "${pids[@]}"
cleanup
bash tools/collect.sh "$log_dir" "$run_output" || failed=1
printf '[RUN] finished; process_failures=%s\n' "$failed"
exit "$failed"
