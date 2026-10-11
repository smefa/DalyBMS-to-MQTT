# Handoff: power modes (2026-10-11)

Written for: whoever picks this firmware up next.

## What exists

Three power modes, switched by MQTT (`<base>/mode/set`, or the "Power mode" select Home Assistant gets through discovery). The mode is kept in flash.

| Mode | Radio | Reports | BMS poll |
|---|---|---|---|
| high | modem sleep | 60 s | 5 s |
| normal (default) | light sleep (CPU too), **10 s after MQTT has settled** | 15 min | 60 s; 5 s while the guard counts down |
| storage | off, deep sleep in chunks | 24 h | at the daily wake |

Details and settings: `docs/mqtt.md`, `src/config.h.example` (`POWER_*`). Code: `src/powermode.*`, radio and poll switching in `src/main.cpp`, policy in `lib/powercore`, MQTT subscribe and parsing in `lib/mqttlite`.

## State of the board (10.10.10.240)

Light-sleep build flashed over OTA. Clean test of the final build: 25 + 10 reboots, each held 100 s, no crash, no hang; heap 6.0-6.6 kB with MQTT up. BMS answering (4 cells).

## Open items

1. **Mode switch from Home Assistant is untested.** Try the select, watch `/log`.
2. **D0 to RST is not wired.** Storage mode and the low-voltage guard's wake-up need it. Do not select storage until it is.
3. **Power draw is unmeasured** in every mode. Light sleep should be the big saving in normal mode; measure it.
4. **Guard fast-poll is untested on hardware** (needs a cell under `GUARD_TRIP_CELL_MV`, or a temporarily raised threshold). Expect `/status.json` sweeps to speed up to one per 5 s while `guard` is `counting`.
5. **USB upload does not work** (esptool resets the board but never syncs). Flash over OTA (see Tooling).
6. Old dashboard tabs keep polling every 2 s until reloaded.

## Things learned the hard way

- **Memory is the limit.** With TLS up there is about 6 kB of heap; the handshake dips to about 3.4 kB. Low heap shows as `Exception 29`, `Unhandled C++ exception: OOM`, or an empty `/status.json`. Check `http://<ip>/heap`.
- **mDNS is gone on purpose.** The responder parses every mDNS packet on the LAN and allocates per record; it ran out of heap mid-parse and the board hung in the crash handler. Removing it gave back about 1 kB. Use the IP or `dalybms.lan`.
- **Light sleep at the moment MQTT connects** gave a Soft WDT reset in roughly one boot in five to ten, 30-40 s after boot, when the first BMS sweep under light sleep met the discovery burst. Hence the settle delay. The old note about `Exception 29` in `ieee80211_parse_beacon` did not reproduce.
- **`POWER_NORMAL_LISTEN_INTERVAL` does nothing on the default SDK.** The core only passes it on NONOS SDK 3.x (`-D PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK305`), which is untested here and may cost heap.
- **TLS connect under the slow radio takes 40 s** and trips the watchdog, another reason the slow radio waits for MQTT.
- **`x = millis() | 1` as a "never 0" timestamp** can land 1 ms in the future, and `millis() - x` then wraps. It bit the settle timer; `ha_mqtt.cpp` still uses it for `last_state_`, where it is harmless because a publish takes longer than a millisecond.
- **String literals cost RAM.** Use `APPLOG(...)`, not `applog::logf`.
- Do not print to Serial: GPIO1 is the BMS request line.

## Tooling

- Firmware: `pio run -e d1_mini`.
- Flash over OTA: `python <framework-arduinoespressif8266>/tools/espota.py -i 10.10.10.240 -p 8266 -a <OTA_PASSWORD> -f .pio/build/d1_mini/firmware.bin` (exit code 0 = done).
- Reset over USB: toggle RTS on COM5 with pyserial (set `dtr`/`rts` False before `open()` so opening does not reset it).
- A crash boot logs `boot: exc .. epc ..` in `/log`; resolve the addresses against that build's `firmware.elf` with pyelftools (Windows blocks the toolchain's `addr2line`). Full dumps print on UART0 at 9600 baud.
- Host tests: `pio test -e native` fails here (Windows blocks MinGW). Use g++ in WSL Ubuntu; it compiles `lib/*` and `test/*` against `.pio/libdeps/native/Unity`. This session changed nothing under `lib/`.
- This folder is not a git repo.
