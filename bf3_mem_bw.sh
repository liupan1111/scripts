#!/usr/bin/env bash
set -euo pipefail
interval="${1:-1}"
rounds="${2:-10}"
mode="${3:-mss}"

modprobe mlxbf-pmc 2>/dev/null || modprobe mlxbf_pmc 2>/dev/null || true

B=""
for h in /sys/class/hwmon/hwmon*; do
  if [[ -r "$h/name" ]] && [[ "$(cat "$h/name")" == "bfperf" ]]; then
    B="$h"
    break
  fi
done
[[ -n "$B" ]] || { echo "ERROR: bfperf not found" >&2; exit 1; }

blocks=()
case "$mode" in
  mss)
    for d in "$B"/mss[0-1]; do [[ -d "$d" ]] && blocks+=("$d"); done
    # Data flits on CHI DATA channels. BF3 public docs do not state the flit size;
    # use STREAM calibration before treating this as exact bytes.
    events=(0x15 0x1d 0x21 0x33 0x3b 0x3f)
    labels=(CHI_DATA0_TX CHI_DATA2_TX CHI_DATA3_TX CHI_DATA0_RX CHI_DATA2_RX CHI_DATA3_RX)
    ;;
  hnf)
    for d in "$B"/llt[0-7]; do [[ -d "$d" ]] && blocks+=("$d"); done
    events=(0xb 0xc 0x35 0x36)
    labels=(HNF0_MEM_READS HNF0_MEM_WRITES HNF1_MEM_READS HNF1_MEM_WRITES)
    ;;
  miss)
    for d in "$B"/llt_miss[0-7]; do [[ -d "$d" ]] && blocks+=("$d"); done
    events=(0x0 0x1 0x5 0x6 0xb 0xc)
    labels=(RD_REQ WR_REQ RD_RESP WR_RESP CHI_TXDAT CHI_RXDAT)
    ;;
  *) echo "usage: $0 [interval] [rounds] [mss|hnf|miss]" >&2; exit 2 ;;
esac
[[ ${#blocks[@]} -gt 0 ]] || { echo "ERROR: no blocks for mode $mode" >&2; exit 1; }

n=${#events[@]}
for d in "${blocks[@]}"; do
  echo 0 > "$d/enable" 2>/dev/null || true
  for ((i=0; i<n; i++)); do
    echo "${events[$i]}" > "$d/event$i"
  done
  echo 1 > "$d/enable"
done

read_sum() {
  local total=0 raw dec d i
  for d in "${blocks[@]}"; do
    for ((i=0; i<n; i++)); do
      raw=$(cat "$d/counter$i")
      dec=$((raw))
      total=$((total + dec))
    done
  done
  echo "$total"
}

cleanup() {
  for d in "${blocks[@]}"; do echo 0 > "$d/enable" 2>/dev/null || true; done
}
trap cleanup EXIT

echo "BFPERF_DIR=$B"
echo "mode=$mode blocks=${#blocks[@]} interval=${interval}s rounds=$rounds"
echo "events=${labels[*]}"
echo "NOTE: GiB/s@64B assumes one event/flit is 64B; calibrate with STREAM."
printf '%-8s %-18s %-18s %-18s\n' "sample" "events/s" "GiB/s@64B" "GB/s@64B"
prev=$(read_sum)
for ((sample=1; sample<=rounds; sample++)); do
  sleep "$interval"
  cur=$(read_sum)
  delta=$((cur - prev))
  prev=$cur
  awk -v i="$sample" -v d="$delta" -v t="$interval" 'BEGIN {
    eps=d/t;
    gib=eps*64/1024/1024/1024;
    gb=eps*64/1000/1000/1000;
    printf("%-8d %-18.0f %-18.2f %-18.2f\n", i, eps, gib, gb);
  }'
done
