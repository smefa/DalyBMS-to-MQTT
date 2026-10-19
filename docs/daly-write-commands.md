# Daly BMS write commands

Protocol reference for the BMS write and unlock commands. Status as of 2026-10-19:

- **Implemented:** cell voltage limits (0x19), pack voltage limits (0x1A) and the sleep timeout / current wave (0x13). The firmware sends them from MQTT and the web page.
- **Not implemented:** MOSFET switching (0xD9, 0xDA), BMS reset (0x00), set SOC (0x21), balance thresholds (0x1F) and current limits (0x1B). The tables below keep them for reference.

Sources:
- https://github.com/acpiccolo/Daly-BMS (`src/protocol.rs`). The Daly UART/485 Communications Protocol V1.2 spec is not in this repo; a copy is at https://robu.in/wp-content/uploads/2021/10/Daly-UART_485-Communications-Protocol-V1.21-1.pdf.
- https://github.com/matthewgream/DalyBMSInterface (`src/DalyBMSRequestResponseTypes.hpp`, `src/DalyBMSRequestResponse.hpp`)
- https://github.com/maland16/daly-bms-uart (reference field offsets)
- A forum post on the configuration writes (not in this repo).

## Frame

Same 13-byte frame as `buildRequest`:

`A5 40 <cmd> 08 <8 data bytes> <checksum>`

Checksum = sum of bytes 0-11, mod 256. Frame byte 4 is the first data byte.

