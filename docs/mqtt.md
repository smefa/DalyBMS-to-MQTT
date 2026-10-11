# Home Assistant over MQTT (WebSocket)

The bridge speaks MQTT 3.1.1 inside binary WebSocket frames (subprotocol
`mqtt`), over `wss://` by default. That lets it reach a broker through a reverse
proxy on port 443, which is the usual way to expose Home Assistant over HTTPS.

It publishes everything, and takes exactly one command back: the power mode
(below). The broker user therefore needs **write** access to `dalybms/#` and
**read** access to `dalybms/mode/set`. Without the read access the bridge logs
`broker refused subscribe` and carries on, but the mode cannot be changed.

## Topics

With `MQTT_BASE_TOPIC "dalybms"`:

| Topic | Direction | Retained | Content |
|---|---|---|---|
| `dalybms/status` | out | yes | `online`; the broker publishes `offline` as the last will |
| `dalybms/state` | out | storage mode only | one JSON document with every value, including `mode` |
| `dalybms/mode/set` | **in** | should be | `storage`, `normal` or `high` (any case) |
| `homeassistant/<component>/dalybms/<id>/config` | out | yes | MQTT discovery |

The state document is also sent immediately whenever the low-voltage guard
changes state, or the mode changes between normal and high.

## Power modes

The **Power mode** entity (a select, under the device's configuration section) or
a publish to `dalybms/mode/set` chooses one. The choice is kept in flash and
survives power loss; a board that has never been told starts in `normal`
(`POWER_DEFAULT_MODE`).

| Mode | Radio | Reports | Dashboard / OTA |
|---|---|---|---|
| `high` | on, modem sleep | every 60 s | ~0.1–0.3 s latency |
| `normal` | on, light sleep (once MQTT has connected and caught up) | every 15 min | ~1 s latency, sometimes a few |
| `storage` | **off** | once a day | only for a short window after each report |

Intervals are `POWER_*` settings in `config.h`. I have not measured the current
draw of any of them on a real board; expect roughly 20 mA for high, a few mA for
normal and well under 1 mA for storage, depending on the regulator and USB chip
on your board.

**Normal** still polls the BMS every 60 s (`POWER_NORMAL_POLL_S`) between
reports, because the low-voltage guard needs a stream of readings to act on.
While the guard is counting down it polls every `POLL_INTERVAL_MS` instead, so a
passing sag gets cleared by fresh readings rather than tripping it.

Between polls the CPU light-sleeps (`POWER_NORMAL_LIGHT_SLEEP`) and the radio wakes
for the access point's DTIM beacons. Light sleep starts only 10 s after MQTT has
settled: connected, discovery sent, first state out. The TLS handshake is a long
blocking call that took 40 s under a slow radio on the bench, long enough to trip
the watchdog. And entering light sleep during the burst of publishes after a
connect gave a Soft WDT reset in roughly one boot in five to ten. A dropped connection goes
back to full speed until it has settled again.

`POWER_NORMAL_LISTEN_INTERVAL` would stretch the radio's wakeups past DTIM, but the
core only applies it on NONOS SDK 3.x
(`-D PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK305`). The default SDK 2.2.x ignores it.

### Memory is the tight resource

With TLS up the ESP8266 has about 6 kB of heap left, and the TLS handshake itself
dips to about 3.4 kB. Anything that asks for memory then can crash the board with
`Exception 29` or `Unhandled C++ exception: OOM`. A dashboard that fires a request
every 2 s while the answers take seconds does exactly that, so the dashboard now
polls one request at a time, every 10 s outside high mode. If you add features,
watch `free_heap` in `/status.json`, or `http://<ip>/heap`, which answers even when
the heap is too low to build the JSON.

**Storage** is deep sleep. A day is longer than the chip can sleep in one go, so it
sleeps in chunks of an hour or so and only brings up the radio after the last
one. On that wake it connects, publishes the state (retained, so a restarted Home
Assistant still has numbers), sends discovery, reads any waiting mode command,
disconnects cleanly and sleeps again. A clean disconnect leaves `status` as
`online`; the entities expire after twice the storage period
(`POWER_STORAGE_INTERVAL_S`) so a bridge that died asleep does not look healthy
for ever. If a report cannot get through it is retried after
`POWER_STORAGE_RETRY_S` (3 h).

Things to know about storage:

- **Needs D0 wired to RST**, like the low-voltage guard ([wiring](wiring.md)).
  Without it the bridge goes to sleep and never wakes until power-cycled, and
  then goes straight back to sleep.
- **Switching out of storage takes up to a day.** The command sits retained on the
  broker (Home Assistant retains it) until the next wake. To act sooner, press the
  reset button or power-cycle the bridge: a storage bridge that did not wake from
  a timer stays up for `POWER_STORAGE_GRACE_S` (2 min) after reporting, long
  enough to collect the command, or to flash over the air.
- **Switching into storage is immediate:** the bridge saves the mode, restarts,
  reports once and sleeps.
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
| Charge / discharge MOSFET | | binary |
| Cycles | | |
| **BMS data fresh** | | binary, diagnostic |
| **Seconds since BMS frame** | s | diagnostic |
| **Low-voltage guard** | | `disabled`, `normal`, `counting`, `recheck` |
| Guard seconds spent low | s | diagnostic |
| WiFi signal, uptime | dBm, s | diagnostic |
| Cell 1…N | mV | one per cell the BMS reports, diagnostic |
| **Power mode** | | select: `storage`, `normal`, `high` |

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

TLS needs about 20 kB of RAM on the ESP8266. Watch `free_heap` in
`/status.json`; if it falls below ~10 kB, TLS handshakes will start to fail.

## Troubleshooting

All firmware messages are at `http://<esp-ip>/log` (UART0 belongs to the BMS).

| Log line | Meaning |
|---|---|
| `mqtt: websocket up, sending CONNECT` then `mqtt: connected` | working |
| `mqtt: broker refused connection, code 4/5` | wrong or unauthorised broker credentials |
| `websocket up` but never `connected` | proxy is not forwarding the `mqtt` subprotocol, or the path is wrong |
| nothing after `mqtt: wss://…` | TLS or DNS failure, or no heap left |
