# Sunshine upstream sync plan: v2026.808.164219

Planning branch: `codex/upstream-sync-v2026.808`

Lumina baseline: `8f41f6f3452d197e034a08eaa86f62daa7ee129b`

Upstream target: `v2026.808.164219` (`25c06d79b54f3d092d3fedd5f5ba44989f394692`)

Intermediate release endpoint: `v2026.726.710`

## Integration model

Lumina and Sunshine have unrelated Git histories. Lumina imported Sunshine as a
new root commit and intentionally removed unsupported platforms. A normal merge
or `--allow-unrelated-histories` merge would reintroduce Linux, Windows,
FreeBSD, packaging, and CI files and would produce an unreviewable conflict
set. This sync must be performed as a staged source port.

Use these rules for every overlapping behavior:

1. If the behavior is equivalent, use Sunshine's implementation and retain an
   upstream commit reference in the Lumina commit message.
2. If both implementations provide distinct behavior, use Sunshine as the
   structural baseline and layer the Lumina-only behavior on top.
3. Preserve Lumina-only macOS functionality, configuration, branding,
   installation, permissions, and diagnostics.
4. Port upstream-only behavior when it applies to the shared server or macOS.
5. Do not reintroduce code that is exclusively for an unsupported platform.
6. Never hand-edit vendored third-party source to emulate an upstream fix.
   Update a dependency snapshot only when an applicable upstream change needs
   it, and do that in a separate commit.

The existing uncommitted `install.sh` change belongs to the user and is outside
this sync. Preserve it throughout the work and do not include it in commits.

## Baseline findings

- `v2026.726.710` contains 184 upstream commits after `v2026.516.143833`.
- `v2026.808.164219` contains another 24 commits.
- There are no exact patch-ID matches between the upstream ranges and Lumina's
  later commits. Several behaviors are present on both sides, but Lumina ported
  or rewrote them, so they require semantic comparison.
- The highest-risk shared files are `video.cpp`, `stream.cpp`, `config.*`,
  `confighttp.cpp`, `nvhttp.cpp`, `input.cpp`, `display_device.*`, and the Web
  UI configuration files.
- The highest-risk macOS files are `display.mm`, `misc.mm`, `microphone.mm`,
  Lumina's replacement `input.mm`, and the macOS CMake/packaging files.
- Sunshine uses submodule pointers while Lumina currently commits dependency
  contents. Raw `third-party/` diffs are therefore not a valid sync guide.

## Feature reconciliation ledger

| Area | Sunshine change | Lumina behavior to preserve | Planned resolution |
| --- | --- | --- | --- |
| VideoToolbox H.264 | Removes `max_ref_frames=1` and enables parallel encoding (`3ee4144a`) | Same behavior plus configurable `MaxFrameDelayCount`, IDR request coalescing, frame timestamps, and conditional performance diagnostics | Use Sunshine's encoder option block as canonical; reapply all Lumina-only controls and timing metadata |
| Encoder teardown/reinit | Null-deref, use-after-free, and capture-reinit fixes (`86a25385`, `7ecd0286`, `e40d355f`) | Lumina asynchronous timestamp tracking and profiler fields overlap the same loop | Port upstream control flow first, then reattach timestamp/profiler data without moving the upstream lifetime checks |
| Display wake/sleep | Wake before display discovery and hold a capture-lifetime assertion (`c9863ebe`) | Session-level remote-user activity, display-sleep assertion, reconnect recovery, and virtual-display cleanup | Combine them: use upstream wake-before-enumeration behavior and retain Lumina's broader session lifecycle and virtual-display cleanup; avoid duplicate assertions |
| macOS display management | New `libdisplaydevice` integration (`fbafc497`) | On-demand `CGVirtualDisplay`, ScreenCaptureKit selection, virtual display preference, and dynamic input targeting | Use upstream physical-display/configuration logic; keep Lumina virtual display creation and choose it before physical-display resolution |
| macOS input | Scroll-speed scaling and right-Alt correction (`be18f2f3`, `07317293`) | Objective-C++ `input.mm`, HID gamepad fast path, virtual-display coordinates, cursor fixes | Manually port upstream input behavior into `input.mm`; never replace it with upstream `input.cpp` |
| Audio | Upstream Core Audio Tap/libdisplaydevice refinements | ScreenCaptureKit system audio, optional Audio Tap experiment, microphone fallback, and missing-device crash fix | Preserve Lumina's backend selection and crash guards; port only upstream helper/lifecycle improvements that apply to both backends |
| RTSP/network | Per-client packet-size limit and 65535 cap (`3a69acef`, `3c54d5ff`) | Lumina pairing/security ports and low-latency control loop | Port the settings and validation; preserve Lumina's 5 ms loop and HID path unless measurements justify changing them |
| Client control | Disconnect only the disabled client (`3a196379`) | Lumina's synchronized access-control implementation | Prefer upstream endpoint/session selection and reapply Lumina branding/config additions |
| Crypto | OpenSSL 4 compatibility (`2c59b2e6`) | Existing Lumina authentication and credential migration | Port upstream compatibility changes without changing stored state formats |
| Logging | Rotate five logs at startup (`7df8c62e`) | Dedicated performance channel that bypasses normal severity only when explicitly compiled and enabled | Use upstream rotation unchanged and retain Lumina's extra logger/channel initialization |
| Web UI | Apps filtering/modals, layout uplift, dynamic Clients page, Welcome styling | Lumina branding, macOS-only VideoToolbox controls, virtual-display setting, secret performance setting remaining absent from UI | Take upstream shared pages/components and dependency versions as the base, then reapply only Lumina-specific fields and branding |
| Tray | Qt tray on all platforms (`089f15d4`) | Existing macOS tray behavior and Lumina assets | Because behavior is equivalent, prefer Sunshine's Qt tray; port Lumina identity/assets and validate bundle size, signing, launch, and restart before removing the old tray path |
| macOS packaging | Bonjour usage description and framework signing fixes (`81a84148`, `22f7a773`, `49e56977`) | `vd_helper`, HID entitlements, local install layout, and helper signing | Use upstream signing/filter logic and add Lumina helper/entitlement targets explicitly |
| Dependencies | Moonlight Common, libdisplaydevice, tray, lizardbyte-common, and build-deps updates | Vendored dependency layout and macOS-only build | Update only required snapshots in isolated commits; do not import NVENC-only or unsupported-platform dependency churn |
| NVENC | Dynamic SDK selection (`fb3d85cf`) | Apple Silicon uses VideoToolbox | Do not port NVENC implementation or CPM machinery unless the project restores NVIDIA support |
| Linux/Windows/FreeBSD | Capture, encoder, packaging, installer, and CI changes | Lumina is macOS-only | Exclude platform-only files; port shared abstractions only when required by macOS/common code |

