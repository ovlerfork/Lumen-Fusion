# Temporary streaming performance diagnostics

Lumina currently includes temporary `PERF_VIDEO`, `PERF_CLIENT_LOSS`, and
`PERF_CLIENT_DISCONNECT` diagnostics for investigating poor gameplay streaming.
They are disabled at runtime by default and are independent of the normal log
level.

## Enable a test session

The profiler is compiled out by default. Build and install a diagnostic binary
with:

```bash
./dev.sh performance
```

This is intentionally a developer-only setting and is not exposed in the Web
UI. Add this directly to `~/.config/lumina/sunshine.conf`:

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

Disable the setting after the test. When disabled, Lumina does not call the
per-stage clocks or update the aggregation windows. The small per-packet timing
fields remain present until the diagnostics are compiled out.

## `PERF_VIDEO` fields

A summary is emitted for each active session every five seconds:

| Field | Meaning |
|---|---|
| `frames`, `output_fps` | Encoded frames sent by the host |
| `source_frames`, `source_fps` | Newly captured ScreenCaptureKit frames |
| `duplicate_frames` | Cached ScreenCaptureKit frames plus host minimum-FPS repeats |
| `idr_frames` | Keyframes actually produced by the encoder |
| `idr_requests` | Recovery keyframes requested by Moonlight |
| `process_cpu_pct` | Process CPU time divided by wall time; may exceed 100% on multicore systems |
| `payload_mbps` | Encoded video payload rate before packet headers/FEC |
| `wire_mbps` | Host video rate including packet headers, encryption prefixes, and FEC |
| `data_shards`, `parity_shards` | Video packet and FEC packet counts |

Each duration has `avg`, `p50`, `p95`, and `max` values:

| Duration | Measurement boundary |
|---|---|
| `capture_queue_ms` | Capture callback to encoder-thread dequeue |
| `convert_ms` | Pixel conversion or hardware-frame preparation |
| `encode_ms` | Encoder submission to matching encoded-packet availability |
| `broadcast_queue_ms` | Encoded packet availability to broadcast-thread dequeue |
| `fec_ms` | Reed-Solomon generation accumulated across the frame |
| `pacing_ms` | Deliberate intra-frame rate-control sleep |
| `send_ms` | Socket send calls accumulated across the frame |
| `network_ms` | Broadcast dequeue through the final send |
| `capture_to_send_ms` | New capture callback through final server send |

VideoToolbox output is asynchronous. Lumina therefore matches submit and ready
timestamps by packet PTS; it does not assume that a packet returned by
`avcodec_receive_packet()` belongs to the frame submitted by the current call.

`PERF_CLIENT_LOSS` mirrors Moonlight's packet-loss report with its interval and
last good frame. A high `idr_requests` rate or repeated loss reports points to
the network/client recovery path even when host send latency is low.
`PERF_CLIENT_DISCONNECT` confirms that the ENet control peer disconnected and
records the session state at that moment.

## Removing or compiling it out

All hot-path sections are bracketed by
`LUMINA_STREAM_PERF_DIAGNOSTICS_BEGIN/END` comments and guarded by
`LUMINA_ENABLE_STREAM_PERF_LOGGING`.

The CMake option defaults to `OFF`. A normal development build explicitly sets
it to `OFF`, so a prior performance build cannot leave it enabled in the CMake
cache:

```bash
./dev.sh
```

For a manual diagnostic build, configure with
`-DLUMINA_ENABLE_STREAM_PERF_LOGGING=ON`. With the option set to `OFF`, the
runtime `streaming_performance_logging` setting has no effect.

Once the cause is known, remove the marked blocks plus the
`streaming_performance_logging` config setting and `performance` logger.

## Earlier baseline

The earlier 1920x1080 HEVC test requested 60 FPS and showed 40.67 new source
frames per second, 57.85 ms average encoder latency, and 82.88 ms p95 encoder
latency, while capture queue, conversion, broadcast queue, and FEC were below
0.1 ms average. Gameplay testing is needed because the previous sample did not
represent the high-motion workload that currently performs poorly.
