// DALY UART pin finder -- a bench tool, NOT the production firmware.
//
// Wire the (up to three) unknown BMS signal pins to D5, D6 and D1 on the D1
// Mini, ground to GND, open http://<ip>/ and press "Auto-detect". The page tries
// every TX/RX assignment of those pins and shows the raw bytes, so you learn
// which BMS pin is TX and which is RX without guessing.
//
// Runs on SoftwareSerial so hardware UART0 stays free for USB debug output
// (115200 baud). Build/flash with:  pio run -e uarttest -t upload
//
// The ESP8266 is 3.3 V only. Meter the BMS pins first: if the BMS TX idles at 5 V
// it needs a divider before it reaches a D1 pin.

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>

#include "daly_protocol.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "src/config.h is missing. Copy src/config.h.example to src/config.h and fill it in."
#endif

namespace {

ESP8266WebServer server(80);
SoftwareSerial sw;

// Pins that are safe to use: not boot-strap (0/2/15), not GPIO16 (no interrupts).
// D1=5, D2=4, D5=14, D6=12, D7=13.
bool pinAllowed(int p) { return p == 4 || p == 5 || p == 12 || p == 13 || p == 14; }

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DALY UART test</title>
<style>
 body{font:15px system-ui,sans-serif;margin:0;padding:16px;max-width:720px;margin-inline:auto;background:#fff;color:#111}
 @media(prefers-color-scheme:dark){body{background:#15171a;color:#e6e6e6}pre{background:#0c0d0f!important}}
 h1{font-size:20px;margin:0 0 4px}
 p.sub{margin:0 0 14px;opacity:.7}
 fieldset{border:1px solid #8884;border-radius:8px;margin:0 0 12px;padding:10px 12px}
 legend{padding:0 6px;opacity:.7}
 label{margin-right:12px;white-space:nowrap}
 select,button{font:inherit;padding:6px 10px;border-radius:6px;border:1px solid #8886;background:#8881;color:inherit}
 button{cursor:pointer} button.primary{background:#2563eb;color:#fff;border-color:#2563eb}
 button:disabled{opacity:.5;cursor:wait}
 pre{background:#f3f4f6;padding:10px;border-radius:8px;overflow:auto;min-height:8em;font-size:13px;white-space:pre-wrap;word-break:break-all}
 .ok{color:#16a34a;font-weight:600}.bad{color:#dc2626}
 .verdict{padding:10px 12px;border-radius:8px;margin:0 0 12px;background:#8881}
</style></head><body>
<h1>DALY UART test</h1>
<p class="sub">Wire the unknown BMS signal pins to <b>D5</b>, <b>D6</b> and <b>D1</b>, BMS GND to GND. Leave the supply pin unconnected.</p>

<div id="verdict" class="verdict">Not tested yet.</div>

<fieldset><legend>Probe</legend>
 <label>Command
  <select id="cmd">
   <option value="90">0x90 pack V / A / SOC</option>
   <option value="91">0x91 min/max cell</option>
   <option value="92">0x92 temperature</option>
   <option value="93">0x93 MOS state</option>
   <option value="94">0x94 status</option>
   <option value="95">0x95 cell voltages</option>
   <option value="">listen only</option>
  </select></label>
 <label>Address <select id="addr"><option value="40">0x40 (UART)</option><option value="80">0x80 (BT/WiFi)</option></select></label>
 <label>Baud <select id="baud"><option>9600</option><option>19200</option><option>4800</option><option>115200</option></select></label>
 <p>
  <button class="primary" id="auto">Auto-detect (all 6 combinations)</button>
 </p>
</fieldset>

<fieldset><legend>Log</legend><pre id="log"></pre></fieldset>

<script>
// D5=GPIO14, D6=GPIO12, D1=GPIO5. The D1 listens (rx) on the pin the BMS transmits on.
const PINS=[{n:'D5',g:14},{n:'D6',g:12},{n:'D1',g:5}];
const COMBOS=[];
for(const t of PINS)for(const r of PINS)if(t!==r)COMBOS.push({rx:t.g,tx:r.g,name:'BMS TX on '+t.n+', BMS RX on '+r.n});
const $=id=>document.getElementById(id), log=$('log');
const say=(t,c)=>{const s=document.createElement('span');if(c)s.className=c;s.textContent=t+'\n';log.appendChild(s);log.scrollTop=log.scrollHeight};

async function probe(o){
  const q=new URLSearchParams({rx:o.rx,tx:o.tx,baud:$('baud').value,cmd:$('cmd').value,addr:$('addr').value});
  const r=await fetch('/api/probe?'+q); if(!r.ok) throw new Error(await r.text());
  const j=await r.json();
  say('--- '+o.name+' @ '+j.baud);
  say('idle level before open: D5='+j.lvl_d5+' D6='+j.lvl_d6+' D1='+j.lvl_d1);
  if(j.tx_hex) say('TX '+j.tx_hex);
  say('RX '+(j.rx_len?j.rx_hex:'(nothing)')+'  ['+j.rx_len+' bytes]');
  if(j.frames) say('valid DALY frames: '+j.frames+(j.summary?'  ->  '+j.summary:''),'ok');
  else if(j.rx_len) say('bytes received but no valid frame: wrong baud, noise, or pins swapped mid-frame','bad');
  return j;
}
function verdict(html,c){const v=$('verdict');v.innerHTML=html;v.className='verdict '+(c||'')}

$('auto').onclick=async()=>{
  $('auto').disabled=true;log.textContent='';
  try{
    let hit=null;
    for(const o of COMBOS){const j=await probe(o); if(j.frames){hit=o;break}}
    if(hit) verdict('<span class="ok">Found it.</span> '+hit.name+'.');
    else verdict('<span class="bad">No valid reply on any combination.</span> Check: BMS awake? GND common? Pins really wired to D5, D6, D1? Try another baud or &ldquo;listen only&rdquo;.');
  }catch(e){say('error: '+e.message,'bad')}
  $('auto').disabled=false;
};
</script></body></html>)HTML";

String hex(const uint8_t* b, size_t n) {
    static const char* d = "0123456789ABCDEF";
    String s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        if (i) s += ' ';
        s += d[b[i] >> 4];
        s += d[b[i] & 15];
    }
    return s;
}

void handleProbe() {
    const int rx = server.arg("rx").toInt();
    const int tx = server.arg("tx").toInt();
    const uint32_t baud = server.arg("baud").toInt();
    const String cmdArg = server.arg("cmd");

    if (!pinAllowed(rx) || !pinAllowed(tx) || rx == tx || baud < 1200 || baud > 115200) {
        server.send(400, "text/plain", "bad pin or baud\n");
        return;
    }

    // Snapshot the line levels before the UART grabs the pins. A BMS TX line
    // idles high; an undriven pin floats (reads 0 or flickers).
    pinMode(14, INPUT);
    pinMode(12, INPUT);
    pinMode(5, INPUT);
    delay(5);
    const int lvl14 = digitalRead(14);
    const int lvl12 = digitalRead(12);
    const int lvl5 = digitalRead(5);

    sw.begin(baud, SWSERIAL_8N1, rx, tx, false, 128);
    delay(20);
    while (sw.available()) sw.read();

    uint8_t req[daly::FRAME_LEN];
    bool sent = false;
    if (cmdArg.length()) {
        const uint8_t cmd = static_cast<uint8_t>(strtoul(cmdArg.c_str(), nullptr, 16));
        daly::buildRequest(cmd, req);
        const uint8_t addr = static_cast<uint8_t>(strtoul(server.arg("addr").c_str(), nullptr, 16));
        if (addr) {
            req[1] = addr;
            req[daly::FRAME_LEN - 1] = daly::checksum(req, daly::FRAME_LEN - 1);
        }
        sw.write(req, daly::FRAME_LEN);
        sent = true;
    }

    // Collect for up to 500 ms (listen-only: 1.5 s, to catch slow chatter).
    uint8_t buf[64];
    size_t n = 0;
    daly::FrameDecoder dec;
    daly::BmsData data;
    int frames = 0;
    bool haveSummary = false;
    uint8_t replyAddr = 0;
    const uint32_t window = sent ? 500 : 1500;
    const uint32_t t0 = millis();
    while (millis() - t0 < window) {
        while (sw.available()) {
            const uint8_t b = static_cast<uint8_t>(sw.read());
            if (n < sizeof(buf)) buf[n++] = b;
            if (dec.feed(b)) {
                frames++;
                replyAddr = dec.frame()[1];
                if (daly::applyFrame(dec.frame(), data) == daly::CMD_PACK) haveSummary = true;
            }
        }
        delay(1); // yields to WiFi
    }
    sw.end();

    String out;
    out.reserve(320);
    out += F("{\"rx\":");
    out += rx;
    out += F(",\"tx\":");
    out += tx;
    out += F(",\"baud\":");
    out += baud;
    out += F(",\"lvl_d5\":");
    out += lvl14;
    out += F(",\"lvl_d6\":");
    out += lvl12;
    out += F(",\"lvl_d1\":");
    out += lvl5;
    out += F(",\"tx_hex\":\"");
    if (sent) out += hex(req, daly::FRAME_LEN);
    out += F("\",\"rx_len\":");
    out += static_cast<unsigned>(n);
    out += F(",\"rx_hex\":\"");
    out += hex(buf, n);
    out += F("\",\"frames\":");
    out += frames;
    out += F(",\"summary\":\"");
    if (frames) {
        char s[96];
        if (haveSummary) {
            snprintf(s, sizeof(s), "reply addr 0x%02X, pack %.1f V, %.1f A, SOC %.1f %%", replyAddr,
                     data.pack_voltage_dv / 10.0, data.pack_current_da / 10.0, data.soc_dpct / 10.0);
        } else {
            snprintf(s, sizeof(s), "reply addr 0x%02X", replyAddr);
        }
        out += s;
    }
    out += F("\"}");

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void startWifi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("\nconnecting to %s", WIFI_SSID);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(250);
        Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nhttp://%s/\n", WiFi.localIP().toString().c_str());
        return;
    }
    // No router in reach (workbench): host our own network instead.
    WiFi.mode(WIFI_AP);
    WiFi.softAP("dalytest");
    Serial.printf("\nno wifi; AP 'dalytest' up, http://%s/\n", WiFi.softAPIP().toString().c_str());
}

} // namespace

void setup() {
    Serial.begin(115200); // USB debug only; the BMS is on SoftwareSerial
    startWifi();

    server.on("/", HTTP_GET, []() { server.send_P(200, PSTR("text/html; charset=utf-8"), PAGE); });
    server.on("/api/probe", HTTP_GET, handleProbe);
    server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });
    server.begin();
}

void loop() {
    server.handleClient();
}
