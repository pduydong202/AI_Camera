#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
log_dir=${1:?log directory required}
run_output=${2:?run output directory required}
declare -A settings=()
while IFS=$'\t' read -r key value; do settings[$key]=$value; done < "$log_dir/runtime_settings.tsv"
warmup=${settings[startup_warmup_sec]}
bash tools/summarize.sh "$log_dir" "$warmup" | tee "$log_dir/summary.txt"
if [[ -f $log_dir/system_cpu_ticks.csv ]]; then
    awk -F, 'NR>1 {
        cpu=$2; total=0; for(i=3;i<=10;++i) total+=$i
        if(!(cpu in first)){first[cpu]=total; idle[cpu]=$6; io[cpu]=$7}
        last[cpu]=total; last_idle[cpu]=$6; last_io[cpu]=$7
    } END {
        for(cpu in first){
            ticks=last[cpu]-first[cpu]
            if(ticks>0) printf "%s busy_pct=%.2f iowait_pct=%.2f\n",cpu,
                100*(ticks-(last_idle[cpu]-idle[cpu])-(last_io[cpu]-io[cpu]))/ticks,
                100*(last_io[cpu]-io[cpu])/ticks
        }
    }' "$log_dir/system_cpu_ticks.csv" | sort -V > "$log_dir/system_summary.txt"
    awk -F, 'NR>1 {
        if(!($2 in lo)||$3<lo[$2])lo[$2]=$3
        if(!($2 in hi)||$3>hi[$2])hi[$2]=$3
    } END {for(metric in lo)printf "%s min=%.0f max=%.0f\n",metric,lo[metric],hi[metric]}' \
        "$log_dir/system_metrics.csv" | sort >> "$log_dir/system_summary.txt"
