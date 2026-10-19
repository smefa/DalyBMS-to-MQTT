# Status

Updated 2026-10-19. Test hardware: Wemos D1 Mini on a DALY 4S 12 V 100 A BMS.

## Working on hardware

- BMS polling and telemetry. The 4S test pack reports all four cells.
- MQTT over WebSocket through a reverse proxy (`wss://`), connected to Home Assistant with discovery. The proxy's TLS handshake was checked on 2026-10-12.
- Web dashboard and the `/status.json`, `/log`, `/heap`, `/events` and `/crash` endpoints.
- USB and OTA flashing. The blue-LED freeze after OTA was fixed on 2026-10-17, and the four OTAs since then were clean.
- Power mode `high`: a clean test of 35 reboots, each held 100 s, with no crash or hang (heap 6.0 to 6.6 kB with MQTT up).
- Storage sleep cycle, run on hardware on 2026-10-19. Uptime was tuned against a real clock (`POWER_SLEEP_CLOCK_PERMILLE` 940). Normal uses the same deep-sleep cycle; it has not been recorded as tested on its own.
- BMS wake pulse on S1: a silent BMS answered right after the first 300 ms pulse (2026-10-12).

## Not verified, or known problems

- Normal and storage need D0 wired to RST, or the board never wakes after its first sleep. Whether the test board has it wired is not recorded.
- Switching mode from Home Assistant has not been recorded as tested end to end.
- The low-voltage guard's fast poll while `counting` has not been tested on hardware.
- Current draw has not been measured in any mode.
- The sleep-timeout write (0x13) uses a frame layout from a forum capture. Check the read-back the first time (see [MQTT: changing the BMS voltage limits](mqtt.md#changing-the-bms-voltage-limits)).
- `dalybms.local` is unreliable in high mode, because light sleep misses some multicast queries. Use the IP address or `dalybms.lan`.
- An OTA from a build older than the 2026-10-17 fix can leave the board with a solid blue LED and no WiFi. Only a power cut clears it.