The BMS answers with its own physical address (0x01), not the 0x40 sent in the request. The decoder does not check the address byte, because rejecting replies on it drops every valid reply. See [esphome/esphome#12445](https://github.com/esphome/esphome/issues/12445).

## Commands

| Action | Cmd | Data | Checksum | Confirmed by |
|---|---|---|---|---|
| BMS reset | 0x00 | all zeros | 0xED | both repos |
| Discharge MOSFET | 0xD9 | byte 4 = 0x01 on / 0x00 off | 0xC7 on, 0xC6 off | both repos, captured from Windows app |
| Charge MOSFET | 0xDA | byte 4 = 0x01 on / 0x00 off | 0xC8 on, 0xC7 off | both repos, captured from Windows app |
| Set SOC | 0x21 | bytes 10-11 = big-endian u16 of round(pct*10), clamp 0-1000 | e.g. 80.5% = 0x0325, checksum 0x36 | acpiccolo only |

Checksums were hand-verified. The four MOSFET frames also match the Windows app's frames on the wire, including charge-off 0xC7. Reset and set SOC have not been captured.

Write replies: the BMS answers with a normal frame carrying the same command byte (`A5 01 <cmd> 08 ...`) and a valid checksum. The payload is not an echo of the request and looks unrelated to it (charge-off replied with all zeros), so do not use it to confirm the new state. Read the setting back instead (0x93 for MOSFETs).

## Config writes

Pattern: write cmd = read cmd - 0x40. The forum post sniffed DalyBmsMonitor V2.2.1 on one BMS-103/VE-309 (16S, FW 41_2308). Rows marked "captured" were also checked against the Windows Daly app on a 4S pack, so the pattern holds across models. The app writes a whole group of four values in one frame.

| Action | Write cmd | Read cmd | Data bytes 4-11 | Status |
|---|---|---|---|---|
| Cell voltage limits | 0x19 | 0x59 | 4-5 high L1 mV, 6-7 high L2 mV, 8-9 low L1 mV, 10-11 low L2 mV (all BE) | captured: `A5 40 19 08 0E38 0E42 08FC 0A28 D2` = 3640 / 3650 high, 2300 / 2600 low |
| Pack voltage limits | 0x1A | 0x5A | 4-5 high L1, 6-7 high L2, 8-9 low L1, 10-11 low L2, all BE in 0.1 V | captured: `A5 40 1A 08 0084 0085 0076 006C F2` = 13.2 / 13.3 V high, 11.8 / 10.8 V low (values unchanged from the read) |
| Balance thresholds | 0x1F | 0x5F | 4-5 start voltage mV (BE), 6-7 diff voltage mV (BE), 8-11 zero | captured: `A5 40 1F 08 0D20 0014 00000000 4D` = start 3360 mV, diff 20 mV |
| Current wave + BMS timeout (sleep) | 0x13 | 0x53 | 4-5 config (copy from read), 6-7 flags (copy from read), 8 unknown (copy from read; 0x13 on the test pack), 9-10 BMS timeout in seconds (BE, 0xFFFF = no timeout), 11 current wave in 0.1 A | captured, see below |
| Current limits | 0x1B (expected) | 0x5B | not captured | read only so far |

Level order for 0x19 and 0x1A is high L1, high L2, low L1, low L2. On the 4S pack the app shows 0x19 high L1 3640 mV and low L1 2300 mV, and 0x1A high L1 13.2 V and low L1 11.8 V.

Balance example: start 3200 mV (0x0C80), diff 20 mV (0x0014) gives data `0C 80 00 14 00 00 00 00`, checksum 0xAC. The 0x5F read carries a trailing field that the balance write leaves as zero: `0D20 0014 0000 0C80`. Its meaning is unknown.

Always read first (0x53 / 0x5F), change only the target field, copy the rest back, then read back to verify.

### Sleep timeout (0x13)

- Read on the 4S test pack (0x53): `00 01 17 06 13 FF FF 02`. No timeout, current wave 0.2 A.
- Bytes 4-8 and 11 are copied from that read. Bytes 9-10 hold the timeout as big-endian seconds, and 0xFFFF means no timeout. Two writes confirmed the byte order: 1234 s is `A5 40 13 08 00 01 17 06 13 04 D2 02 09`, and 4321 s is `A5 40 13 08 00 01 17 06 13 10 E1 02 24`.
- No timeout: `A5 40 13 08 00 01 17 06 13 FF FF 02 31`.
- Byte 8 stays 0x13 whether or not a timeout is set. The forum post's "sleep enable" in that byte does not match this pack.
- Write replies are not reliable. The first reply was the old data with byte 4 changed, and later replies were stray single bytes. Confirm a write by reading the setting back.
- The 0x13 checksum depends on the copied bytes. Compute it as the sum of frame bytes 0-11 mod 256.
- Current wave (DfilterCur) is the current below which the BMS reports 0 A. The forum post says 0x02 (0.2 A) plus a zero-current calibration fixed idle active balancing. Its example sleep timer, 62000 s, is 0xF230.

### Engineering unlock

The forum post says extended commands are silently ignored until this sequence is sent. The Windows app sends it on connect. Not implemented here.

| Cmd | Data | Checksum |
|---|---|---|
| 0xE0 | A5 5A 00 00 00 00 00 00 | 0xCC |
| 0xE1 | A5 5A 00 00 00 00 00 00 | 0xCD |
| 0xE2 | A5 5A 00 00 00 00 00 00 | 0xCE |
| 0xE3 | RTC sync: YY MM DD HH MM SS 00 00 | sum mod 256 |

Frames are `A5 40 <cmd> 08 <data> <checksum>`. Extra reads said to need the unlock: 0x50 battery ratings, 0x67 alt FW version, 0x68 and 0x69 extended status, 0x99 equilibrium status, 0xD8 equilibrium config.

## Open questions

- The forum post says 0x59-0x60 need the unlock, but the repos above read 0x59 and 0x5A without it. This may be model dependent.
- The 0x5B field order is unverified. 0x5C to 0x5E are not decoded.
- Replies to 0x69, 0x54, 0x56 and 0x6A were absent on the test pack, so they may be model dependent or need something else.
- The app sends 0x64 with byte 4 = 0x01 about every 3.3 s while its settings screen is open. It never gets a reply. Purpose unknown, possibly a keep-alive.
- Wrong values can damage the pack. The forum post notes the UART ground is tied to B-, so use an isolated adapter.

## Notes

- Replies are normal 13-byte frames. The acpiccolo crate only checks length and checksum and ignores the payload.
- Neither repo has write commands for voltage thresholds, rated capacity or balancing (the forum post above adds balance thresholds and current wave). 0x97 is read-only.
- matthewgream defines read frames only for thresholds 0x59-0x60 (0x59 cell voltage, 0x5A voltage, 0x5B current, 0x5C sensor, 0x5D charge, 0x5E cell sensor, 0x5F cell balance, 0x60 short-circuit). Its README marks them "read/write (likely)" and also lists 0x5C twice. The code uses 0x5E for cell sensor.
- `isKnownCommand` in `lib/daly/daly_protocol.h` accepts 0x90-0x98, the threshold reads 0x59 and 0x5A, and the writes 0x19, 0x1A, 0x13 and 0x53. Replies to 0x00, 0xD9, 0xDA, 0x21, 0x1F and 0x1B would be dropped until it is extended.
- Reset and set-SOC change BMS state. Treat with care.
