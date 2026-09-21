#!/usr/bin/env bash
set -euo pipefail

package_root="$(cd "$1" && pwd -P)"
validation_home="$2"
unrelated_cwd="$(mktemp -d "${RUNNER_TEMP}/portable cwd.XXXXXX")"
installer="${package_root}/install-lumen-fusion.command"
cd "$package_root"
test -x bin/lumina
test -x bin/vd_helper
test -x launch-lumen-fusion.command
test -x install-lumen-fusion.command
test -s LICENSE
test -s hid_entitlements.plist
test -s assets/apps.json
test -s assets/web/index.html
test -s bin/qt.conf
test -s bin/PlugIns/platforms/libqcocoa.dylib
test -d bin/Frameworks
for library in Core Gui Widgets Svg; do
  test -n "$(find bin/Frameworks \( -name "Qt${library}*" -o -name "libQt6${library}*" \) -print -quit)"
done
while IFS= read -r -d '' binary; do
  if ! file -b "$binary" | grep -q 'Mach-O'; then
    continue
  fi
  lipo "$binary" -verify_arch arm64
  codesign --verify --strict --verbose=2 "$binary"
  # Only system absolute dependencies may remain after relocation.
  otool -L "$binary" | tail -n +2 | while IFS= read -r dependency; do
    dependency="${dependency%% (compatibility version*}"
    dependency="${dependency#"${dependency%%[![:space:]]*}"}"
    case "$dependency" in
      /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*) ;;
      *) echo "Unbundled dependency in $binary: $dependency" >&2; exit 1 ;;
    esac
  done
done < <(find . -type f -print0)
env HOME="$validation_home" ./launch-lumen-fusion.command --help

sentinel="${validation_home}/.config/lumina/portable-validation-sentinel"
mkdir -p "$(dirname "$sentinel")"
printf 'Existing Lumina configuration\n' > "$sentinel"
cd "$unrelated_cwd"
for phase in install reinstall; do
  echo "Validating portable $phase"
  env HOME="$validation_home" "$installer"
  cmp <(printf 'Existing Lumina configuration\n') "$sentinel"
  test -x "${validation_home}/.local/bin/lumen-fusion"
  env HOME="$validation_home" PATH="${validation_home}/.local/bin:${PATH}" lumen-fusion --help
done
