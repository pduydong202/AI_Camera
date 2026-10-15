#!/usr/bin/env bash
set -euo pipefail
log_dir=${1:?log directory required}
elapsed=${2:?elapsed seconds required}
shift 2
command -v pgrep >/dev/null || { printf 'pgrep is required for worker discovery.\n' >&2; exit 1; }
if [[ ! -e $log_dir/system_cpu_ticks.csv ]]; then
    printf 'elapsed_sec,cpu,user,nice,system,idle,iowait,irq,softirq,steal\n' > "$log_dir/system_cpu_ticks.csv"
    printf 'elapsed_sec,metric,value\n' > "$log_dir/system_metrics.csv"
fi
# Raw counters retain enough information to calculate utilization afterwards.
# Guest time is already included in user/nice and must not be counted twice.
awk -v elapsed="$elapsed" '/^cpu[0-9]* / {
    printf "%s,%s",elapsed,$1
    for(i=2;i<=9;++i) printf ",%.0f",$i
    printf "\n"
}' /proc/stat >> "$log_dir/system_cpu_ticks.csv"
awk -v elapsed="$elapsed" '/^(MemAvailable|MemTotal|SwapFree):/ {
    gsub(":","",$1); printf "%s,%s_kB,%s\n",elapsed,$1,$2
}' /proc/meminfo >> "$log_dir/system_metrics.csv"
for zone in /sys/class/thermal/thermal_zone*; do
    if [[ -r $zone/temp ]]; then
        if read -r temperature < "$zone/temp" && [[ $temperature =~ ^-?[0-9]+$ ]]; then
            printf '%s,%s_mC,%s\n' "$elapsed" "${zone##*/}" "$temperature" >> "$log_dir/system_metrics.csv"
        fi
    fi
done
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    if [[ -r $cpu/cpufreq/scaling_cur_freq ]]; then
        if read -r frequency < "$cpu/cpufreq/scaling_cur_freq" && [[ $frequency =~ ^[0-9]+$ ]]; then
            printf '%s,%s_kHz,%s\n' "$elapsed" "${cpu##*/}" "$frequency" >> "$log_dir/system_metrics.csv"
        fi
    fi
done
if [[ ! -e $log_dir/thermal_types.txt ]]; then
    for zone in /sys/class/thermal/thermal_zone*; do
        if [[ -r $zone/type ]]; then
            IFS= read -r type < "$zone/type" || continue
            printf '%s %s\n' "${zone##*/}" "$type"
        fi
    done > "$log_dir/thermal_types.txt"
fi
# Workers exec /proc/self/exe, so their Linux comm can be "exe". Select the
# supplied supervisor PIDs and their current children rather than matching names.
process_ids=()
for parent in "$@"; do
    [[ $parent =~ ^[0-9]+$ && -d /proc/$parent ]] || continue
    process_ids+=("$parent")
    # The vendor kernel need not expose /proc/.../children. pgrep -P is
    # already used successfully by the launcher's native affinity snapshot.
    while IFS= read -r child; do
        [[ $child =~ ^[0-9]+$ ]] && process_ids+=("$child")
    done < <(pgrep -P "$parent" || true)
done
if [[ ! -e $log_dir/process_memory.csv ]]; then
    printf 'elapsed_sec,pid,ppid,name,rss_kib,vmsize_kib,rss_anon_kib,threads\n' > "$log_dir/process_memory.csv"
fi
for process in "${process_ids[@]}"; do
    if [[ -r /proc/$process/status ]]; then
        awk -v elapsed="$elapsed" -v pid="$process" '
            /^Name:/ {name=$2}
            /^PPid:/ {ppid=$2}
            /^VmRSS:/ {rss=$2; found=1}
            /^VmSize:/ {size=$2}
            /^RssAnon:/ {anon=$2}
            /^Threads:/ {threads=$2}
            END {if(found)printf "%s,%s,%s,%s,%s,%s,%s,%s\n",elapsed,pid,ppid,name,rss,size,anon,threads}
        ' "/proc/$process/status" >> "$log_dir/process_memory.csv" 2>> "$log_dir/system_probe_errors.txt" || true
    fi
done
{
    printf '\nSNAPSHOT elapsed_sec=%s\n' "$elapsed"
    if ((${#process_ids[@]}>0)); then
        pid_list=$(IFS=,; printf '%s' "${process_ids[*]}")
        ps -p "$pid_list" -o pid,ppid,comm,psr,pcpu,rss,nlwp,etime || true
    fi
} >> "$log_dir/process_samples.txt" 2>> "$log_dir/system_probe_errors.txt"
