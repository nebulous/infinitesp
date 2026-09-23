# Bus Capture and Diagnostics

Three ways to watch the bus: the JSONL stream (the format to attach to issues), the raw tap (wire-exact, a maintainer and bench tool), and the `STATS` log line (bus health). This page covers all three plus capture recipes for filing issues.

## JSONL Capture Stream (default on, port 2373)

Streams every bus frame as one JSON object per line with timestamps. This is the capture format to attach to issues: it has timing (unlike REPORT?), and is redacted (unlike the raw tap). The port number spells room temperature: 23°C / 73°F.

```bash
nc infinitesp.local 2373 | tee capture.jsonl
```

Each line is one wire frame, like so:

```json
{"ts":"2026-09-13T17:37:54.226Z","src":"20","dst":"52","func":"0B","reg":"0104","data":"564152205350..."}
```

`ts` is ISO8601 UTC, taken from the config's `time:` platform automatically (until the clock syncs, or on time-less configs, lines carry boot-relative `"ms"` instead). `src`/`dst` are bus addresses, `func` is the frame function (`0B` read, `0C` write, `06` reply), `reg` is the register number, and `data` is the register payload in hex. Reads carry no payload and show `"data":""`; `null` appears only when a payload had to be omitted (an anomaly worth reporting). The port is output-only: anything a client sends is discarded, so typing into the connection does nothing (the interactive command interface is the SAM ASCII port on 23).

Privacy filtering is applied before anything leaves the device: registers carrying WiFi credentials or dealer information produce no lines at all, and serial-bearing registers keep only the first four serial characters (Carrier serials lead with the week and year of manufacture, which is useful for diagnostics) with the rest masked for safer sharing in public forums.

```yaml
uart_tcp_server:
  - id: jsonl_uart
    port: 2373
    ...

bus_jsonl:
  infinitesp_id: infinitesp_hub
  uart_id: jsonl_uart
```

Timestamps use whatever `time:` platform the config declares to avoid extra wiring. Leaving it enabled costs little: with no client connected the stream costs almost no cpu and about a kilobyte of RAM. To disable it anyway, comment out the `jsonl_uart` server entry and the `bus_jsonl` block.

## Raw Bus Tap (Network)

Expose the raw ABCD serial bus over TCP so external tools (Infinitude, Wireshark dissector, custom scripts) can observe the bus in real time. This is useful for protocol analysis and for running Infinitude alongside InfinitESP.

```yaml
# Add these alongside the normal infinitesp config.
# IMPORTANT: infinitesp must read from the bridge, not the hardware UART.
# Change uart_id on the infinitesp: block from bus_uart to bus_bridge.

uart_tcp_server:
  - id: raw_bus_tap
    port: 4242
    max_clients: 2
    client_mode: fanout
    idle_timeout: 30s

uart_bridge:
  id: bus_bridge
  uarts:
    - bus_uart                                           # hardware RS485 (bidirectional)
    - uart: raw_bus_tap
      flow: from_bridge      # monitor-only (use 'both' to allow TCP client writes)
```

Connect with `nc infinitesp.local 4242` to see raw hex traffic. The `from_bridge` flow direction means TCP clients can only observe, not inject bytes onto the bus. This is the safer default. Change to `both` for bidirectional access (e.g., running Infinitude through the tap).

> **Warning:** the raw tap is wire-exact: it includes WiFi credentials (registers 4608/4609 travel the bus) and equipment serials. It is a maintainer and bench tool. Never post a raw-tap capture in an issue; use the JSONL stream on port 2373 instead.

> **Warning:** When using `flow: both`, TCP clients share the bus with InfinitESP. The ABCD bus has no arbitration. Simultaneous transmits will collide. Only use bidirectional mode if your tool understands the protocol timing.

## STATS Bus Health

The firmware prints a `STATS` line to the ESPHome log every 5 seconds. A healthy bus looks like this: `crc_fail=0`, `stale=0`, `overflow_evts=0`, and `reply_got` approximately equal to `reply_exp`.

The counters that matter:

- `crc_fail` > 0: bus noise or bad wiring. CRC errors clustered around the device's own transmissions point at RS485 biasing, bus loading, or wiring rather than the transceiver.
- `stale` > 0: bytes arriving with gaps > 100ms. A transport issue (a TCP bridge dropping bytes, an overloaded host), not a protocol issue.
- `overflow_evts` > 0: the main loop is not keeping up. Reduce logging verbosity.
- `reply_got` consistently below `reply_exp`: the device's polls are being sent but not answered. The thermostat is not seeing InfinitESP (wiring, address conflict, or commissioning state).

## Capturing for an Issue

- **`capture.jsonl`, 30 seconds around the behavior**: `nc infinitesp.local 2373 | tee capture.jsonl`. Best for timing-sensitive problems: mode changes that don't stick, flapping entities, suspected collisions.
- **`report.json`**: the `REPORT?` snapshot via the SAM ASCII port. Often eliminates the need for full logs. Recipe and privacy notes: [SAM ASCII interface](sam-ascii.md#report).
- **ESPHome logs, 30 seconds**: `esphome logs infinitesp.yaml --device infinitesp.local`. Includes the `STATS` line. Best for protocol issues.
