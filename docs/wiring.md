# Wiring

## Board

Luxorparts Wemos D1 Mini (Kjell 87294) — **ESP8266EX**, 4 MB flash, 3.3 V logic.

Note this is an ESP8266, not an ESP32, despite what the repository folder name
suggests. It matters: the ESP8266 has one usable hardware UART, and it is shared
with the USB-serial bridge.

<img src="D1.webp" alt="Wemos D1 Mini board with pin labels" width="320">

## DALY UART (TTL) port

| DALY UART | D1 Mini pin | ESP8266 GPIO |
|---|---|---|
| TX  | **D5**   | GPIO14 (receive-only SoftwareSerial) |
| RX  | TX / D10 | GPIO1 (hardware UART0 TX) |
| GND | GND      | — |

**Do not use the D1 Mini RX pad (GPIO3) for the BMS TX wire.** See
[Why the BMS reply is not on UART0 RX](#why-the-bms-reply-is-not-on-uart0-rx).

Both sides are 3.3 V TTL, so no level shifter is needed.

### Verified pinout (DALY 4S 12V 100A R05A1 F23)

Found with the bench tool in [tools/uarttest/](../tools/uarttest/) and confirmed
by a valid reply (9600 baud, 8N1, request address `0x40`, reply address `0x01`).
The BMS port is a 6-position housing; count positions from the end where the
black wire sits. It matches the pinout of the USB-UART dongle that plugs into
the same port, which uses positions 1, 5 and 6.

| Position | Pigtail wire | Function | Goes to |
|---|---|---|---|
| 1 | black  | GND        | D1 GND |
| 2 | green  | not used   | leave unconnected |
| 3 | —      | empty slot | — |
| 4 | yellow | not used   | leave unconnected |
| 5 | white  | BMS TX     | D1 **D5** (GPIO14) |
| 6 | red    | BMS RX     | D1 TX (GPIO1) |

**The red wire is a data line, not a supply.** Do not treat it as one, and do
not connect the unused wires. Wire colours are specific to this pigtail; go by
position if you build another cable.

To re-check a different board, flash the `uarttest` environment
(`pio run -e uarttest -t upload`) and use its Auto-detect page.

### Before you connect anything

**Verify the pinout against your own BMS.** DALY ships several connector
variants and the pin order is not consistent between the UART and RS485 models
— on some the same physical RJ45 carries RS485 differential pairs instead of
TTL. Meter it or check the label before powering anything on.

**Do not take power from the BMS UART connector.** Some DALY variants expose
battery voltage on one of those pins, which will destroy a D1 Mini. Where the
power does come from is its own section below.

**Ground must be common.** The BMS UART ground has to be tied to the D1 Mini
ground for the signal to have a reference. Since both ultimately sit on battery
negative, keep the run short and use a single ground path to avoid a loop.

## Power, and the D0 → RST jumper

Feed the D1 Mini from a 12 V → 5 V buck converter (MP1584, LM2596 or similar,
1 A is plenty) into the **5 V pin** or the micro-USB socket. Put a few hundred µF
across the buck input: charging and inverter loads make the 12 V rail spiky.

Take that 12 V **ahead of the BMS output MOSFETs**, i.e. straight off the pack
terminals rather than from the load side. Otherwise the bridge dies at exactly
the moment its telemetry becomes interesting — a discharge cutoff — and you get
silence instead of an explanation.

The price of that choice is that the BMS's own low-voltage protection no longer
protects the pack *from the bridge*. After cutoff, the bridge is the only load
left on the battery, and its ~20 mA — half an amp-hour a day — will walk a
LiFePO4 pack down into permanent damage over a few weeks.
[src/voltguard.h](../src/voltguard.h) is what prevents that, and it needs one
wire:

| From | To | Why |
|---|---|---|
| D0 (GPIO16) | RST | Lets the ESP8266 wake itself from deep sleep |

**Storage power mode needs the same jumper** ([docs/mqtt.md](mqtt.md#power-modes)): it
sleeps for a day at a time and wakes itself the same way. So does normal, and so
does **high** once WiFi has been missing for `POWER_OFFLINE_GIVEUP_S` (an hour by
default): it then sleeps and tests for a network once a day. On a board without the
jumper, set `POWER_OFFLINE_GIVEUP_S 0` in `config.h` so high never gives up.

**Without this jumper the guard is a one-way trip.** The first time the pack goes
low the board sleeps and never wakes; you get it back by power-cycling it. With
the jumper it wakes every `GUARD_SLEEP_MINUTES`, reads the pack with the radio
off, and resumes on its own once the voltage has recovered.

The jumper has one side effect: it interferes with the auto-reset circuit some
USB flashing tools rely on. If a USB upload fails to start, lift the D0 end,
flash, and put it back. After the first flash you are on OTA anyway.

Deep sleep here draws roughly 20 µA — about a thousandth of what the board uses
awake — so a sleeping bridge is not what flattens the battery.

## BMS wake (S1)

The BMS has a wake pad, **S1**, which idles at about 2.9 V (an internal pull-up that
sources only ~30 µA). Shorting S1 to ground wakes the BMS. Storage mode uses it,
because the BMS has usually gone to sleep during the bridge's day-long sleep and
would not answer the report's first requests.

| From | To | Why |
|---|---|---|
| D7 (GPIO13) | 1 kΩ, then BMS **S1** | pulled low for 300 ms to wake the BMS |
| GND | BMS GND pad | common reference, same as the UART ground |

- The pin is an input (high-Z) except during a pulse. It is **never driven high**: S1
  is a 2.9 V pull-up, not a supply, and 5 V or 3.3 V on it is not what the BMS expects.
- The 1 kΩ is protection against a firmware fault or a slipped probe; at 30 µA it
  drops about 30 mV. 510 Ω to 2 kΩ also work.
- D7 has no boot role. Avoid D3, D4 and D8, which do. D5, TX and D0 are taken above.
- Enable it with `#define BMS_WAKE_PIN 13` in `config.h` ([config.h.example](../src/config.h.example)).
  Without that define the feature is off and the pin is not touched.
- In every power mode the bridge pulses only if the BMS has not answered `BMS_WAKE_GRACE_S` (3 s)
  after boot, then every `BMS_WAKE_RETRY_S` (10 s) while it stays silent, at most
  `BMS_WAKE_MAX_PULSES` (3) times. An awake BMS is never pulsed. The pulse
  length, settle time and counts are `BMS_WAKE_*` settings.
- Verified on hardware 2026-10-12: a BMS that was silent (every request timing out) answered the sweep right after the first 300 ms pulse. Without the pulse, normal mode never woke it.
- If the BMS does not wake, try a longer `BMS_WAKE_PULSE_MS`.

## Current draw

These are typical values, **not measured on this board**.

| State | Draw | Notes |
|---|---|---|
| Awake, WiFi modem sleep | ~20 mA avg | Normal running. Radio parks between DTIM beacons. |
| Awake, radio forced on | ~75 mA | What `WIFI_NONE_SLEEP` in main.cpp used to cost. |
| Guard deep sleep | ~20 µA | Plus whatever the buck converter idles at. |

Check the buck converter's own quiescent draw before blaming the ESP: a cheap
LM2596 module burns 5–10 mA doing nothing, which swamps the deep-sleep figure
and is the actual floor for the whole bridge.

## Why the BMS reply is not on UART0 RX

Requests go out on hardware UART0 TX (GPIO1); replies come in on a
receive-only `SoftwareSerial` on D5 (GPIO14). See `BmsPort` in
[main.cpp](../src/main.cpp).

Wiring the BMS TX to the D1 Mini RX pad (GPIO3) was tried first and received
**nothing**: no frames and no checksum errors, while the same BMS answered fine
through `uarttest` on D5/D6 and through its Bluetooth dongle. The CH340 USB chip
drives GPIO3 straight from its TX output with no series resistor (the 470 Ω
resistor is on the other direction), and it stays powered from 3V3 even with USB
unplugged. The BMS TX is a weak driver and cannot pull that line low, so the
replies never reach the ESP. Frames injected through the CH340 decoded correctly,
which ruled out the firmware.

GPIO1 (TX) is not affected: the CH340 and the BMS only have inputs on it.
Hardware TX is kept because software TX blocks for about 1 ms per byte.

Consequences:

1. **Nothing in the firmware may print to `Serial`** — GPIO1 is the BMS request
   line. All diagnostics go to `http://dalybms.local/log` instead.
2. The BMS can stay connected during USB flashing, since it no longer shares
   GPIO3 with the USB bridge.

At boot the ESP8266 ROM emits a short burst at 74880 baud on TX. The BMS ignores
it; this is harmless.

## Serial parameters

9600 baud, 8N1 — fixed by the DALY protocol, not configurable.

## Placement

Keep the D1 Mini out of the battery compartment if the pack vents, and check
WiFi signal at the intended spot: `http://dalybms.local/status.json` reports `rssi`.
Anything worse than about −75 dBm will drop polls.