fi
printf 'camera,session,file,size_bytes,codec,width,height,frames,duration,probe_ok\n' > "$log_dir/clip_inventory.csv"
partial_count=0
probe_failures=0
clips=()
while IFS= read -r -d '' path; do
    clips+=("$path")
    relative=${path#"$run_output"/}
    camera=${relative%%/*}
    rest=${relative#*/sessions/}; session=${rest%%/*}; file=${path##*/}
    size=$(stat -c '%s' "$path")
    codec=unknown; width=0; height=0; frames=unknown; duration=unknown; ok=SKIP
    if command -v ffprobe >/dev/null; then
        if fields=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height,nb_frames:format=duration -of default=noprint_wrappers=1 "$path" 2>> "$log_dir/clip_probe_errors.txt"); then
            ok=YES
            while IFS='=' read -r key value; do
                case "$key" in codec_name) codec=$value;; width) width=$value;; height) height=$value;; nb_frames) frames=$value;; duration) duration=$value;; esac
            done <<< "$fields"
            if [[ $codec != h264 && $codec != hevc ]]; then ok=NO; ((probe_failures+=1)); fi
        else ok=NO; ((probe_failures+=1)); fi
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$camera" "$session" "$file" "$size" "$codec" "$width" "$height" "$frames" "$duration" "$ok" >> "$log_dir/clip_inventory.csv"
done < <(find "$run_output" -type f -name '*.mp4' -print0 | sort -z)
mkdir -p "$log_dir/event_tables" "$log_dir/clip_samples"
while IFS= read -r -d '' path; do
    relative=${path#"$run_output"/}; destination="$log_dir/event_tables/$relative"
    mkdir -p -- "$(dirname -- "$destination")"; cp -- "$path" "$destination"
done < <(find "$run_output" -type f -name 'events.csv' -print0)
while IFS= read -r -d '' path; do ((partial_count+=1)); printf '%s\n' "${path#"$run_output"/}" >> "$log_dir/partial_clips.txt"; done < <(find "$run_output" -type f -name '*.mp4.part' -print0)

# Select evenly spaced samples from the first camera, within the YAML byte
# budget. The default two samples remain first/last. Full clips stay on board.
sample_bytes=0
sample_paths=()
sample_count=${settings[evidence_sample_clip_count]}
sample_max_bytes=$((settings[evidence_sample_max_mib]*1024*1024))
if ((${#clips[@]}>0 && sample_count>0 && sample_max_bytes>0)); then
    first_camera=${clips[0]#"$run_output"/}; first_camera=${first_camera%%/*}
    camera_clips=()
    for path in "${clips[@]}"; do
        rel=${path#"$run_output"/}
        if [[ ${rel%%/*} == "$first_camera" ]]; then camera_clips+=("$path"); fi
    done
    if ((sample_count>${#camera_clips[@]})); then sample_count=${#camera_clips[@]}; fi
    for ((i=0;i<sample_count;++i)); do
        index=0
        if ((sample_count>1)); then index=$((i*(${#camera_clips[@]}-1)/(sample_count-1))); fi
        sample_paths+=("${camera_clips[$index]}")
    done
fi
sample_index=0
for path in "${sample_paths[@]}"; do
    size=$(stat -c '%s' "$path")
    if ((sample_bytes+size>sample_max_bytes)); then continue; fi
    ((sample_bytes+=size)); ((sample_index+=1))
    destination="$log_dir/clip_samples/sample_$sample_index.mp4"
    cp -- "$path" "$destination"
    printf '%s\t%s\n' "${destination##*/}" "${path#"$run_output"/}" >> "$log_dir/clip_samples/index.tsv"
    if command -v ffmpeg >/dev/null; then
        # Preserve fine input timestamps while decoding to null. Coarse output
        # encoder timebases can generate warnings on valid VFR/jittered clips.
        if ffmpeg -nostdin -v error -xerror -i "$destination" -map 0:v:0 -vsync 0 -enc_time_base -1 -f null - 2> "$destination.decode.txt"; then
            printf 'sample_%s decode_exit=0\n' "$sample_index" >> "$log_dir/sample_decode.txt"
        else
            printf 'sample_%s decode_exit=FAIL\n' "$sample_index" >> "$log_dir/sample_decode.txt"; ((probe_failures+=1))
        fi
    fi
done
{
    printf 'clips=%s partial_clips=%s media_check_failures=%s\n' "${#clips[@]}" "$partial_count" "$probe_failures"
    for log in "$log_dir"/cameras/*.log; do
        awk -v camera="${log##*/}" '
            /\[WATCH\] start / {attempts++}
            /\[WATCH\] first_frame / {sessions++}
            /\[WATCH\] forced_kill / {forced++}
            /\[ERROR\]/ {errors++}
            /mpp_frame_deinit/ {mpp++}
            END {printf "%s attempts=%d sessions_with_frames=%d forced_kills=%d app_errors=%d mpp_deinit_lines=%d\n",camera,attempts,sessions,forced,errors,mpp}
        ' "$log"
    done
} | tee "$log_dir/run_health.txt"
if [[ -f $log_dir/process_memory.csv ]]; then
    printf 'camera,worker_pid,samples,min_rss_kib,max_rss_kib,last_rss_kib\n' > "$log_dir/worker_memory_summary.csv"
    expected_workers=0
    sampled_workers=0
    for log in "$log_dir"/cameras/*.log; do
        while IFS= read -r worker; do
            ((expected_workers+=1))
            camera=${log##*/}; camera=${camera%.log}
            row=$(awk -F, -v camera="$camera" -v worker="$worker" '
                NR>1 && $2==worker {
                    n++;rss=$5+0
                    if(n==1||rss<lo)lo=rss
                    if(n==1||rss>hi)hi=rss
                    last=rss
                }
                END {printf "%s,%s,%d,%d,%d,%d\n",camera,worker,n,lo,hi,last}
            ' "$log_dir/process_memory.csv")
            printf '%s\n' "$row" >> "$log_dir/worker_memory_summary.csv"
            IFS=, read -r _camera _worker samples _rest <<< "$row"
            if ((samples>0)); then ((sampled_workers+=1)); fi
        done < <(sed -nE 's/.*\[WATCH\] start attempt=[0-9]+ worker_pid=([0-9]+).*/\1/p' "$log" | sort -u)
    done
    printf 'RSS_CHECK sampled_workers=%s total_workers=%s\n' "$sampled_workers" "$expected_workers" | tee -a "$log_dir/run_health.txt"
fi
mkdir -p evidence
archive="evidence/${log_dir##*/}.tar.gz"
tar -czf "$archive" -C "$(dirname -- "$log_dir")" "${log_dir##*/}"
printf '[EVIDENCE] %s/%s\n' "$PWD" "$archive"
ls -lh -- "$archive"
if ((partial_count>0||probe_failures>0)); then exit 1; fi
