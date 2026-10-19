// Answers mDNS queries for <DEVICE_HOSTNAME>.local with the board's IPv4 address,
// and nothing else: no services, no probing, no IPv6.
//
// The core's ESP8266mDNS is not used on purpose. It parses every mDNS packet on the
// LAN into heap objects, and with TLS up it ran out mid-parse and hung the board.
// This one parses each packet in place inside the receive callback, allocates
// nothing there, and always frees the packet before returning; the answer is
// built in loop(), only when there is heap to spare.
#pragma once

#include <Arduino.h>

namespace mdnslite {

// Call every pass of loop(). Joins the mDNS group when WiFi comes up, sends the
// pending answer, and announces the address after each connect.
void loop(bool wifi_up);

uint32_t received(); // mDNS packets received from the LAN since boot
uint32_t queries();  // queries for our name among them
uint32_t answers();  // answers sent
uint32_t skipped();  // answers dropped for low heap or a failed send

} // namespace mdnslite
