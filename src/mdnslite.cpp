#include "mdnslite.h"

#include <ESP8266WiFi.h>
#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>

#include "applog.h"
#include "config.h"

namespace mdnslite {
namespace {

constexpr uint16_t kPort = 5353;
constexpr uint32_t kTtlS = 120;       // RFC 6762 recommends 120 s for host records
constexpr uint32_t kLegacyTtlS = 10;  // and at most 10 s in a legacy unicast reply
constexpr uint32_t kMcastGapMs = 1000; // RFC 6762: at most one multicast per second
// Below this an answer is not worth the risk: the TLS handshake alone dips the heap by
// a few kB, and an out-of-memory there reboots the board.
constexpr uint32_t kMinHeap = 4096;

// "<hostname>.local" in DNS wire format, lower case, with the terminating zero label.
uint8_t name_[1 + 63 + 1 + 5 + 1];
uint8_t name_len_ = 0;

constexpr uint32_t kRetryMs = 10000;   // socket setup retry after a failure

udp_pcb* pcb_ = nullptr;
bool was_up_ = false;
bool ready_ = false;       // socket bound and group joined on this connection
bool tried_ = false;       // a setup attempt was made on this connection
uint32_t tried_at_ = 0;

// Set by the receive callback, sent from loop(). The callback runs in the SDK's
// task, which never interrupts loop() (and vice versa), so plain variables do.
bool mcast_pending_ = false;
// One unicast reply: to a legacy resolver, or to a query sent straight to our IP.
bool uni_pending_ = false;
bool uni_legacy_ = false;
uint16_t uni_id_ = 0;
uint16_t uni_type_ = 1;
ip_addr_t uni_addr_;
uint16_t uni_port_ = 0;

uint8_t announce_left_ = 0;
bool mcast_sent_ = false;
uint32_t last_mcast_at_ = 0;

uint32_t rx_ = 0;
uint32_t queries_ = 0;
uint32_t answers_ = 0;
uint32_t skipped_ = 0;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

uint8_t* put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
    return p + 2;
}

void buildName() {
    const char* host = DEVICE_HOSTNAME;
    uint8_t n = 0;
    while (host[n] != '\0' && n < 63) n++;
    uint8_t* p = name_;
    *p++ = n;
    for (uint8_t i = 0; i < n; i++) {
        char c = host[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        *p++ = static_cast<uint8_t>(c);
    }
    *p++ = 5;
    memcpy(p, "local", 5);
    p += 5;
    *p++ = 0;
    name_len_ = static_cast<uint8_t>(p - name_);
}

// Compares the name at `off` with ours, following compression pointers, and moves
// `off` past it. 1 = ours, 0 = another name, -1 = malformed or cut off.
int matchName(const uint8_t* b, size_t len, size_t& off) {
    size_t pos = off;
    size_t end = 0;
    size_t e = 0; // position in name_
    bool jumped = false;
    bool same = true;
    uint8_t hops = 0;
    for (;;) {
        if (pos >= len) return -1;
        const uint8_t c = b[pos];
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 8) return -1;
            if (!jumped) {
                end = pos + 2;
                jumped = true;
            }
            pos = static_cast<size_t>(c & 0x3F) << 8 | b[pos + 1];
            continue;
        }
        if (c & 0xC0) return -1;
        if (pos + 1 + c > len) return -1;
        if (same) {
            if (e + 1 + c > name_len_ || name_[e] != c) {
                same = false;
            } else {
                for (uint8_t k = 0; k < c && same; k++) {
                    uint8_t ch = b[pos + 1 + k];
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<uint8_t>(ch + ('a' - 'A'));
                    same = ch == name_[e + 1 + k];
                }
                e += 1 + c;
            }
        }
        pos += 1 + c;
        if (c == 0) {
            off = jumped ? end : pos;
            return same ? 1 : 0;
        }
    }
}

// Runs inside lwIP: no allocation, no logging, nothing that can block.
void handle(const uint8_t* b, size_t len, const ip_addr_t* from, uint16_t port) {
    if (len < 12) return;
    const uint16_t flags = be16(b + 2);
    if ((flags & 0x8000) || (flags & 0x7800)) return; // a response, or not a standard query
    uint16_t count = be16(b + 4);
    if (count > 8) count = 8;
    size_t off = 12;
    for (uint16_t i = 0; i < count; i++) {
        const int r = matchName(b, len, off);
        if (r < 0 || off + 4 > len) return;
        const uint16_t type = be16(b + off);
        const uint16_t cls = be16(b + off + 2) & 0x7FFF; // top bit: unicast response wanted
        off += 4;
        if (r == 1 && (type == 1 || type == 255) && (cls == 1 || cls == 255)) {
            queries_++;
            if (port == kPort && ip4_addr_ismulticast(ip4_current_dest_addr())) {
                // A multicast answer serves a "unicast response wanted" query too.
                mcast_pending_ = true;
            } else {
                // Sent to our address rather than the group, so the asker may not be
                // listening on it (RFC 6762 5.5). A legacy resolver (one-shot query from
                // an ephemeral port) also wants its ID and question echoed (6.7).
                uni_pending_ = true;
                uni_legacy_ = port != kPort;
                uni_id_ = be16(b);
                uni_type_ = type;
                ip_addr_copy(uni_addr_, *from);
                uni_port_ = port;
            }
            return;
        }
    }
}

