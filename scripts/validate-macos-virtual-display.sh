#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd "$script_dir/.." && pwd -P)"
helper="${1:?expected built vd_helper path}"
diagnostics="${DIAGNOSTICS:?expected diagnostics directory}/virtual-display"
mkdir -p "$diagnostics"
exec > >(tee "$diagnostics/native-display-lifecycle.log") 2>&1

if [[ "${GITHUB_ACTIONS:-}" != true || "$(uname -s)" != Darwin ]]; then
  echo 'Native display validation requires a disposable GitHub Actions Darwin session.' >&2
  exit 1
fi
test -x "$helper"
sw_vers
xcrun clang --version

test_dir="$(mktemp -d "${RUNNER_TEMP:?}/native-display.XXXXXX")"
trap 'rm -rf "$test_dir"' EXIT
cp "$helper" "$test_dir/vd_helper"

# The harness includes the current production wrapper with three observation
# substitutions; the built helper and all remaining CoreGraphics calls are native.
harness="$repo_root/tests/integration/native_display_lifecycle.m"
xcrun clang -fobjc-arc -fblocks -I"$repo_root/src/platform/macos" \
  -DNATIVE_WRAPPER_OBJECT -c "$harness" -o "$test_dir/virtual_display.o"
xcrun clang -fobjc-arc -fblocks -I"$repo_root/src/platform/macos" \
  "$test_dir/virtual_display.o" "$harness" \
  -framework Foundation -framework AppKit -framework CoreGraphics \
  -o "$test_dir/native_display_lifecycle"
"$test_dir/native_display_lifecycle" --disposable-windowserver