## Ordered implementation

### Phase 1: shared correctness and lifetime fixes

Port the smallest correctness patches before any broad refactor:

- `b91ace72`: validate thread-safe construction errors.
- `e40d355f`: queue draining and capture-reinit freeze fix.
- `7ecd0286`: move the shutdown/reinit check next to encoding to prevent
  packets after teardown.
- `86a25385`: guard encoder flushing when no frame was submitted.
- `2c59b2e6`: OpenSSL 4 compatibility.
- `3a196379`: disconnect only the disabled client.
- `3a69acef` and `3c54d5ff`: packet-size setting and bounds.

For common source files, start from Sunshine's function implementation and
reapply Lumina-only fields and call sites. Add focused tests from upstream with
each behavior instead of deferring tests to the end.

#### Phase 1 review checkpoint

Implemented on 2026-08-10 and left uncommitted for review. All eight listed
upstream changes are represented. Lumina's frame timestamps and conditional
performance diagnostics remain attached to the upstream encoder-loop ordering,
and no dependency or unsupported-platform files changed.

Validation completed:

- Release build with `BUILD_TESTS=ON`, `BUILD_DOCS=OFF`, and performance logging
  disabled.
- Production Web UI build.
- New OpenSSL credential/signature and thread-safe event regression tests.
- 324 of 328 otherwise enabled tests passed. The four failures are existing
  macOS mouse tests affected by the live cursor position/display scaling.
- Two additional config-documentation tests remain excluded because this
  Lumina tree has no `docs/configuration.md`; they report every config option as
  missing. The developer-only performance flag also intentionally remains
  absent from the Web UI.

### Phase 2: macOS streaming and input convergence

1. Reconcile VideoToolbox configuration with `3ee4144a` as the canonical
   implementation.
2. Keep `vt_max_frame_delay`, IDR coalescing, capture timestamps, host
   processing latency, and compile-time performance diagnostics.
3. Port `be18f2f3` and `07317293` into Lumina's `input.mm`.
4. Integrate the useful portions of `c9863ebe` with Lumina's session-level
   power assertions. There must be one owner for every IOPM assertion.
5. Port `fbafc497` physical-display/libdisplaydevice behavior while retaining
   the virtual-display-first selection path.
6. Preserve ScreenCaptureKit video/audio, HID gamepad, issue-5 descriptors,
   virtual display, cursor recovery, and reconnect behavior as explicit
   invariants.

### Phase 3: configuration, Web API, and Web UI

Port the backend and frontend together so the UI never expects a missing API:

- Apps filtering/search and modal edit/delete flows (`3a720151`, `3d9e6d24`).
- Shared UI consistency changes (`3266c341`).
- Do not open the UI automatically at launch (`a84735d1`).
- Dynamic Clients section (`c58fd6d8`).
- Welcome page correction (`e9ed5188`).
- Upgrade Vue, Vue I18n, Vite, date-fns, marked, and migrate
  `lucide-vue-next` to `@lucide/vue` using the versions in
  `v2026.808.164219`.