void onPacket(void*, udp_pcb*, pbuf* p, const ip_addr_t* from, u16_t port) {
    if (p == nullptr) return;
    rx_++;
    // A query small enough to matter fits the first segment; parse it in place.
    handle(static_cast<const uint8_t*>(p->payload), p->len, from, port);
    pbuf_free(p);
}

void joinGroup() {
    ip_addr_t group;
    IP_ADDR4(&group, 224, 0, 0, 251);
    for (netif* n = netif_list; n != nullptr; n = n->next) {
        if (!netif_is_up(n) || ip4_addr_isany_val(*netif_ip4_addr(n))) continue;
        if (!(n->flags & NETIF_FLAG_IGMP)) {
            n->flags |= NETIF_FLAG_IGMP;
            igmp_start(n);
        }
        // Leave first: a reconnect keeps the membership, and every join adds a user.
        igmp_leavegroup_netif(n, ip_2_ip4(&group));
        igmp_joingroup_netif(n, ip_2_ip4(&group));
    }
}

// `quiet` keeps a retry from logging the same failure every few seconds.
bool start(bool quiet) {
    if (name_len_ == 0) buildName();
    if (pcb_ == nullptr) {
        pcb_ = udp_new();
        if (pcb_ == nullptr) {
            if (!quiet) APPLOG("mdns: no memory for socket");
            return false;
        }
        if (udp_bind(pcb_, IP_ANY_TYPE, kPort) != ERR_OK) {
            if (!quiet) APPLOG("mdns: bind failed");
            udp_remove(pcb_);
            pcb_ = nullptr;
            return false;
        }
        udp_set_multicast_ttl(pcb_, 255);
        udp_recv(pcb_, onPacket, nullptr);
    }
    joinGroup();
    APPLOG("mdns: %s.local, heap=%u", DEVICE_HOSTNAME, ESP.getFreeHeap());
    return true;
}

bool send(const ip_addr_t* to, uint16_t port, bool legacy) {
    uint8_t msg[12 + 2 * sizeof(name_) + 4 + 14];
    uint8_t* p = msg;
    p = put16(p, legacy ? uni_id_ : 0);
    p = put16(p, 0x8400); // response, authoritative
    p = put16(p, legacy ? 1 : 0);
    p = put16(p, 1);
    p = put16(p, 0);
    p = put16(p, 0);
    if (legacy) {
        memcpy(p, name_, name_len_);
        p += name_len_;
        p = put16(p, uni_type_);
        p = put16(p, 1);
    }
    memcpy(p, name_, name_len_);
    p += name_len_;
    p = put16(p, 1);                         // A
    p = put16(p, legacy ? 0x0001 : 0x8001); // IN, plus cache-flush when multicast
    const uint32_t ttl = legacy ? kLegacyTtlS : kTtlS;
    p = put16(p, static_cast<uint16_t>(ttl >> 16));
    p = put16(p, static_cast<uint16_t>(ttl));
    p = put16(p, 4);
    const IPAddress ip = WiFi.localIP();
    for (uint8_t i = 0; i < 4; i++) *p++ = ip[i];
    const size_t n = static_cast<size_t>(p - msg);

    if (ESP.getFreeHeap() < kMinHeap) {
        skipped_++;
        return false;
    }
    pbuf* pb = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(n), PBUF_RAM);
    if (pb == nullptr) {
        skipped_++;
        return false;
    }
    memcpy(pb->payload, msg, n);
    const err_t err = udp_sendto(pcb_, pb, to, port);
    pbuf_free(pb);
    if (err != ERR_OK) {
        skipped_++;
        return false;
    }
    answers_++;
    return true;
}

} // namespace

void loop(bool wifi_up) {
    if (!wifi_up) {
        was_up_ = false;
        return;
    }
    if (!was_up_) {
        was_up_ = true;
        ready_ = false;
        tried_ = false;
        // Announce twice, a second apart (RFC 6762 8.3), so caches that still hold an
        // old address for the name pick up the new one.
        announce_left_ = 2;
    }
    if (!ready_) {
        if (tried_ && millis() - tried_at_ < kRetryMs) return;
        ready_ = start(tried_);
        tried_ = true;
        tried_at_ = millis();
        if (!ready_) return;
    }

    if (uni_pending_) {
        uni_pending_ = false;
        send(&uni_addr_, uni_port_, uni_legacy_);
    }
    if ((mcast_pending_ || announce_left_ > 0) &&
        (!mcast_sent_ || millis() - last_mcast_at_ >= kMcastGapMs)) {
        ip_addr_t group;
        IP_ADDR4(&group, 224, 0, 0, 251);
        send(&group, kPort, false);
        // Dropped, not retried, when it fails: the asker repeats its query.
        mcast_pending_ = false;
        if (announce_left_ > 0) announce_left_--;
        mcast_sent_ = true;
        last_mcast_at_ = millis();
    }
}

uint32_t received() { return rx_; }
uint32_t queries() { return queries_; }
uint32_t answers() { return answers_; }
uint32_t skipped() { return skipped_; }

} // namespace mdnslite
