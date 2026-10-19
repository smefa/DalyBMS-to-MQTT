# Local patch to links2004/WebSockets 2.7.3

This is a vendored copy of the library (src, library.json, library.properties, LICENSE).
The only change is in `src/WebSocketsClient.cpp`, right after the TLS client is
created: `setBufferSizes(WEBSOCKETS_SSL_RX_BUFFER, 512)` when that macro is defined.

Why: the ESP8266 core's BearSSL client allocates a 16 kB receive buffer by default,
and the library offers no way to change it. With TLS up that left 2-5 kB of heap, and
the WiFi stack crashed (`Exception 29` in `ieee80211_parse_beacon`) when it could not
allocate. `platformio.ini` sets `WEBSOCKETS_SSL_RX_BUFFER=2048`, which frees about 14 kB.

The server must support the TLS max_fragment_length extension, or the handshake
fails. Nginx Proxy Manager did on both addresses (checked 2026-10-12 with a raw
TLS 1.2 ClientHello, reply echoed the requested size for 512 to 4096).
To go back to the stock library, delete this directory, restore
`links2004/WebSockets @ ^2.4.0` in `lib_deps`, and drop the build flag.
