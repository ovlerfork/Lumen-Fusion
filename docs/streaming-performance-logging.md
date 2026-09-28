# Temporary streaming performance diagnostics

Lumina currently includes temporary `PERF_VIDEO`, `PERF_CLIENT_LOSS`, and
`PERF_CLIENT_DISCONNECT` diagnostics for investigating poor gameplay streaming.
They are disabled at runtime by default and are independent of the normal log
level.

## Enable a test session

macOS release builds include performance logging support. In the Web UI, open
**Configuration → Advanced**, enable **Streaming Performance Logging**, and
use **Apply** to save and restart. The runtime setting defaults to `false`.

For a development build, build and install diagnostic support with:

```bash
./dev.sh performance
```

You can also set the same option in `~/.config/lumina/sunshine.conf`:

```ini
streaming_performance_logging = enabled
```

Restart Lumina, reproduce the problem for at least 60 seconds, and then extract
the records:

```bash
rg 'PERF_VIDEO|PERF_CLIENT_LOSS|PERF_CLIENT_DISCONNECT' ~/.config/lumina/sunshine.log
```

The diagnostics use a dedicated `Performance` log channel (internal severity
7), so they are written when `min_log_level = error` or even
`min_log_level = none`. No unrelated debug logging is required.

Enabling diagnostics adds timing, aggregation, and log I/O overhead; keep it
enabled consistently in both sides of a comparison. Disable the setting and
apply/restart after the test. When disabled, Lumina does not call the
per-stage clocks or update the aggregation windows. The small per-packet timing
fields remain present until the diagnostics are compiled out.

## `PERF_VIDEO` fields

A summary is emitted when a frame reaches the sender after at least five
seconds in the current window. `interval_s` reports the actual elapsed time;
counts and rates describe that window, not the whole session.

| Field | Meaning |
|---|---|
| `session`, `peer` | Streaming session ID and client address |
| `width`, `height`, `requested_fps`, `requested_kbps`, `codec` | Negotiated stream settings |
| `interval_s` | Actual aggregation-window duration in seconds |
| `frames`, `output_fps` | Encoded frames sent by the host |
| `source_frames`, `source_fps` | Consumed, non-repeated captured frames that reached the sender, and their rate; not every ScreenCaptureKit callback |
| `duplicate_frames` | Cached ScreenCaptureKit frames plus host minimum-FPS repeats |
| `idr_frames` | Keyframes actually produced by the encoder |
| `idr_requests` | Recovery keyframes requested by Moonlight |
| `process_cpu_pct` | Process CPU time divided by wall time; may exceed 100% on multicore systems |
| `payload_mbps` | Encoded video payload rate before packet headers/FEC |
| `wire_mbps` | Host video rate including packet headers, encryption prefixes, and FEC |
| `data_shards`, `parity_shards` | Video packet and FEC packet counts |
| `encode_over_budget` | Frames whose measured encode duration exceeded `1000 / requested_fps` milliseconds |
| `percentile_method` | `reservoir512`: at most 512 samples retained across each window |

Each duration has `avg`, `p50`, `p95`, `p99`, and `max` values, for example
`encode_ms_p99`. Percentiles use all observations up to 512 samples, then a
reservoir spanning the window. Average and maximum include every observation.
An empty sample set reports zero, which does not establish zero latency.

| Duration | Measurement boundary |
|---|---|
| `used_capture_gap_ms` | Gap between capture timestamps of consecutive consumed, non-repeated frames that reached the sender |
| `encode_output_gap_ms` | Gap between matching encoded-packet availability timestamps of consecutive frames reaching the sender |
| `send_complete_gap_ms` | Gap between consecutive host frame-send completions |
| `capture_queue_ms` | Capture callback to encoder-thread dequeue |
| `convert_ms` | Pixel conversion or hardware-frame preparation |
| `encode_ms` | Encoder submission to matching encoded-packet availability |
| `broadcast_queue_ms` | Encoded packet availability to broadcast-thread dequeue |
| `fec_ms` | Reed-Solomon generation accumulated across the frame |
| `pacing_ms` | Deliberate intra-frame rate-control sleep |
| `send_ms` | Socket send calls accumulated across the frame |
| `network_ms` | Broadcast dequeue through the final send |
| `capture_to_send_ms` | Consumed new capture timestamp through final host send |

The three gap measurements retain their preceding timestamp across reporting
windows. They describe host cadence, not packet loss or the client's pacing-drop
statistic. Capture callbacks discarded before encoding are not counted in
`source_fps`.

`capture_to_send_ms` is **host-only**, from a consumed new capture's timestamp
through the final host send. It excludes client transit, decoding, presentation,
and display latency, so it is not an end-to-end measurement.

VideoToolbox uses output callbacks. Lumina retains synchronous
`VTCompressionSessionCompleteFrames` completion for submitted frames before
FFmpeg polls its output queue. Submit and ready timestamps are matched by packet
PTS; a received packet is not assumed to belong to the current submission.

`PERF_CLIENT_LOSS` mirrors Moonlight's packet-loss report with its interval and
last good frame. A high `idr_requests` rate or repeated loss reports points to
the network/client recovery path even when host send latency is low.
`PERF_CLIENT_DISCONNECT` confirms that the ENet control peer disconnected and
records the session state at that moment.

## Comparing latency settings

Start from the unchanged defaults in the README's
[latency experiments](../README.md#latency-experiments) section. Change one
variable at a time, apply/restart, and create a fresh streaming session. Keep the
client, scene or gameplay sequence, resolution, FPS, bitrate, codec, and network
the same. Compare multiple windows, including p95/p99, frame cadence, source and
output FPS, CPU usage, and client observations. These settings do not guarantee
a latency improvement.

For encoder comparisons, retain the requested selection log and actual
`EncoderID`/hardware-encoder log. Check property readback: a requested FFmpeg
option alone does not establish that the encoder applied it. Logs distinguish
unsupported or rejected properties, verified values, and unavailable or
differing readback. Encoder-selection and property messages use normal log
levels, so keep informational logging enabled when collecting them. The latency
settings retain the synchronous output-completion fix.

## Build-time support

All hot-path sections are bracketed by
`LUMINA_STREAM_PERF_DIAGNOSTICS_BEGIN/END` comments and guarded by
`LUMINA_ENABLE_STREAM_PERF_LOGGING`.

The CMake option defaults to `OFF`, while the macOS release build script sets it
to `ON`. A normal development build explicitly sets it to `OFF`, so a prior
performance build cannot leave it enabled in the CMake cache:

```bash
./dev.sh
```

For a manual diagnostic build, configure with
`-DLUMINA_ENABLE_STREAM_PERF_LOGGING=ON`. With the option set to `OFF`, the
runtime `streaming_performance_logging` setting has no effect.

## Earlier baseline

The earlier 1920x1080 HEVC test requested 60 FPS and showed 40.67 consumed new
source frames per second, 57.85 ms average encoder latency, and 82.88 ms p95 encoder
latency, while capture queue, conversion, broadcast queue, and FEC were below
0.1 ms average. Gameplay testing is needed because the previous sample did not
represent the high-motion workload that currently performs poorly.
