#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 BASELINE_URL CANDIDATE_URL [endpoint]" >&2
  exit 2
fi

baseline="$1"
candidate="$2"
endpoint="${3:-/api/health}"
rounds="${ROUNDS:-5}"
requests="${REQUESTS_PER_ROUND:-}"
duration_sec="${DURATION_SEC:-60}"
concurrency="${CONCURRENCY:-10}"
warmup="${WARMUP_REQUESTS:-20}"
baseline_revision="${BASELINE_REVISION:-unknown}"
candidate_revision="${CANDIDATE_REVISION:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}"
baseline_pid="${BASELINE_PID:-}"
candidate_pid="${CANDIDATE_PID:-}"
out="${OUTPUT:-bench/ab-$(date +%Y%m%d-%H%M%S).tsv}"
mkdir -p "$(dirname "$out")"
printf 'revision\tround\trequest\thttp_code\ttime_total\tcpu_pct\trss_kb\tfd_count\n' > "$out"

sample_process() {
  local pid="$1"
  if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
    printf '%s\t%s\t%s' '-1' '-1' '-1'
    return
  fi
  local cpu rss fd_count
  read -r cpu rss < <(ps -p "$pid" -o '%cpu=,rss=')
  if [[ -d "/proc/$pid/fd" ]]; then
    fd_count=$(find "/proc/$pid/fd" -maxdepth 1 -type l 2>/dev/null | wc -l | tr -d ' ' || true)
  else
    fd_count=$(lsof -p "$pid" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ' || true)
  fi
  printf '%s\t%s\t%s' "${cpu:--1}" "${rss:--1}" "${fd_count:--1}"
}

warmup_side() {
  local base="$1" i
  for ((i=1; i<=warmup; i++)); do
    curl -sS -o /dev/null --max-time "${CURL_TIMEOUT_SEC:-10}" \
      "${base%/}${endpoint}" >/dev/null || true
  done
}

run_round() {
  local label="$1" base="$2" round="$3" pid="$4" i result code elapsed
  local process_metrics cpu rss fd_count
  local tmp="${out}.${label}.${round}.tmp"
  : > "$tmp"
  if [[ -n "$requests" ]]; then
    for ((i=1; i<=requests; i++)); do
      (
        result=$(curl -sS -o /dev/null --max-time "${CURL_TIMEOUT_SEC:-10}" \
          -w '%{http_code}\t%{time_total}' "${base%/}${endpoint}" || printf '000\t-1')
        code="${result%%$'\t'*}"
        elapsed="${result#*$'\t'}"
        printf '%s\t%s\t%s\t%s\t%s\n' \
          "$label" "$round" "$i" "$code" "$elapsed" >> "$tmp"
      ) &
      if (( i % concurrency == 0 )); then wait; fi
    done
  else
    local deadline=$((SECONDS + duration_sec))
    i=0
    while (( SECONDS < deadline )); do
      i=$((i + 1))
      (
        result=$(curl -sS -o /dev/null --max-time "${CURL_TIMEOUT_SEC:-10}" \
          -w '%{http_code}\t%{time_total}' "${base%/}${endpoint}" || printf '000\t-1')
        code="${result%%$'\t'*}"
        elapsed="${result#*$'\t'}"
        printf '%s\t%s\t%s\t%s\t%s\n' \
          "$label" "$round" "$i" "$code" "$elapsed" >> "$tmp"
      ) &
      if (( i % concurrency == 0 )); then wait; fi
    done
  fi
  wait
  process_metrics=$(sample_process "$pid")
  cpu="${process_metrics%%$'\t'*}"
  process_metrics="${process_metrics#*$'\t'}"
  rss="${process_metrics%%$'\t'*}"
  fd_count="${process_metrics#*$'\t'}"
  awk -v OFS='\t' -v cpu="$cpu" -v rss="$rss" -v fd="$fd_count" \
    '{print $0, cpu, rss, fd}' "$tmp" >> "$out"
  rm -f "$tmp"
}

echo "controlled A/B: rounds=$rounds duration_sec=$duration_sec requests_per_round=${requests:-unbounded} concurrency=$concurrency warmup=$warmup endpoint=$endpoint"
printf 'baseline_revision=%s\ncandidate_revision=%s\n' "$baseline_revision" "$candidate_revision" >&2
warmup_side "$baseline"
warmup_side "$candidate"
for ((round=1; round<=rounds; round++)); do
  # Alternate at round granularity so time-dependent load affects both sides.
  if (( round % 2 == 1 )); then
    run_round baseline "$baseline" "$round" "$baseline_pid"
    run_round candidate "$candidate" "$round" "$candidate_pid"
  else
    run_round candidate "$candidate" "$round" "$candidate_pid"
    run_round baseline "$baseline" "$round" "$baseline_pid"
  fi
done
awk -F '\t' 'NR>1 && $5 >= 0 { n[$1]++; if ($4 ~ /^2/) ok[$1]++; sum[$1]+=$5; values[$1, n[$1]]=$5 }
  END { for (k in n) { m=n[k]; for (i=1;i<=m;i++) a[i]=values[k,i]; for (i=1;i<=m;i++) for (j=i+1;j<=m;j++) if (a[j]<a[i]) {t=a[i];a[i]=a[j];a[j]=t}; p95=a[int(m*0.95)+1]; p99=a[int(m*0.99)+1]; printf "%s requests=%d success=%.2f%% avg_ms=%.3f p95_ms=%.3f p99_ms=%.3f\n", k,m,100*ok[k]/m,1000*sum[k]/m,1000*p95,1000*p99} }' "$out"
echo "results=$out"
