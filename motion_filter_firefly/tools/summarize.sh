#!/usr/bin/env bash
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
log_dir="${1:?Usage: bash tools/summarize.sh <log_dir> [warmup_sec]}"
warmup_sec="${2:-$(awk '$1 == "startup_warmup_sec:" {print $2; exit}' "$script_dir/../config.yaml")}"
if [[ ! -d "$log_dir" || ! "$warmup_sec" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Check the log directory and warmup seconds."
    exit 1
fi
shopt -s nullglob
log_files=("$log_dir"/cameras/*.log)
if ((${#log_files[@]} == 0)); then
    echo "No camera log files found."
    exit 1
fi
printf 'SUMMARY (windows starting after warmup=%s seconds)\n' "$warmup_sec"
for log_file in "${log_files[@]}"; do
    label="${log_file##*/}"
    label="${label%.log}"
    awk -v label="$label" -v warmup="$warmup_sec" '
        /\[PERF\]/ {
            for (key in value) delete value[key]
            for (i=1; i<=NF; ++i) {
                if (split($i, pair, "=") == 2) value[pair[1]] = pair[2]
            }
            dt = value["dt_sec"] + 0
            n = value["frames"] + 0
            if (dt <= 0 || n <= 0 || value["wall_sec"] - dt < warmup) next
            seconds += dt
            frames += n
            if (value["decode_fps"] == "nan") unknown_decode = 1
            else decoded += value["decode_fps"] * dt
            skipped += value["sample_skips"]
            wait_sum += value["wait_ms"] * n
            map_sum += value["map_ms"] * n
            rga_sum += value["rga_ms"] * n
            process_sum += value["process_ms"] * n
            event_sum += value["event_ms"] * n
            cpu_seconds += value["cpu_pct"] * dt / 100
            if ("rga_cpu_ms" in value && "process_cpu_ms" in value) {
                cpu_detail_seen = 1
                cpu_detail_frames += n
                process_detail_wall += value["process_ms"] * n
                if (value["rga_cpu_ms"] == "nan") rga_cpu_unknown = 1
                else rga_cpu_sum += value["rga_cpu_ms"] * n
                if (value["process_cpu_ms"] == "nan") process_cpu_unknown = 1
                else process_cpu_sum += value["process_cpu_ms"] * n
                if (value["main_cpu_ms"] == "nan") main_cpu_unknown = 1
                else main_cpu_sum += value["main_cpu_ms"] * n
            }
            if ("input_gaps" in value) {
                diagnostics_seen = 1
                input_gaps += value["input_gaps"]
                metadata_misses += value["metadata_misses"]
                pts_repaired += value["pts_repaired"]
                latency_n = value["latency_frames"] + 0
                if (latency_n > 0 && value["queue_age_ms"] != "nan" && value["local_delay_ms"] != "nan") {
                    latency_frames += latency_n
                    queue_sum += value["queue_age_ms"] * latency_n
                    local_sum += value["local_delay_ms"] * latency_n
                    if (value["queue_p95_ms"] > queue_p95_worst) queue_p95_worst = value["queue_p95_ms"]
                    if (value["local_p95_ms"] > local_p95_worst) local_p95_worst = value["local_p95_ms"]
                    if (value["queue_max_ms"] > queue_max) queue_max = value["queue_max_ms"]
                    if (value["local_max_ms"] > local_max) local_max = value["local_max_ms"]
                }
            }
        }
        END {
            if (seconds <= 0 || frames <= 0) {
                printf "%s: no usable post-warmup PERF windows\n", label
            } else {
                decode_text = unknown_decode ? "nan" : sprintf("%.2f", decoded / seconds)
                printf "%s seconds=%.2f frames=%d analysis_fps=%.2f decode_fps=%s skips=%d wait_ms=%.2f map_ms=%.2f rga_ms=%.2f process_ms=%.2f event_ms=%.2f cpu_pct=%.2f cpu_ms_per_frame=%.2f", label, seconds, frames, frames/seconds, decode_text, skipped, wait_sum/frames, map_sum/frames, rga_sum/frames, process_sum/frames, event_sum/frames, 100*cpu_seconds/seconds, 1000*cpu_seconds/frames
                if (cpu_detail_seen && cpu_detail_frames > 0) {
                    non_cpu = (process_detail_wall-process_cpu_sum)/cpu_detail_frames
                    if (non_cpu < 0) non_cpu = 0
                    printf " rga_cpu_ms=%s process_cpu_ms=%s process_non_cpu_ms=%s main_cpu_ms=%s", rga_cpu_unknown ? "nan" : sprintf("%.2f",rga_cpu_sum/cpu_detail_frames), process_cpu_unknown ? "nan" : sprintf("%.2f",process_cpu_sum/cpu_detail_frames), process_cpu_unknown ? "nan" : sprintf("%.2f",non_cpu), main_cpu_unknown ? "nan" : sprintf("%.2f",main_cpu_sum/cpu_detail_frames)
                }
                if (diagnostics_seen) {
                    printf " input_gaps=%d metadata_misses=%d pts_repaired=%d", input_gaps, metadata_misses, pts_repaired
                    if (latency_frames > 0) {
                        printf " queue_age_ms=%.2f queue_p95_worst_window_ms=%.2f queue_max_ms=%.2f local_delay_ms=%.2f local_p95_worst_window_ms=%.2f local_max_ms=%.2f", queue_sum/latency_frames, queue_p95_worst, queue_max, local_sum/latency_frames, local_p95_worst, local_max
                    } else printf " latency=unavailable"
                }
                printf "\n"
            }
        }
    ' "$log_file"
    awk -v label="$label" -v warmup="$warmup_sec" '
        BEGIN {
            count=split("setup mog cleanup l2_prior ccl l2_evidence l2_update l3 decision debug",names," ")
        }
        /\[STAGE\]/ {
            for (k in v) delete v[k]
            for (i=1; i<=NF; ++i) if (split($i,pair,"=")==2) v[pair[1]]=pair[2]
            dt=v["dt_sec"]+0; n=v["frames"]+0
            if (dt<=0 || n<=0 || v["wall_sec"]-dt<warmup) next
            frames+=n
            if ("l3_region_pct" in v) {
                region_seen=1
                region_frames+=n
                region_sum+=v["l3_region_pct"]*n
                empty_fast+=v["l3_empty_fast"]
            }
            for (j=1; j<=count; ++j) {
                wall_key=names[j] "_ms"; cpu_key=names[j] "_cpu_ms"
                if (!(wall_key in v) || v[wall_key]=="nan") unknown[wall_key]=1
                else sums[wall_key]+=v[wall_key]*n
                if (!(cpu_key in v) || v[cpu_key]=="nan") unknown[cpu_key]=1
                else sums[cpu_key]+=v[cpu_key]*n
            }
        }
        END {
            if (frames>0) {
                printf "%s STAGES frames=%d",label,frames
                for (j=1; j<=count; ++j) {
                    wall_key=names[j] "_ms"; cpu_key=names[j] "_cpu_ms"
                    printf " %s=%s %s=%s",wall_key,unknown[wall_key]?"nan":sprintf("%.2f",sums[wall_key]/frames),cpu_key,unknown[cpu_key]?"nan":sprintf("%.2f",sums[cpu_key]/frames)
                }
                if (region_seen && region_frames>0)
                    printf " l3_region_pct=%.2f l3_empty_fast=%d",region_sum/region_frames,empty_fast
                printf "\n"
            }
        }
    ' "$log_file"
    awk -v label="$label" -v warmup="$warmup_sec" '
        /\[PTS\]/ {
            for (k in v) delete v[k]
            for (i=1; i<=NF; ++i) if (split($i,pair,"=")==2) v[pair[1]]=pair[2]
            if (v["dt_sec"]+0<=0 || v["wall_sec"]-v["dt_sec"]<warmup) next
            b=v["branch"]
            if (b!="analysis" && b!="encoded") next
            seen[b]=1
            observed[b]+=v["observed"]; repaired[b]+=v["repaired"]
            missing[b]+=v["raw_missing"]; duplicate[b]+=v["raw_duplicate"]
            backward[b]+=v["raw_backward"]; catchup[b]+=v["catchup"]
            if (v["repair_run_max"]+0>runmax[b]) runmax[b]=v["repair_run_max"]+0
            if (v["offset_max_ms"]+0>offsetmax[b]) offsetmax[b]=v["offset_max_ms"]+0
            if (v["raw_forward_gap_max_ms"]+0>forwardmax[b]) forwardmax[b]=v["raw_forward_gap_max_ms"]+0
            if (v["raw_backward_gap_max_ms"]+0>backwardmax[b]) backwardmax[b]=v["raw_backward_gap_max_ms"]+0
        }
        END {
            split("analysis encoded",branches," ")
            for (j=1; j<=2; ++j) {
                b=branches[j]
                if (seen[b])
                    printf "%s PTS branch=%s observed=%d repaired=%d raw_missing=%d raw_duplicate=%d raw_backward=%d catchup=%d repair_run_max=%d offset_max_ms=%.6f raw_forward_gap_max_ms=%.6f raw_backward_gap_max_ms=%.6f\n",label,b,observed[b],repaired[b],missing[b],duplicate[b],backward[b],catchup[b],runmax[b],offsetmax[b],forwardmax[b],backwardmax[b]
            }
        }
    ' "$log_file"
done