- Import upstream translations after the English Lumina-only keys are stable.

Retain these Lumina settings and behaviors:

- `virtual_display`, default disabled.
- `vt_max_frame_delay`.
- `max_bitrate` and hot reload behavior.
- Developer-only `streaming_performance_logging`, with no Web UI control.
- Lumina product name, links, icons, config paths, and credentials migration.

### Phase 4: logging, tray, packaging, and build system

1. Port upstream log rotation (`7df8c62e`) and its tests.
2. Migrate to the upstream Qt tray (`089f15d4`) in an isolated commit.
3. Port macOS signing fixes (`22f7a773`, `49e56977`) and Bonjour metadata
   (`81a84148`).
4. Reapply Lumina's `vd_helper`, ScreenCaptureKit, IOKit, HID entitlement,
   helper-signing, and local asset-install rules.
5. Keep `dev.sh` local workflow behavior: documentation off by default and
   performance diagnostics compiled only with `./dev.sh performance`.

Do not replace Lumina's macOS build files wholesale, because upstream does not
know about its virtual display, ScreenCaptureKit, or HID sources.

### Phase 5: dependencies and cleanup

- Update Moonlight Common and libdisplaydevice only after their consumers have
  been ported and tested.
- Update the tray snapshot with the Qt tray migration.
- Add lizardbyte-common only if retained upstream common code requires it.
- Keep NVENC headers/CPM changes out of the macOS-only target.
- Remove obsolete Lumina compatibility code only after the upstream
  replacement passes the same runtime tests.
- Run a final semantic diff against both upstream release ranges and document
  every applicable commit as `ported`, `equivalent`, `combined`, or
  `not applicable`.

## Commit strategy

Keep each phase bisectable and avoid a single sync commit. Suggested commit
sequence:

1. `sync(upstream): port shared stream lifetime fixes`
2. `sync(upstream): port RTSP and client-control fixes`
3. `sync(upstream): reconcile VideoToolbox and macOS power management`
4. `sync(upstream): reconcile macOS display and input implementations`
5. `sync(upstream): update shared Web UI and configuration`
6. `sync(upstream): add rotating logs`
7. `sync(upstream): migrate macOS tray and packaging`
8. `sync(upstream): update required dependency snapshots`
9. `docs(upstream): record v2026.808 sync ledger`

Every sync commit should include the relevant Sunshine commit hashes in its
body. Do not stage or commit the user's existing `install.sh` modification.

## Validation gates

### After every phase

- `git diff --check`
- Configure with `BUILD_DOCS=OFF`.
- Build the `sunshine` target on Apple Silicon.
- Run the tests relevant to the changed subsystem.
- Confirm no unsupported-platform source was accidentally restored.
- Confirm no unrelated vendored dependency content changed.

### Web/API gate

- Production Web UI build.
- Login, logout, CSRF, password change, pairing, unpairing, and client
  enable/disable.
- Saving Web UI configuration must preserve developer-only unknown keys.
- Virtual display defaults to disabled and can be enabled for the next stream.

### macOS runtime gate

- Physical display: start, stop, reconnect, and reconnect after display sleep.
- Virtual display: disabled path, enabled path, multiple resolutions/frame
  rates, creation failure fallback, and destruction after the final session.
- Video: H.264 and HEVC at 1080p60; verify no all-IDR regression, stable
  bitrate, IDR recovery, and `vt_max_frame_delay` behavior.
- Audio: ScreenCaptureKit system audio, Audio Tap experiment, microphone mix,
  missing-device fallback, and a session with no available display.
- Input: cursor visibility, absolute/relative mouse, scroll scaling, every
  left/right modifier, HID controller buttons, both sticks, triggers, and
  issue-5 descriptor behavior.
- Power: connect while the screen is asleep, maintain a long session through
  the configured display timeout, disconnect, and reconnect without local
  mouse movement.
- Logging: normal log levels, five-file startup rotation, performance build
  disabled/enabled, and performance records at `min_log_level = error`.

### Packaging gate

- Fresh `dev.sh` and `dev.sh performance` builds.
- Fresh install without importing state and upgrade while preserving state.
- Verify signatures and entitlements for Lumina, `vd_helper`, and Qt
  frameworks/plugins.
- Launch from the installed path and confirm Web assets, icons, Bonjour,
  Screen Recording, Accessibility, audio, and controller behavior.

## Completion criteria

The sync is complete when all applicable changes through
`v2026.808.164219` are represented in the ledger, Lumina-only macOS behavior is
retained, equivalent implementations use Sunshine's version, all validation
gates pass, and the branch contains no accidental platform resurrection or
unrelated user changes.
