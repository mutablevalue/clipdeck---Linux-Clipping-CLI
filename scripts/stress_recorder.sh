#!/usr/bin/env bash
set -euo pipefail

CLIPDECK_BIN="${CLIPDECK_BIN:-./build/clipdeck}"
DURATION_SECONDS="${DURATION_SECONDS:-7200}"
SAVE_INTERVAL_SECONDS="${SAVE_INTERVAL_SECONDS:-60}"
SAMPLE_INTERVAL_SECONDS="${SAMPLE_INTERVAL_SECONDS:-30}"
LOG_PATH="${LOG_PATH:-output/runtime/stress-recorder.log}"

mkdir -p "$(dirname "$LOG_PATH")"

log() {
  printf '[%(%Y-%m-%d %H:%M:%S)T] %s\n' -1 "$*" | tee -a "$LOG_PATH"
}

rss_kb_for_pid() {
  local pid="$1"
  ps -o rss= -p "$pid" 2>/dev/null | awk '{print $1}'
}

segment_count() {
  find output/runtime/segments -maxdepth 1 -type f -name '*.mp4' 2>/dev/null | wc -l
}

segment_bytes() {
  find output/runtime/segments -maxdepth 1 -type f -name '*.mp4' -printf '%s\n' 2>/dev/null |
    awk '{sum += $1} END {print sum + 0}'
}

zombie_media_process_count() {
  ps -eo stat=,comm= |
    awk '$1 ~ /Z/ && ($2 == "ffmpeg" || $2 == "ffprobe") {count++} END {print count + 0}'
}

if [[ ! -x "$CLIPDECK_BIN" ]]; then
  log "ClipDeck binary is not executable: $CLIPDECK_BIN"
  exit 1
fi

: >"$LOG_PATH"
log "Starting ClipDeck recorder stress test."
log "duration_seconds=$DURATION_SECONDS save_interval_seconds=$SAVE_INTERVAL_SECONDS sample_interval_seconds=$SAMPLE_INTERVAL_SECONDS"

"$CLIPDECK_BIN" start
trap '"$CLIPDECK_BIN" stop >/dev/null 2>&1 || true' EXIT
sleep 2

pid_file="${XDG_RUNTIME_DIR:-/tmp}/clipdeck/clipdeck.pid"
if [[ ! -f "$pid_file" ]]; then
  log "ClipDeck pid file was not created: $pid_file"
  exit 1
fi

daemon_pid="$(cat "$pid_file")"
if ! kill -0 "$daemon_pid" 2>/dev/null; then
  log "ClipDeck daemon is not running after start."
  exit 1
fi

started_at="$(date +%s)"
next_save="$started_at"
next_sample="$started_at"
max_rss_kb=0
min_rss_kb=0
save_count=0
failure_count=0

while true; do
  now="$(date +%s)"
  elapsed=$((now - started_at))
  if (( elapsed >= DURATION_SECONDS )); then
    break
  fi

  if (( now >= next_save )); then
    if "$CLIPDECK_BIN" save >>"$LOG_PATH" 2>&1; then
      save_count=$((save_count + 1))
    else
      failure_count=$((failure_count + 1))
    fi
    next_save=$((now + SAVE_INTERVAL_SECONDS))
  fi

  if (( now >= next_sample )); then
    rss_kb="$(rss_kb_for_pid "$daemon_pid")"
    rss_kb="${rss_kb:-0}"
    if (( min_rss_kb == 0 || rss_kb < min_rss_kb )); then
      min_rss_kb="$rss_kb"
    fi
    if (( rss_kb > max_rss_kb )); then
      max_rss_kb="$rss_kb"
    fi
    log "sample elapsed_seconds=$elapsed rss_kb=$rss_kb segments=$(segment_count) segment_bytes=$(segment_bytes) zombie_media_processes=$(zombie_media_process_count)"
    next_sample=$((now + SAMPLE_INTERVAL_SECONDS))
  fi

  sleep 1
done

zombies="$(zombie_media_process_count)"
log "summary saves=$save_count failures=$failure_count min_rss_kb=$min_rss_kb max_rss_kb=$max_rss_kb segments=$(segment_count) segment_bytes=$(segment_bytes) zombie_media_processes=$zombies"

if (( zombies > 0 )); then
  log "Stress test failed: zombie ffmpeg/ffprobe processes were found."
  exit 1
fi

log "Stress test completed."
