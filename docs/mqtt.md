# Home Assistant over MQTT (WebSocket)

The bridge speaks MQTT 3.1.1 inside binary WebSocket frames (subprotocol
`mqtt`), over `wss://` by default. That lets it reach a broker through a reverse
proxy on port 443, which is the usual way to expose Home Assistant over HTTPS.

It publishes everything, and takes four kinds of command back: the power mode, the
BMS voltage limits, the pack voltage profile and the MOSFET switches (all below). The
broker user therefore needs **write** access to `dalybms/#` and `homeassistant/#` (the
retained discovery configs) and **read** access to `dalybms/mode/set`, `dalybms/limits/set`,
`dalybms/profile/set`, `dalybms/charge_mos/set` and `dalybms/discharge_mos/set`. Without the read access the bridge logs `broker refused
a subscribe` and carries on, but those commands cannot be received.

## Topics

With `MQTT_BASE_TOPIC "dalybms"`:

| Topic | Direction | Retained | Content |
|---|---|---|---|
| `dalybms/status` | out | yes | `online`; the broker publishes `offline` as the last will |
| `dalybms/state` | out | normal and storage modes | one JSON document with every value, including `mode` |
| `dalybms/mode/set` | **in** | should be | `storage`, `normal` or `high` (any case) |
| `dalybms/limits` | out | yes | the BMS's 8 voltage limits as JSON, sent when they change |
| `dalybms/limits/set` | **in** | **never** | JSON command to change the limits (below) |
| `dalybms/limits/result` | out | no | `queued`, then `done` / `unchanged` / `mismatch`; or why it was refused |
| `dalybms/profile/set` | **in** | no | `normal` or `storage`, the bare profile name (see [Pack voltage profiles](#pack-voltage-profiles)) |
| `dalybms/charge_mos/set`, `dalybms/discharge_mos/set` | **in** | no | `ON` or `OFF` (see [Switching the MOSFETs](#switching-the-mosfets)) |
| `homeassistant/<component>/dalybms/<id>/config` | out | yes | MQTT discovery |

The state document is also sent immediately whenever the low-voltage guard
changes state, or the mode changes between normal and high.

## Changing the BMS voltage limits

Publish JSON to `dalybms/limits/set` (not retained: a retained command would be
replayed on every reconnect). `pin` is required; send fields of **one**
group (cell, pack or sleep) per command. Fields you leave out keep their current value.

```json
{"pin":"your-pin","cell_high_l1_mv":3650,"cell_high_l2_mv":3700,"cell_low_l1_mv":2800,"cell_low_l2_mv":2600}
{"pin":"your-pin","pack_high_l1_v":58.4,"pack_low_l1_v":44.0}
```

A third group, `{"pin":"your-pin","sleep_timeout_s":62000}`, sets the BMS sleep timeout
(10-65535 s). It rewrites the BMS's combined sleep/current-wave frame (0x13) with every
other byte copied from the last reading, so the sleep enable and current wave are kept.
That frame layout comes from a forum capture, not the spec or this pack; check the
read-back result the first time.

Keys and units are those of `dalybms/limits` and `status.json`: cells in mV, packs in V.
Level order is high L1, high L2, low L1, low L2. Ranges are checked (cell 1500-4500 mV,
pack 5-100 V) and every high level must be above every low level.

The PIN is `SETTINGS_PIN` in `config.h`. There is no default, and the build fails
without it. Five wrong PINs lock
changes for 60 s. The bridge sends the change to the BMS between readings, then reads
the limits back and reports on `dalybms/limits/result`. `mismatch` means the BMS did not
take the values. A change that equals what the BMS already holds is not written
(`unchanged`), so the BMS memory is not worn by repeats. The web dashboard has the same
form under "Change BMS voltage limits"; it sends the PIN over plain HTTP.

### Pack voltage profiles

Two ready-made sets of pack limits, set in `config.h` (`PACK_NORMAL_{HIGH,LOW}_{L1,L2}_DV` and
`PACK_STORAGE_...`, in 0.1 V): **normal** 14.2-11.0 V and **storage** 13.2-12.0 V as L2, with
L1 0.1 V inside by default (normal: high 14.1 / 14.2, low 11.1 / 11.0).

- Home Assistant: the **Pack voltage profile** select. It publishes the bare name to
  `dalybms/profile/set` (not retained) with no PIN, so the broker login is the only gate,
  as for the power mode. The broker user needs read access to that topic. The current
  profile is `pack_profile` in the state document (`normal`, `storage`, `custom` or `unknown`).
- With a PIN: `{"pin":"your-pin","profile":"storage"}` to `dalybms/limits/set`.
- Web page: the Normal / Storage buttons under "Change BMS settings" (PIN required).

Outcomes are reported on `dalybms/limits/result` like any other limits change.

## Switching the MOSFETs

The **Charge MOSFET** and **Discharge MOSFET** are Home Assistant switches. They publish
`ON` or `OFF` to `dalybms/charge_mos/set` or `dalybms/discharge_mos/set` (not retained, no
PIN: the broker login is the gate, as for the profile select). Turning the discharge MOSFET
off cuts the load; turning the charge MOSFET off stops charging.

- With a PIN, over MQTT: `{"pin":"your-pin","discharge_mos":"off"}` to `dalybms/limits/set`
  (`true`/`false`, `"on"`/`"off"` and `1`/`0` are all accepted). One MOSFET per command.
- Web page: click the Charge MOS or Discharge MOS pill (PIN required). The pill changes to
  what the BMS reports after the switch, not to what was clicked.
- The command is always sent, even if the last reading shows that state already. The bridge
  then reads the BMS back (0x93) for up to 30 s; `dalybms/limits/result` reports `queued`,
  then `done`, or `mismatch` if the BMS never showed the new state.
- The switch follows the new state in the next state document, which is sent at once when a
  MOSFET changes.
- Only high mode is listening all the time. In normal and storage mode the bridge is awake
  for seconds per report, and a non-retained switch command sent while it sleeps is lost.
- These replace the earlier binary sensors with the same names. On the first discovery the
  bridge deletes the old retained `binary_sensor` configs, so those entities disappear from
  Home Assistant and automations that used them need pointing at the switches.

## Power modes

The **Power mode** entity (a select, under the device's configuration section) or
a publish to `dalybms/mode/set` chooses one. The choice is kept in flash and
survives power loss; a board that has never been told starts in `normal`
(`POWER_DEFAULT_MODE`).

| Mode | Radio | Reports | Dashboard / OTA |
|---|---|---|---|
| `high` | on, light sleep (once MQTT has connected and caught up) | every 2 min | ~1 s latency, sometimes a few |
| `normal` | **off** between reports (deep sleep) | every 15 min | only for a short window after each report |
| `storage` | **off** | once a day | only for a short window after each report |

Intervals are `POWER_*` settings in `config.h`. Current draw has not been measured
on a real board. Expect roughly 20 mA for high and well under 1 mA for normal and
storage, depending on the regulator and USB chip on your board; these are estimates.

`high` is the only mode that is online all the time. Its CPU light-sleeps between
polls (`POWER_HIGH_LIGHT_SLEEP`) and the radio wakes for the access point's DTIM
beacons. Light sleep starts only 10 s after MQTT has settled (connected, discovery
sent, first state out): the TLS handshake is a long blocking call that took 40 s
under a slow radio on the bench, enough to trip the watchdog. A dropped connection
goes back to full speed until it has settled again. A listen interval would stretch
the radio's wakeups, but the core only applies it on NONOS SDK 3.x
(`-D PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK305`), so it is not used.
 `normal` and `storage` are the
same wake, report and deep-sleep cycle, 15 minutes (`POWER_NORMAL_INTERVAL_S`) and
a day (`POWER_STORAGE_INTERVAL_S`) apart. The BMS is only polled during the
session (every `POLL_INTERVAL_MS`), so its own sleep timeout does not matter and
there are no idle pings or keepalive to tune. A session also stays up while the
low-voltage guard is counting down, so a passing sag is cleared by fresh readings
rather than tripping it.

### Memory is the tight resource

With TLS up the ESP8266 has about 6 kB of heap left, and the TLS handshake itself
dips to about 3.4 kB. Anything that asks for memory then can crash the board with
`Exception 29` or `Unhandled C++ exception: OOM`. A dashboard that fires a request
every 2 s while the answers take seconds does exactly that, so the dashboard now
polls one request at a time, every 10 s outside high mode. If you add features,
watch `free_heap` in `/status.json`, or `http://dalybms.local/heap`, which answers even when
the heap is too low to build the JSON.

**Storage and normal** are deep sleep. A day is longer than the chip can sleep in one go, so it
sleeps in chunks of an hour or so and only brings up the radio after the last
one. On that wake it connects, publishes the state (retained, so a restarted Home
Assistant still has numbers), sends discovery, reads any waiting mode command,
disconnects cleanly and sleeps again. A clean disconnect leaves `status` as
`online`; the entities expire after twice the sleep period
(`POWER_NORMAL_INTERVAL_S` or `POWER_STORAGE_INTERVAL_S`) so a bridge that died asleep does not look healthy
for ever. If a report cannot get through it is retried after
`POWER_STORAGE_RETRY_S` (3 h). After `POWER_OFFLINE_GIVEUP_S` (1 h) without WiFi, in
any mode, it tests for a network only once per `POWER_OFFLINE_INTERVAL_S` (24 h); a
connection or a power-up starts the count again.

Things to know about normal and storage:

- **Needs D0 wired to RST**, like the low-voltage guard ([wiring](wiring.md)).
  Without it the bridge goes to sleep and never wakes until power-cycled, and
  then goes straight back to sleep.
- **Switching out takes up to a full period (15 min or a day).** The command sits retained on the
  broker (Home Assistant retains it) until the next wake. To act sooner, press the
  reset button or power-cycle the bridge: a storage bridge that did not wake from
  a timer stays up for `POWER_STORAGE_GRACE_S` (2 min) after reporting, long
  enough to collect the command, or to flash over the air.
- **Switching into normal or storage is immediate:** the bridge saves the mode, restarts,
  reports once and sleeps.
- **Waking the BMS:** with `BMS_WAKE_PIN` set and S1 wired ([wiring](wiring.md#bms-wake-s1)),
  each boot, in every mode, pulses the BMS wake pad before polling it, and again every
  10 s (up to 3 pulses) while the BMS stays silent. Without it, a BMS that has gone
  to sleep may not answer and the report goes out without readings.
- The low-voltage guard only sees the pack during those short wakes. Between them
  the bridge draws very little, which is the point, but it is not watching.
- Retained commands are re-delivered on every connect, which is harmless: asking
  for the mode it is already in does nothing.

## Entities (created by discovery)

| Entity | Unit | Notes |
|---|---|---|
| Pack voltage / current / power | V / A / W | current is negative while discharging |
| State of charge | % | |
| Remaining capacity | Ah | |
| Highest / lowest cell, imbalance | mV | |
| Highest / lowest temperature | °C | |
| Battery state | | `idle`, `charging`, `discharging` |
| **Charge / discharge MOSFET** | | switch, see [Switching the MOSFETs](#switching-the-mosfets) |
| Cycles | | |
| **BMS data fresh** | | binary, diagnostic |
| **Seconds since BMS frame** | s | diagnostic |
| **Low-voltage guard** | | `disabled`, `normal`, `counting`, `recheck` |
| Guard seconds spent low | s | diagnostic |
| WiFi signal, uptime | dBm, text ("45 m", "3 h 20 m", "2 d 4 h 24 m") | diagnostic |
| IP address | | diagnostic; the way to find the board when `dalybms.local` does not resolve |
| Cell 1…N | mV | one per cell the BMS reports, diagnostic |
| **Power mode** | | select: `storage`, `normal`, `high` |
| **Pack voltage profile** | | select: `normal`, `storage`; shows none for `custom` or `unknown` |

### Read "BMS data fresh" before trusting anything else

If the BMS stops answering — cable pulled, connector loose, BMS asleep — every
value keeps its **last known number**. Only *BMS data fresh* (off) and *Seconds
since BMS frame* say so. Alarm on it alongside your low-SOC alarm, or a dead
bridge reads as a permanently healthy battery.

If the ESP itself is offline, the broker publishes `offline` and every entity
goes *unavailable*.

### The low-voltage guard is your warning shot

The bridge deep-sleeps when the pack gets low, because it is powered ahead of the
BMS output MOSFETs and would otherwise flatten the battery it is reporting on
([voltguard.h](../src/voltguard.h)).

*Low-voltage guard* becomes `counting` the moment the voltage drops below the trip
threshold and stays there for `GUARD_TRIP_SECONDS` (60 by default) before the
bridge goes quiet. Automate the notification on `counting`, not on the entities
going unavailable afterwards: once asleep, the ESP is off the network, which looks
the same as a crashed bridge.

`recheck` means the bridge woke briefly to re-measure with the radio off. It
never connects to MQTT in that state, so you will not see it.

## Home Assistant side

1. Install the **Mosquitto broker** add-on and the **MQTT** integration.
2. Enable a WebSocket listener on the broker (the add-on's custom config, e.g.
   `listener 1884` / `protocol websockets`), or any broker that supports MQTT
   over WebSocket.
3. Expose it through your reverse proxy on a path, e.g. `/mqtt`, forwarding
   WebSocket upgrades to that listener. Behind nginx:

   ```nginx
   location /mqtt {
       proxy_pass http://homeassistant:1884;
       proxy_http_version 1.1;
       proxy_set_header Upgrade $http_upgrade;
       proxy_set_header Connection "upgrade";
       proxy_read_timeout 3600s;
   }
   ```
4. Create a broker user for the bridge and put it in `src/config.h`.
5. Set `MQTT_HOST`, `MQTT_PORT` (443), `MQTT_PATH` (`/mqtt`) and flash.

The device appears under **Settings → Devices → MQTT → DALY BMS**.

## Two networks (internal and external)

If `WIFI_SSID_EXT` is defined, the bridge tries that network first and falls back
to `WIFI_SSID` after 15 seconds, alternating until one connects. The MQTT server
follows the network: `MQTT_HOST_EXT`/`MQTT_PORT_EXT` on the external one,
`MQTT_HOST`/`MQTT_PORT` on the internal one. After a WiFi drop the search starts
over, external first. The log shows which network and server were chosen.

## TLS

With `MQTT_TLS 1` the connection is encrypted. With an empty
`MQTT_TLS_FINGERPRINT` it does **not** check the server certificate, so it
resists eavesdropping but not an active man-in-the-middle. To pin the server,
set the certificate's SHA-1 fingerprint:

```sh
openssl s_client -connect ha.example.com:443 </dev/null 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha1
```

The fingerprint changes at every certificate renewal (Let's Encrypt: roughly
every 60 days), after which the bridge stops connecting until you reflash or
update it over the air.

With TLS up, the ESP8266 has about 6 kB of heap left, and the handshake dips to about
3.4 kB (see [Memory is the tight resource](#memory-is-the-tight-resource)). The
WebSocket library's TLS receive buffer is cut to 2 kB to make that room; see
`lib/WebSockets/PATCH.md`. Watch `free_heap` in `/status.json`.

## Troubleshooting

All firmware messages are at `http://dalybms.local/log` (UART0 belongs to the BMS).

| Log line | Meaning |
|---|---|
| `mqtt: websocket up, sending CONNECT` then `mqtt: connected` | working |
| `mqtt: broker refused connection, code 4/5` | wrong or unauthorised broker credentials |
| `websocket up` but never `connected` | proxy is not forwarding the `mqtt` subprotocol, or the path is wrong |
| nothing after `mqtt: wss://…` | TLS or DNS failure, or no heap left |

## References

- [MQTT 3.1.1 specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)
- [Home Assistant MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
