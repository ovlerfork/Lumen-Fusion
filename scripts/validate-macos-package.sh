#!/usr/bin/env bash
set -euo pipefail

app="$(cd "$1" && pwd -P)"
diagnostics="$2"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
mkdir -p "$diagnostics"
diagnostics="$(cd "$diagnostics" && pwd -P)"
exec > >(tee "$diagnostics/bundle-validation.log") 2>&1

test "$(uname -s)" = Darwin
test "$(uname -m)" = arm64
echo 'Native validation OS (no claim of testing older deployment targets):'
sw_vers
uname -m
echo 'Ad-hoc signed payload: no Developer-ID trust or notarization is asserted.'
contents="$app/Contents"
plist="$contents/Info.plist"
plutil -lint "$plist"
plist_value() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist"; }
test "$(plist_value CFBundleIdentifier)" = org.ovlerfork.LumenFusion
test "$(plist_value CFBundleExecutable)" = 'Lumen Fusion'
test "$(plist_value CFBundleName)" = 'Lumen Fusion'
test "$(plist_value CFBundlePackageType)" = APPL
test "$(plist_value LSUIElement)" = true
test -n "$(plist_value CFBundleVersion)"
test -n "$(plist_value CFBundleShortVersionString)"
test "$(plist_value CFBundleIconFile)" = sunshine.icns
test -s "$contents/Resources/sunshine.icns"
plutil -p "$plist"
for binary in 'Lumen Fusion' vd_helper; do
  test -x "$contents/MacOS/$binary"
  file -b "$contents/MacOS/$binary" | grep -q 'Mach-O'
done
for resource in apps.json web/index.html web/welcome.html qt.conf; do
  test -s "$contents/Resources/$resource"
done
for plugin in platforms/libqcocoa.dylib imageformats/libqsvg.dylib iconengines/libqsvgicon.dylib; do
  test -s "$contents/PlugIns/$plugin"
done
test -d "$contents/Frameworks"
for library in Core Gui Widgets Svg; do
  test -n "$(find "$contents/Frameworks" \( -name "Qt${library}*" -o -name "libQt6${library}*" \) -print -quit)"
done
codesign --verify --deep --strict --verbose=2 "$app"
codesign --display --verbose=4 "$app"
while IFS= read -r -d '' binary; do
  if ! file -b "$binary" | grep -q 'Mach-O'; then
    continue
  fi
  echo "Inspecting $binary"
  lipo "$binary" -verify_arch arm64
  codesign --verify --strict --verbose=2 "$binary"
  minimums="$(otool -arch arm64 -l "$binary" | awk '
    /cmd LC_BUILD_VERSION/ { build=1 }
    /cmd LC_VERSION_MIN_MACOSX/ { legacy=1 }
    build && /minos/ { print $2; build=0 }
    legacy && /version/ { print $2; legacy=0 }
  ')"
  test -n "$minimums"
  echo "arm64 minimum macOS version(s): $minimums"
  otool -arch arm64 -L "$binary" | tail -n +2 | while IFS= read -r dependency; do
    dependency="${dependency%% (compatibility version*}"
    dependency="${dependency#"${dependency%%[![:space:]]*}"}"
    case "$dependency" in
      /System/Library/*|/usr/lib/*|@executable_path/*|@loader_path/*|@rpath/*) ;;
      *) echo "Unbundled dependency in $binary: $dependency" >&2; exit 1 ;;
    esac
  done
done < <(find "$contents" -type f -print0)
python3 "$script_dir/validate-macos-startup.py" "$app" "$diagnostics"
