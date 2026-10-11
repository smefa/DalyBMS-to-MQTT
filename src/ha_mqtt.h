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
// See docs/mqtt.md.
#pragma once

#include "daly_bms.h"

namespace ha_mqtt {

// `bms` must outlive this module. Call once WiFi is being brought up; the
// WebSocket connects on its own once the network is available.
void begin(const DalyBms* bms);

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

} // namespace ha_mqtt
