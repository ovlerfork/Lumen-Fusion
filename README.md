<div align="center">
  <img src="sunshine.png" alt="Lumen Fusion icon" width="256" />
  <h1>Lumen Fusion</h1>
  <p>Native macOS game streaming for Moonlight clients.</p>
  <img src="lumina.png" alt="Lumen Fusion screenshot" width="512" />
</div>

Lumen Fusion is a macOS-focused descendant of [Lumen](https://github.com/trollzem/Lumen) and [Sunshine](https://github.com/LizardByte/Sunshine). It runs as a menu-bar application, captures the Mac display and system audio through native macOS APIs, and streams to [Moonlight](https://moonlight-stream.org/) clients.

## Install the native macOS app

Requires **Apple Silicon and macOS 15 or later**. The native DMG is the primary installation path. Open the [Lumen Fusion releases page](https://github.com/ovlerfork/Lumen-Fusion/releases) and choose the DMG for the release you intend to install. It does not require Terminal, Homebrew, `sudo`, or a source build.

1. If upgrading, choose **Quit** from the old app's menu bar and wait for it to exit.
2. Open the DMG and drag **Lumen Fusion.app** into **Applications**. In Finder, replace the previous copy when asked.
3. Eject the disk image.
4. Open **Lumen Fusion** from **Applications**.

Lumen Fusion is a menu-bar app, so it may not show a Dock icon or main window. Use its menu-bar icon to open the local administration page in your browser, set the administrator account, and pair a Moonlight client.

Your settings, certificates, and existing Moonlight pairings stay in `~/.config/lumina/`. Replacing the application does not erase that directory. An older command-line installation may still exist; use the copy in **Applications** when checking an upgrade.

### First launch and permissions

Preview builds are **ad-hoc signed, not Developer ID signed or Apple-notarized**. An update can require a new app-specific approval; no system-wide security changes are needed.

If macOS blocks this app because it cannot verify the developer, first verify the download source. Then use this app's **Open Anyway** action in **System Settings → Privacy & Security** and open it again. Do not use broad quarantine-removal commands.

Grant **Screen & System Audio Recording** when macOS asks. It is required for display capture and native system-audio capture. Grant **Accessibility** when you need remote keyboard or mouse control. If macOS asks you to quit and reopen the app after changing a permission, do so; an application update can require permission again.

System audio uses ScreenCaptureKit and is captured as stereo. Leave the audio device unset, or use `audio_sink = system` in `~/.config/lumina/sunshine.conf`. Virtual audio devices such as BlackHole are optional and are not required for native system audio.

### Launch at login

The menu-bar Launch at Login setting is opt-in and off by default. It uses `SMAppService.mainAppService`, macOS's per-user application login item, rather than a system daemon. The menu reflects the system approval status. If macOS reports that approval is required, open the system Login Items settings and approve it there; cancelling leaves it disabled. You can also turn the setting off, and the app does not enable it again by itself.

## Adaptive virtual desktop (opt-in)

In **Configuration → Audio/Video**, enable the virtual display and select **Adaptive** layout. Existing installations keep their previous Extend/Mirror/System behavior until you select this mode.

| Situation | Adaptive behavior |
| --- | --- |
| Another local screen is usable | The remote display is an extension. Its default disconnect action removes it, allowing macOS to return windows to the remaining screen. |
| No other local screen is usable | The virtual display becomes primary. Temporary disconnect stops media but retains the desktop for Resume. |
| A local screen returns while paused | After a short availability check, the local screen becomes primary again and the unused virtual screen is removed under the default policy. |
| A local screen returns while streaming | The virtual screen becomes an extension without ending its active stream. |
| Explicitly end the remote desktop session or quit Lumen Fusion | Retained desktop resources and the app's idle-power assertions are released. |

The local-screen and headless disconnect actions are independently configurable. A retained desktop stays available for Resume until you explicitly end the session or release it, or the configured local-screen policy removes it. The menu-bar **Virtual Desktop…** entry shows its state and allows releasing an unused desktop. No capture, video/audio encoding, or media packet loop is kept running solely for retention; applications and WindowServer may still render their own content.

The power setting can prevent system idle sleep, prevent both display and system idle sleep, or leave idle sleep unrestricted. The selected idle-sleep protection remains active while the desktop is retained.

Display detection excludes this app's virtual screen and checks active/awake local outputs and closed-lid state. Some monitors or docks continue advertising a powered-off panel; **Local display detection → Treat as absent/present** provides an override. Detection is not a guarantee that the panel is physically visible. Windows and full-screen Spaces are managed by macOS; not every application's window placement can be guaranteed.

Resume reuses a still-owned, usable virtual display. If the client negotiates a different resolution or frame rate while the desktop is retained, the desktop keeps its current mode and the media pipeline scales to the requested output. End the desktop session and reconnect to change the desktop's mode.

This feature does **not** unlock the Mac, change password policies, or override manual sleep, lid-close sleep, thermal protection, or low-battery sleep. It is not a replacement for macOS closed-display operating requirements. Idle-sleep prevention and screen-lock policy remain separate.

On macOS virtual machines whose hardware model begins with `VirtualMac`, explicit virtual-display **Mirror** requests are rejected before changing displays. Native mirror transactions reproducibly caused display-list loss and termination of the graphical CI session; the underlying WindowServer cause remains unresolved. An unreadable model also refuses an explicit Mirror request. Adaptive primary/extension transitions do not use mirroring and remain available. Tests on these machines check safe rejection and unchanged ownership/topology, not successful native mirroring; physical-Mac mirror behavior requires separate validation.

## Pair and stream

1. Open the local administration page from the menu bar.
2. Complete the administrator setup if prompted.
3. In Moonlight, discover Lumen Fusion on the local network or add the Mac manually.
4. Enter Moonlight's pairing PIN in the local administration page.
5. Start a stream.

The local administration page listens on `https://localhost:47990`. Moonlight discovery uses the local network; remote access requires appropriate network configuration.

## Configuration

Configuration lives in `~/.config/lumina/`:

| Path | Purpose |
| --- | --- |
| `sunshine.conf` | Streaming, audio, encoder, and network settings |
| `sunshine_state.json` | Local administration state and paired clients |
| `credentials/` | TLS certificates and pairing material |
| `apps.json` | Applications shown to Moonlight |
| `sunshine.log` | Runtime log |

Useful settings include:

```ini
# Native ScreenCaptureKit system audio. Leaving the device unset also uses it.
audio_sink = system

# Optional virtual display behavior.
virtual_display = disabled

# Maximum streaming bitrate, in kbps.
max_bitrate = 80000
```

Use the local administration page for routine configuration. The application preserves existing configuration and pairing data while you replace the `.app` bundle.

## Latency experiments

The macOS **Configuration → VideoToolbox Encoder** and **Audio/Video** tabs expose
reversible experimental settings. Existing defaults remain unchanged:

| Setting | Values and default |
| --- | --- |
| `vt_low_latency_rate_control` | `inherit` (default) keeps the encoder specification; `auto` removes only the low-latency rate-control request, preserving hardware/software constraints. |
| `vt_prio_speed` | `inherit` (default), `enabled`, `disabled`. Inherit keeps the existing speed preference enabled. |
| `vt_power_efficient` | `inherit` (default), `enabled`, `disabled`. Inherit keeps FFmpeg's power-efficiency default. |
| `vt_coder` | `auto` (default), `cabac`, `cavlc`; applies to H.264. |
| `macos_capture_queue_depth` | Integer `3..8`, default `4`; applies to new ScreenCaptureKit video capture sessions. |

Compare one variable at a time with the same scene, client, stream settings, and
network. Apply/restart and start a fresh session for each comparison. Inspect the
actual `EncoderID` and property readback; a requested option may be unsupported
or rejected. These settings retain synchronous VideoToolbox output completion
and do not promise a measured latency improvement.

Enable **Configuration → Advanced → Streaming Performance Logging** for test
sessions. It defaults to off; macOS release builds include support. Timing,
aggregation, and log writes add overhead, so use the same logging setting in
both comparisons and disable it afterward. `capture_to_send` is host-only, not
end-to-end latency. See the [diagnostic fields and test procedure](docs/streaming-performance-logging.md)
for p99, cadence measurements, and the consumed-frame definition of `source_fps`.

### Packaged encoder benchmark

The DMG and the app's `Contents/Resources` include `benchmark-latency.command`.
It runs the **same Release binary and FFmpeg libraries** as streaming, through an
isolated `--benchmark` entry before normal host initialization. No Homebrew,
compiler, Python, configuration edit, pairing reset, virtual display, or capture
permission is required by this synthetic test. Disconnect active streams and stop
other encoders first for comparable measurements; an idle host may remain open
so an adaptive desktop is not discarded.

```bash
bash "/Applications/Lumen Fusion.app/Contents/Resources/benchmark-latency.command" --repeat 2
```

Results are saved to a new `Lumen-Fusion-Benchmark.*` directory on the Desktop
(or in the home directory if Desktop is absent). Each case has a log, raw CSV,
and JSON summary; `cases.tsv` retains each exit code. The matrix tests both codecs
with baseline, automatic selection, automatic selection plus power preference
disabled, speed preference disabled, and unpaced baseline; it also tests H.264
CAVLC and inherited thread QoS. Defaults are 1600×1112, 60 FPS, 20 Mbps,
30 warmup plus 180 measured frames. Width, height, FPS, bitrate (bits/second),
warmup, frame count and repeat count can be supplied explicitly.

Use `--max-frame-delay N` to request a delay for all 12 cases, or
`--suite frame-delay --fps 120 --frames 1000 --warmup 600 --repeat 2` for a
targeted HEVC auto sweep at -1 (inherit), 0, 1 and 2 frames. These options cannot
be combined. Setter status and property readback distinguish accepted requests
from unsupported or rejected requests; measured maxima are not latency guarantees.
The native CLI supports H.264 and baseline frame-delay experiments separately.

Read existing results, including 0.0.40 JSON, without running the encoder:

```bash
bash "/Applications/Lumen Fusion.app/Contents/Resources/benchmark-latency.command" --summarize "$HOME/Desktop/Lumen-Fusion-Benchmark.EXAMPLE"
```

This read-only mode uses macOS `plutil` and needs no installed app. It prints
requested/actual FPS, measured count, encoder p50/p95/p99/max, preparation and
wake-lateness maxima, packet-gap maximum, skipped ticks, and separate cold-frame
and warmup timings. Missing values appear as `N/A`; encoder latency and packet
intervals are separate columns.

A single comparison can also be run directly, using a new output file prefix:

```bash
"/Applications/Lumen Fusion.app/Contents/MacOS/Lumen Fusion" --benchmark --codec h264 --variant auto --output "$HOME/Desktop/lumen-auto"
```

It writes `lumen-auto-r1.csv` and `lumen-auto-r1.json` after timing. Existing results
are not overwritten. `--benchmark --help` lists the available options. The default
USER_INITIATED thread QoS matches the application's encode-thread policy; inherited
QoS is a measurement control, not a newly enabled streaming optimization.

Warmup and the first frame are reported separately. Raw records include preparation,
sleep deadlines/wake times, submission, output, missed source ticks, PTS and bytes.
The report records the actual encoder/property readback and effective output FPS.
Native VideoToolbox decoding then checks all encoded frames, dimensions and PTS
**outside the timed pass**. This does not measure image quality.

A value near 5 ms in this tool means **synthetic submission-to-packet latency**, not
5 ms host processing or end-to-end latency. Confirm any promising choice with the
real-stream diagnostics above, including 60 FPS cadence, p95/p99, drops and visual
quality. Capture queue changes require that real-stream test. No experimental
choice is automatically saved or made the default.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| No image or system audio | Grant Screen & System Audio Recording, then quit and relaunch if macOS requests it. |
| Remote keyboard or mouse does not work | Grant Accessibility, then relaunch if requested. |
| macOS says the developer cannot be verified | Verify the download source, then use this app's Open Anyway action in Privacy & Security. |
| The old build still starts | Launch `/Applications/Lumen Fusion.app`; an old command-line installation may still be on your PATH. |
| Moonlight cannot pair | Open the local administration page from the menu bar and enter the PIN shown by Moonlight. |

For a defect report, include the macOS version, app build or commit, client type, and the relevant part of `~/.config/lumina/sunshine.log` with any credentials removed.

## Historical technical notes

The repository keeps its source and upstream lineage for developers and contributors. These notes describe implementation history; they are not installation requirements or current performance guarantees:

- [Lumen Fusion incremental changes and provenance (Chinese)](docs/LUMEN_FUSION_OPTIMIZATIONS.zh-CN.md)
- [Streaming-performance diagnostic format](docs/streaming-performance-logging.md)
- [Lumen](https://github.com/trollzem/Lumen) and [Sunshine](https://github.com/LizardByte/Sunshine) upstream projects

## License

Lumen Fusion is licensed under the same terms as Sunshine (GPLv3). See [LICENSE](LICENSE).
