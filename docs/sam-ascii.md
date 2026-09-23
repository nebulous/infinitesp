# SAM ASCII Interface

InfinitESP implements the Carrier SAM ASCII serial protocol, the same text command/response interface a real SYSTXCCSAM01 exposes on its DB-9 RS-232 port (9600 8N1, CRLF). The `sam_ascii` component is a plain ESPHome `UARTDevice`: it speaks the protocol over whatever `uart_id` you bind it to and has no transport of its own. The command set works the same way regardless of transport.

**Over the network (the default in the example config).** The bundled config binds `sam_ascii` to a `uart_tcp_server` on port 23, so you can drive it with any telnet client:

```bash
nc infinitesp.local 23
```

**Over a real RS-232 port (a true SAM replacement).** Point `sam_ascii` at a second hardware UART wired to an RS-232 transceiver and it behaves like the physical SAM's serial port, useful for replacing a faulty module or feeding a legacy automation controller that expects a SAM:

```yaml
uart:
  - id: bus_uart       # ABCD RS485 bus (38400)
    # ... RS485 pins ...
    baud_rate: 38400
  - id: ascii_uart     # SAM RS-232 port (9600 8N1)
    tx_pin: GPIO4
    rx_pin: GPIO5
    baud_rate: 9600
    data_bits: 8
    parity: NONE
    stop_bits: 1

sam_ascii:
  infinitesp_id: infinitesp_hub
  uart_id: ascii_uart
```

The commands below work identically over either transport.

## Read Commands

| Command | Description |
|---------|-------------|
| `MODE?` | System mode (HEAT/COOL/AUTO/OFF/EMERGENCY HEAT) |
| `OAT?` | Outdoor air temperature (°F) |
| `TIME?` | Current time from bus clock |
| `DAY?` | Current day of week |
| `ZONE?` | Displayed zone number (1-8) |
| `BLIGHT?` | Backlight (ON/OFF) |
| `CFGEM?` | Display units (F/C) |
| `CFGDEAD?` | Heat/cool deadband (0-6) |
| `CFGCPH?` | Cycles per hour (2-6) |
| `CFGPER?` | Schedule periods per day (2 or 4) |
| `CFGPGM?` | Programming enabled (ON/OFF) |
| `DEALER?` | Dealer name |
| `DEALERPH?` | Dealer phone |
| `FILTRLVL?` / `UVLVL?` / `HUMLVL?` / `VENTLVL?` | Accessory life used % |
| `FILTRRMD?` / `UVRMD?` / `HUMRMD?` / `VENTRMD?` | Accessory reminder (ON/OFF) |
| `VACAT?` | Vacation state (ON/OFF) |
| `VACDAYS?` | Vacation duration, whole days (rounded up) |
| `VACHOURS?` | Vacation duration, hours (last commanded) |
| `VACMINT?` / `VACMAXT?` | Vacation min/max temperature |
| `VACMINH?` / `VACMAXH?` | Vacation min/max humidity |
| `VACFAN?` | Vacation fan mode |
| `Z1RT?` | Zone 1 room temperature (°F) |
| `Z1RH?` | Zone 1 humidity (%) |
| `Z1RHTG?` | Zone 1 humidification target (%) |
| `Z1HTSP?` | Zone 1 heat setpoint (°F) |
| `Z1CLSP?` | Zone 1 cool setpoint (°F) |
| `Z1FAN?` | Zone 1 fan mode (AUTO/LOW/MED/HIGH) |
| `Z1HOLD?` | Hold state (OFF / ON until HH:MM PM / PERMANENT) |
| `Z1OVR?` | Timed override active (ON/OFF) |
| `Z1OTMR?` | Override timer (HH:MM) |
| `Z1UNOCC?` | Zone unoccupied (ON/OFF) |
| `Z1NAME?` | Zone 1 name |
| `HELP` | List all commands |

Prefix with `Z#` for other zones (e.g., `Z2HTSP?`). The same prefix applies to the zone verbs below (`Z2HTSP!`, `Z2FAN!`, ...).

## Write Commands

Append `!` and a value to set parameters. Write commands need SAM emulation
(`sam_address` nonzero); on a passive install they ACK but send nothing and
change no state.

```
MODE!COOL           # Set system mode (HEAT/COOL/AUTO/OFF; EHEAT/HEATPUMP need experimental_heat_source_modes and NAK otherwise)
ZONE!2               # Set displayed zone (1-8; ACKed on every tstat tested, adopted by older UI-family controls)
VACDAYS!7           # Arm vacation for 7 whole days (0-365; 0 clears)
VACHOURS!48         # Arm vacation for 48 hours at native resolution (0-8760; 0 clears)
Z1HTSP!72            # Set zone 1 heat setpoint
Z1CLSP!68            # Set zone 1 cool setpoint
Z1FAN!AUTO           # Set zone 1 fan mode
Z1HOLD!120           # Hold zone 1 for 120 minutes
Z1HOLD!on            # Permanent hold
Z1HOLD!off           # Cancel hold, resume schedule
```

## REPORT?

`REPORT?` provides a quick bus snapshot and often eliminates the need for full logs. It produces a JSON dump of all observed bus traffic, device info, cached registers, and diagnostic counters: a self-contained snapshot of the bus state.

To save a clean snapshot to `report.json`, run this one-liner from any machine that can reach the device (Python ships with the ESPHome toolchain. On Windows use `python` instead of `python3`):

```bash
python3 -c "import socket;s=socket.create_connection(('infinitesp.local',23),5);s.sendall(b'REPORT?\r\n');open('report.json','wb').write(next(l for l in s.makefile('rb') if l.startswith(b'{')))"
```

To explore interactively instead (querying live values like `MODE?`, `Z1RT?`), connect with netcat and type commands:

```bash
nc infinitesp.local 23
```

> **Privacy note:** serials and dealer information are redacted in the REPORT output as of v2026.9.4. Device serials keep their first four characters (the week and year of manufacture); the rest of each serial field is masked. WiFi, cloud-account, and dealer registers are omitted entirely. No manual scrubbing is needed before posting. On older versions, open the file and remove the `serial` fields and any register data from `0104`, `060A`, `4608`, `4609`, and `460A` before posting publicly.

For timing-sensitive problems (mode changes that don't stick, flapping entities, suspected collisions), the JSONL stream capture is better than REPORT?: it carries per-frame timing. See [bus capture](bus-capture.md).
