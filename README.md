# DALY BMS → ESP8266 → Home Assistant

Reads a DALY BMS over its TTL UART port with a Wemos D1 Mini and publishes the
data to Home Assistant over **MQTT carried on a WebSocket** (`wss://`, so it
passes through a reverse proxy on port 443), with MQTT discovery so the entities
appear on their own. It also serves a self-contained web dashboard.

```
DALY BMS ──UART 9600──▶ Wemos D1 Mini ──WiFi──▶ reverse proxy ──▶ MQTT broker ──▶ Home Assistant
                          ├── MQTT over WebSocket (publishes; takes one command: power mode)
                          └── :80  dashboard + /status.json
```

## Status

- **ESP8266 firmware** — running on hardware, reading a DALY 4S BMS (2026-10-11).
- **MQTT over WebSocket** — implemented and compiles; not yet run against a broker.

## Layout

| Path | What |
|---|---|
| [lib/daly/](lib/daly/) | DALY protocol parser. No Arduino dependency, so it unit-tests on the host. |
| [lib/mqttlite/](lib/mqttlite/) | Minimal MQTT 3.1.1 packet encoder. No Arduino dependency, host-tested. |
| [src/](src/) | Firmware: polling driver, MQTT/Home Assistant publisher, HTTP server, dashboard. |
| [test/](test/) | Host-side parser and MQTT tests, hardware not required. |
| [tools/uarttest/](tools/uarttest/) | Bench web app (`pio run -e uarttest`) that finds the BMS TX/RX pins by trying every combination. |
| [docs/](docs/) | Wiring, MQTT / Home Assistant setup. |

## Getting started

### 1. Wire it

Read [docs/wiring.md](docs/wiring.md) first — in particular, **verify your BMS
connector pinout** and **do not take power from the BMS UART port**.

### 2. Configure

```sh
cp src/config.h.example src/config.h
```

Fill in the WiFi SSID, password, an OTA password and the MQTT broker details
(see [docs/mqtt.md](docs/mqtt.md)). `src/config.h` is
git-ignored so credentials stay out of the repository.

### 3. Build and flash

Requires [PlatformIO](https://platformio.org/install) — either the VS Code
extension or the CLI (`pip install platformio`).

```sh
pio test -e native      # parser tests, no hardware needed
pio run -e d1_mini      # compile
pio run -e d1_mini -t upload
```

The `d1_mini` build needs nothing else: PlatformIO fetches its own
`xtensa-lx106-elf-gcc`. The `native` test environment compiles for the host, so
it needs a host compiler — on Windows that means MinGW-w64, e.g.
`winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT`.

The BMS replies arrive on D5, not the RX pad, so the BMS can stay connected for
USB flashing. OTA also works:

```sh
pio run -e d1_mini -t upload --upload-port dalybms.lan
```

### 4. Check it

```sh
curl http://<esp-ip>/status.json
```

Open `http://<esp-ip>/` for the dashboard, `http://<esp-ip>/log` for
diagnostics. Compare SOC, pack voltage and cell voltages against the DALY phone
app — they should match.

### 5. Set up Home Assistant

Follow [docs/mqtt.md](docs/mqtt.md).

## Design notes

**The BMS answers with the wrong address, and that is normal.** DALY units reply
with their own physical address (`0x01`), not the `0x40` sent in the request. A
parser that validates the address byte rejects every valid response. This is
[esphome/esphome#12445](https://github.com/esphome/esphome/issues/12445), closed
"not planned" and still unfixed upstream. `FrameDecoder` checks the start byte,
command range, payload length and checksum — never the address — and
`test_response_address_byte_is_not_matched` guards it.

**Nothing may print to `Serial`.** UART0 TX is the BMS request line; a stray `Serial.print`
injects bytes into it. Use `applog::logf()` and read `/log`.

**Stale data is reported, not hidden.** A BMS that stops answering leaves the
last values in place. `/status.json` sets `valid: false` and the *BMS data fresh*
entity goes off, so a dead link cannot masquerade as a healthy battery. Alarm on
it — see [docs/mqtt.md](docs/mqtt.md).

**The bridge protects the pack from itself.** It is powered ahead of the BMS
output MOSFETs so that telemetry survives a discharge cutoff — which means the
BMS's low-voltage protection does not cover it, and ~20 mA left running would
flatten a LiFePO4 pack over a few weeks. [voltguard](src/voltguard.h)
watches the cell voltages the BMS already reports and deep-sleeps the board at
3.00 V/cell, waking every half hour to re-measure with the radio off and
resuming once the pack is back above 3.20 V/cell. The *Low-voltage guard* entity shows
`counting` for a full minute before it sleeps, so Home Assistant can get the
notification out first.
**This needs D0 wired to RST** — see [docs/wiring.md](docs/wiring.md).

**Three power modes**, switched from Home Assistant: `high` (report every minute),
`normal` (every 15 minutes, radio at its slowest) and `storage` (radio off, one
report a day). Storage also needs D0 wired to RST. See
[docs/mqtt.md](docs/mqtt.md#power-modes).

## References

- [Daly UART/485 Communications Protocol V1.2](https://robu.in/wp-content/uploads/2021/10/Daly-UART_485-Communications-Protocol-V1.21-1.pdf)
- [maland16/daly-bms-uart](https://github.com/maland16/daly-bms-uart) — reference field offsets
- [MQTT 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)
- [Home Assistant MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
