#!/bin/bash
# Run the installed app's isolated encoder benchmark; never edit host settings.
set -u
if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo 'This benchmark requires Apple Silicon macOS.' >&2
  exit 2
fi

here="$(cd "$(dirname "$0")" && pwd)"
app=""
output=""
repeat=1
extra=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app|--output|--repeat)
      [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
      case "$1" in
        --app) app="$2" ;;
        --output) output="$2" ;;
        --repeat) repeat="$2" ;;
      esac
      shift 2 ;;
    --width|--height|--fps|--bitrate|--frames|--warmup)
      [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
      extra+=("$1" "$2"); shift 2 ;;
    --help|-h)
      printf '%s\n' \
        'Lumen Fusion encoder comparison (not capture/network/end-to-end latency)' \
        'Usage: bash benchmark-latency.command [--app APP] [--output NEW_DIRECTORY]' \
        '       [--repeat N] [--width N --height N --fps N --bitrate BITS_PER_SECOND]' \
        '       [--frames N --warmup N]' \
        'Defaults: 1600x1112, 60 FPS, 20000000 bits/s, 180 measured + 30 warmup frames.' \
        'Disconnect active streams and stop other encoders for comparable results.' \
        'An idle host may stay open to preserve an adaptive desktop. No configuration is changed.'
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$repeat" in ''|*[!0-9]*) echo '--repeat must be 1..100' >&2; exit 2 ;; esac
[[ ${#repeat} -le 3 && "$repeat" -ge 1 && "$repeat" -le 100 ]] || { echo '--repeat must be 1..100' >&2; exit 2; }

if [[ -z "$app" ]]; then
  for candidate in "$here/../.." "$here/Lumen Fusion.app" '/Applications/Lumen Fusion.app' "$HOME/Applications/Lumen Fusion.app"; do
    if [[ -x "$candidate/Contents/MacOS/Lumen Fusion" && -f "$candidate/Contents/Resources/benchmark-latency.command" ]]; then
      app="$candidate"
      break
    fi
  done
fi
binary="$app/Contents/MacOS/Lumen Fusion"
[[ -n "$app" && -x "$binary" && -f "$app/Contents/Resources/benchmark-latency.command" ]] || {
  echo 'A benchmark-enabled Lumen Fusion.app was not found. Install the new build or pass --app.' >&2
  exit 2
}
"$binary" --benchmark --help >/dev/null || { echo 'This app does not support the benchmark command.' >&2; exit 2; }

umask 077
if [[ -z "$output" ]]; then
  base="$HOME/Desktop"
  [[ -d "$base" ]] || base="$HOME"
  output="$(mktemp -d "$base/Lumen-Fusion-Benchmark.XXXXXX")" || exit 2
else
  [[ ! -e "$output" && ! -L "$output" ]] || { echo 'Output directory already exists; use a new path.' >&2; exit 2; }
  parent="$(cd "$(dirname "$output")" && pwd -P)" || exit 2
  output="$parent/$(basename "$output")"
  case "$output/" in *.app/*) echo 'Results must be outside application bundles.' >&2; exit 2 ;; esac
  mkdir "$output" || exit 2
fi
output="$(cd "$output" && pwd -P)"
case "$output/" in *.app/*) echo 'Results must be outside application bundles.' >&2; exit 2 ;; esac

printf '%s\n' \
  'Disconnect active streams and stop other encoders for comparable measurements.' \
  'This test does not stop the host or change configuration, pairing, login items or display layout.' \
  'Timings cover synthetic source preparation and encoding, NOT complete host processing latency.' \
  "Results: $output"
printf 'case\tcodec\tvariant\tmode\tqos\texit_code\n' > "$output/cases.tsv"
failed=0
run_case() {
  local name="$1" codec="$2" variant="$3" mode="$4" qos="$5" rc
  echo "--- $name ---"
  "$binary" --benchmark --codec "$codec" --variant "$variant" "$mode" --qos "$qos" \
    --repeat "$repeat" --output "$output/$name" ${extra[@]+"${extra[@]}"} > "$output/$name.log" 2>&1
  rc=$?
  cat "$output/$name.log"
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$codec" "$variant" "$mode" "$qos" "$rc" >> "$output/cases.tsv"
  [[ $rc -eq 0 ]] || failed=1
}
for codec in h264 hevc; do
  run_case "$codec-baseline" "$codec" baseline --paced user-initiated
  run_case "$codec-auto" "$codec" auto --paced user-initiated
  run_case "$codec-auto-poweroff" "$codec" auto-poweroff --paced user-initiated
  run_case "$codec-speedoff" "$codec" speedoff --paced user-initiated
  run_case "$codec-unpaced" "$codec" baseline --unpaced user-initiated
done
run_case h264-cavlc h264 h264-cavlc --paced user-initiated
run_case h264-inherit-qos h264 baseline --paced inherit
if [[ $failed -ne 0 ]]; then
  echo 'Some cases failed; their logs and exit codes are retained. Do not treat failed cases as performance results.' >&2
else
  echo 'All cases completed with packet and native-decode checks. Compare actual FPS and tail latency as well as p50.'
fi
echo "Results: $output"
exit "$failed"
