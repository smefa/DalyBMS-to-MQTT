// Publishes the BMS to Home Assistant over MQTT carried on a WebSocket
// (ws:// or wss://), with MQTT discovery so the entities appear on their own.
//
// Topics, with MQTT_BASE_TOPIC = "dalybms":
//   dalybms/status   "online" / "offline", retained; "offline" is also the will
//   dalybms/state    one JSON document with every value, published periodically
//
//   dalybms/mode/set "storage" / "normal" / "high": the one command it accepts,
//                    which switches the power mode (src/powermode.h)
//
//   dalybms/limits   the BMS's voltage limits as JSON, retained, republished when they change
//   dalybms/limits/set     JSON {"pin":"your-pin","cell_high_l1_mv":3650,...} changes them
//   dalybms/limits/result  outcome of the last command: queued / done / mismatch / why refused
//                          (not retained; never publish the command retained either)
//
// See docs/mqtt.md.
#pragma once

#include "daly_bms.h"

namespace ha_mqtt {

// `bms` must outlive this module. Call once WiFi is being brought up; the
// WebSocket connects on its own once the network is available.
void begin(DalyBms* bms);

// Chooses between MQTT_HOST/MQTT_PORT (internal) and MQTT_HOST_EXT/MQTT_PORT_EXT.
// Call whenever WiFi comes up; reconnects only if the choice actually changed.
// Without MQTT_HOST_EXT defined, the internal server is always used.
void selectServer(bool external);

// Call every loop() pass.
void loop();

// Send the state document on the next loop() pass instead of waiting out the interval.
void publishNow();

// A storage session is done when this connection has published the state, sent
// all discovery, and had time to receive a retained mode command.
bool sessionComplete();

// A limits change is accepted but not yet confirmed by the BMS; do not sleep.
bool writePending();

// True while a guard change, or a report that failed, has not gone out yet. A session
// that ends now would leave Home Assistant on the old state until the next wake.
bool reportPending();

// The burst that follows a connect is over: state published, discovery sent and
// the subscribe answered. From here on traffic is the occasional publish or ping.
bool settled();

// Close the MQTT session cleanly (the broker keeps "online" and publishes no
// will), flush, and drop the socket. Blocks for about 200 ms. No-op when not connected.
void goodbye();

// Which server selectServer() picked: "internal", "external", or "none" yet.
const char* serverName();

// Host of that server, "" if none yet.
const char* serverHost();

// True between a CONNACK and the next disconnect.
bool connected();

// How long MQTT has shown no sign of life: since the last state document went out (or
// the last CONNACK), and, with pings on, since the broker last sent anything. Grows
// while disconnected, WiFi down included, so callers clamp it to their own WiFi-up time.
// 0 when no server is chosen or the config blocks connecting.
uint32_t msSilent();

} // namespace ha_mqtt
