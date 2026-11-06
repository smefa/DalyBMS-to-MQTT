// DALY BMS card, in the style of ha-plex-info. Reads the MQTT-discovered entities of
// the bridge. Optional: title, device_id, entities (key -> entity_id overrides),
// cell_min_mv / cell_max_mv (cell bar scale), show_* switches (see SECTIONS).
const TOKENS = `
  :host {
    display: block;
    --daly-accent: var(--primary-color, #03a9f4);
    --daly-ok: var(--success-color, #43a047);
    --daly-warn: var(--warning-color, #ffa600);
    --daly-bad: var(--error-color, #db4437);
    --daly-dim: var(--secondary-text-color, #727272);
    --daly-surface: rgba(127, 127, 127, .10);
    --daly-surface: color-mix(in srgb, var(--primary-text-color) 7%, transparent);
    --daly-hairline: var(--divider-color, rgba(127, 127, 127, .3));
  }
  * { box-sizing: border-box; }
  ha-card { overflow: hidden; }
  .pad { padding: 14px 16px 16px 16px; }
  .head { display: flex; align-items: center; gap: 12px; }
  .grow { flex: 1; min-width: 0; }
  .name { font-size: 1.05rem; font-weight: 500; line-height: 1.25; }
  .sub { font-size: .78rem; color: var(--daly-dim); margin-top: 2px;
         white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .sq { width: 40px; height: 40px; border-radius: 13px; flex: 0 0 auto; display: grid;
        place-items: center; color: var(--daly-dim); background: var(--daly-surface); }
  .sq ha-icon { --mdc-icon-size: 22px; }
  .sq.on { color: var(--daly-ok); background: rgba(67, 160, 71, .18);
           background: color-mix(in srgb, var(--daly-ok) 18%, transparent); }
  .sq.warn { color: var(--daly-warn); background: rgba(255, 166, 0, .18);
             background: color-mix(in srgb, var(--daly-warn) 18%, transparent); }
  .sq.bad { color: var(--daly-bad); background: rgba(219, 68, 55, .18);
            background: color-mix(in srgb, var(--daly-bad) 18%, transparent); }
  .pill { display: inline-flex; align-items: center; gap: 6px; flex: 0 0 auto;
          font-size: .76rem; font-weight: 500; padding: 4px 10px 4px 8px;
          border-radius: 999px; background: var(--daly-surface); color: var(--daly-dim); }
  .pill .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .pill.on { color: var(--daly-ok); background: rgba(67, 160, 71, .16);
             background: color-mix(in srgb, var(--daly-ok) 16%, transparent); }
  .pill.warn { color: var(--daly-warn); background: rgba(255, 166, 0, .16);
               background: color-mix(in srgb, var(--daly-warn) 16%, transparent); }
  .pill.bad { color: var(--daly-bad); background: rgba(219, 68, 55, .16);
              background: color-mix(in srgb, var(--daly-bad) 16%, transparent); }
  .stats { display: grid; gap: 8px; margin-top: 12px;
           grid-template-columns: repeat(auto-fit, minmax(84px, 1fr)); }
  .stat { background: var(--daly-surface); border-radius: 11px; padding: 8px 10px; min-width: 0;
          cursor: pointer; transition: background 160ms; }
  .stat:hover { background: rgba(127, 127, 127, .18); }
  .stat .k { font-size: .68rem; color: var(--daly-dim); text-transform: uppercase;
             letter-spacing: .04em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .stat .v { font-size: 1.3rem; font-weight: 500; margin-top: 2px; font-variant-numeric: tabular-nums; }
  .stat .v small { font-size: .75rem; font-weight: 400; color: var(--daly-dim); }
  .section { margin-top: 16px; font-size: .72rem; color: var(--daly-dim);
             text-transform: uppercase; letter-spacing: .06em; display: flex; gap: 8px; }
  .section .aside { margin-left: auto; text-transform: none; letter-spacing: normal; }
  .soc { height: 14px; margin-top: 8px; border-radius: 7px; background: var(--daly-surface); overflow: hidden; }
  .soc i { display: block; height: 100%; border-radius: 7px; }
  .cells { display: grid; gap: 6px; margin-top: 8px; }
  .cell { display: flex; align-items: center; gap: 10px; font-size: .8rem; cursor: pointer; }
  .cell .n { width: 3.2em; color: var(--daly-dim); }
  .cell .track { flex: 1; height: 8px; border-radius: 4px; background: var(--daly-surface); overflow: hidden; }
  .cell .track i { display: block; height: 100%; background: var(--daly-accent); }
  .cell.hi .track i { background: var(--daly-warn); }
  .cell.lo .track i { background: #4fa3d1; }
  .cell .mv { width: 4.6em; text-align: right; font-variant-numeric: tabular-nums; }
  .switches { display: flex; gap: 8px; margin-top: 8px; flex-wrap: wrap; }
  .sw { flex: 1; min-width: 120px; display: flex; align-items: center; gap: 8px; padding: 8px 12px;
        border: 0; border-radius: 11px; font: inherit; font-size: .85rem; cursor: pointer; text-align: left;
        color: var(--daly-dim); background: var(--daly-surface); }
  .sw.on { color: var(--daly-ok); background: rgba(67, 160, 71, .16);
           background: color-mix(in srgb, var(--daly-ok) 16%, transparent); }
  .sw ha-icon { --mdc-icon-size: 20px; }
  .sw[disabled] { opacity: .5; cursor: default; }
  .kv { display: flex; flex-wrap: wrap; gap: 4px 14px; margin-top: 8px; font-size: .8rem; }
  .kv span { color: var(--daly-dim); }
  .kv b { font-weight: 500; color: var(--primary-text-color); }
  .empty { color: var(--daly-dim); font-size: .88rem; padding: 10px 0 2px; }
  .banner { margin-top: 16px; padding: 10px 14px; border-radius: 12px; font-size: .86rem;
            display: flex; align-items: center; gap: 10px; color: var(--daly-warn);
            background: rgba(255, 166, 0, .14);
            background: color-mix(in srgb, var(--daly-warn) 14%, transparent); }
  .banner.bad { color: var(--daly-bad); background: rgba(219, 68, 55, .14);
                background: color-mix(in srgb, var(--daly-bad) 14%, transparent); }
  .banner ha-icon { --mdc-icon-size: 20px; flex: 0 0 auto; }
`;

// key -> [domain, entity name slug]. The slug is what Home Assistant derives the entity
// ID from. The card matches the end of the entity ID, so an ID renamed to something else
// (or a renamed entity) needs an `entities` override; device_id only picks the device.
const ENTITY_SLUGS = {
  voltage: ["sensor", "pack_voltage"], current: ["sensor", "pack_current"],
  power: ["sensor", "pack_power"], soc: ["sensor", "state_of_charge"],
  remaining_ah: ["sensor", "remaining_capacity"], cell_max_mv: ["sensor", "highest_cell"],
  cell_min_mv: ["sensor", "lowest_cell"], cell_delta_mv: ["sensor", "cell_imbalance"],
  temp_max: ["sensor", "highest_temperature"], temp_min: ["sensor", "lowest_temperature"],
  state: ["sensor", "battery_state"], cycles: ["sensor", "cycles"],
  charge_mos: ["switch", "charge_mosfet"], discharge_mos: ["switch", "discharge_mosfet"],
  bms_ok: ["binary_sensor", "bms_data_fresh"], stale_s: ["sensor", "seconds_since_bms_frame"],
  guard: ["sensor", "low_voltage_guard"], rssi: ["sensor", "wifi_signal"],
  uptime: ["sensor", "uptime"], ip: ["sensor", "ip_address"],
  mode: ["select", "power_mode"], profile: ["select", "pack_voltage_profile"],
};
const SECTIONS = [
  ["show_header", true], ["show_summary", true], ["show_status", true],
  ["show_soc", true], ["show_tiles", true], ["show_cells", true],
  ["show_temps", true], ["show_switches", true], ["show_info", true],
];
const enabled = (config, key) => config[key] ?? SECTIONS.find(s => s[0] === key)?.[1] ?? true;
const esc = (value) => String(value ?? "").replace(/[&<>"']/g,
  (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const BAD = ["unavailable", "unknown", ""];
const ago = (ms) => {
  const m = Math.floor(Math.max(0, ms) / 60000);
  return m < 1 ? "just now" : m < 60 ? `${m} min ago` : m < 2880 ? `${Math.floor(m / 60)} h ago` : `${Math.floor(m / 1440)} d ago`;
};

class DalyBmsCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._sig = "";
  }
  setConfig(config) {
    this._config = config || {};
    this._sig = "";
    if (this._hass) this.hass = this._hass;
  }
  getCardSize() { return 8; }
  static getStubConfig() { return {}; }

  // Finds the bridge's entities: the device is `device_id`, else the first one named
  // "DALY BMS" (the discovery name), else the first MQTT device with a "pack_voltage" entity.
  _entities(hass) {
    const config = this._config || {};
    const byDevice = {};
    Object.entries(hass.entities || {}).forEach(([id, info]) => {
      if (info.platform !== "mqtt" || !info.device_id) return;
      (byDevice[info.device_id] ||= []).push(id);
    });
    let device = config.device_id;
    if (!device) {
      device = Object.keys(byDevice).find(d => hass.devices?.[d]?.name === "DALY BMS")
        || Object.keys(byDevice).find(d => byDevice[d].some(id => id.endsWith("_pack_voltage")));
    }
    const ids = byDevice[device] || [];
    const found = {};
    for (const [key, [domain, slug]] of Object.entries(ENTITY_SLUGS)) {
      found[key] = ids.find(id => id.startsWith(`${domain}.`) && id.endsWith(`_${slug}`));
    }
    const cells = ids.map(id => /^sensor\..*_cell_(\d+)$/.exec(id)).filter(Boolean)
      .map(m => [Number(m[1]), m[0]]).sort((a, b) => a[0] - b[0]);
    found.cells = cells.map(c => c[1]);
    found.cellNumbers = cells.map(c => c[0]);
    Object.entries(config.entities || {}).forEach(([key, id]) => { found[key] = id; });
    return found;
  }
  set hass(hass) {
    this._hass = hass;
    const ids = this._entities(hass);
    const all = [...Object.values(ids).flat()].filter(v => typeof v === "string");
    // The minute keeps the "last update" age moving between state changes.
    const sig = JSON.stringify([Math.floor(Date.now() / 60000), all.map(id => [id, hass.states[id]])]);
    if (sig === this._sig) return;
    this._sig = sig;
    this._ids = ids;
    this._render();
  }
  _state(key) { return this._ids[key] ? this._hass.states[this._ids[key]] : undefined; }
  _num(key) {
    const s = this._state(key);
    if (!s || BAD.includes(s.state)) return null;
    const n = Number(s.state);
    return Number.isFinite(n) ? n : null;
  }
  _text(key) {
    const s = this._state(key);
    return !s || BAD.includes(s.state) ? null : s.state;
  }
  _fmt(key, digits = 0) {
    const n = this._num(key);
    return n === null ? "–" : n.toFixed(digits);
  }

  _render() {
    const cfg = this._config || {};
    const num = (k) => this._num(k);
    const voltage = num("voltage");
    // Offline = the bridge itself is gone; stale = it answers but the BMS does not. The
    // firmware makes the BMS values unavailable while the data is stale, so their absence
    // alone cannot tell the two apart: only bms_ok can.
    const bridge = this._state("bms_ok");
    const offline = bridge ? BAD.includes(bridge.state) : voltage === null && num("soc") === null;
    const stale = !offline && bridge.state === "off";
    const batt = this._text("state") || "idle";
    const pillClass = offline ? "bad" : stale ? "warn" : batt === "charging" ? "on" : batt === "discharging" ? "warn" : "";
    const pillText = offline ? "offline" : stale ? "stale" : batt;
    const sub = offline ? "No data from the bridge"
      : `${this._fmt("voltage", 2)} V · ${this._fmt("current", 1)} A · ${this._fmt("power")} W`;
    const guard = this._text("guard");
    const soc = num("soc");
    const socColor = soc === null ? "var(--daly-dim)" : soc < 15 ? "var(--daly-bad)" : soc < 30 ? "var(--daly-warn)" : "var(--daly-ok)";
    const icon = batt === "charging" ? "mdi:battery-charging" : "mdi:battery";

    const banners = [];
    if (stale) banners.push(["bad", "mdi:alert", `BMS has not answered for ${this._fmt("stale_s")} s. Its values are shown as unavailable until it does.`]);
    if (guard === "counting") banners.push(["bad", "mdi:battery-alert", "Low-voltage guard is counting down: the bridge is about to deep-sleep."]);

    const tiles = [["soc", "SOC", "%", 1], ["voltage", "Voltage", "V", 2], ["current", "Current", "A", 1], ["power", "Power", "W", 0],
      ["remaining_ah", "Remaining", "Ah", 1], ["cycles", "Cycles", "", 0]]
      .filter(([key]) => this._ids[key]).map(([key, label, unit, digits]) =>
        `<div class="stat" data-entity="${esc(this._ids[key])}"><div class="k">${label}</div><div class="v">${this._fmt(key, digits)}${unit ? ` <small>${unit}</small>` : ""}</div></div>`).join("");

    const cells = (this._ids.cells || []).map((id, i) => {
      const s = this._hass.states[id];
      const mv = s && !BAD.includes(s.state) && Number.isFinite(Number(s.state)) ? Number(s.state) : null;
      return {id, n: this._ids.cellNumbers[i], mv};
    });
    const valid = cells.filter(c => c.mv !== null).map(c => c.mv);
    const hi = Math.max(...valid), lo = Math.min(...valid);
    const scaleLo = Number(cfg.cell_min_mv) || 2500, scaleHi = Number(cfg.cell_max_mv) || 3700;
    const cellRows = cells.map(c => {
      const pct = c.mv === null ? 0 : Math.min(100, Math.max(0, (c.mv - scaleLo) / (scaleHi - scaleLo) * 100));
      const cls = valid.length > 1 && hi !== lo ? (c.mv === hi ? "hi" : c.mv === lo ? "lo" : "") : "";
      return `<div class="cell ${cls}" data-entity="${esc(c.id)}"><span class="n">Cell ${c.n}</span><span class="track"><i style="width:${pct.toFixed(1)}%"></i></span><span class="mv">${c.mv === null ? "–" : c.mv + " mV"}</span></div>`;
    }).join("");

    const switchBtn = (key, label) => {
      const s = this._state(key);
      if (!this._ids[key]) return "";
      const on = s?.state === "on", dead = !s || BAD.includes(s.state);
      return `<button class="sw ${on ? "on" : ""}" data-switch="${esc(this._ids[key])}" ${dead ? "disabled" : ""}><ha-icon icon="${on ? "mdi:toggle-switch" : "mdi:toggle-switch-off-outline"}"></ha-icon>${label}: ${dead ? "–" : on ? "on" : "off"}</button>`;
    };
    const switches = switchBtn("charge_mos", "Charge MOSFET") + switchBtn("discharge_mos", "Discharge MOSFET");

    // The uptime text changes with every report, so when it was last updated is when the
    // bridge last reported. last_reported/last_updated of the other sensors also move when
    // Home Assistant redelivers retained MQTT values after a restart, which says nothing
    // about the bridge.
    const stamps = [this._state("uptime")]
      .filter(s => s && !BAD.includes(s.state)).map(s => Date.parse(s.last_updated))
      .filter(Number.isFinite);
    const lastUpdate = stamps.length ? Math.max(...stamps) : null;
    const info = [["Mode", this._text("mode")], ["Profile", this._text("profile")], ["Guard", guard],
      ["Last update", lastUpdate === null ? null : ago(Date.now() - lastUpdate)],
      ["WiFi", num("rssi") === null ? null : `${num("rssi")} dBm`], ["Uptime", this._text("uptime")], ["IP", this._text("ip")]]
      .filter(([, v]) => v).map(([k, v]) => `<span>${k} <b>${esc(v)}</b></span>`).join("");

    const temps = num("temp_max") === null ? "" :
      `<span>Temperature <b>${this._fmt("temp_min")}–${this._fmt("temp_max")} °C</b></span>`;
    const delta = num("cell_delta_mv") === null ? "" : `${this._fmt("cell_delta_mv")} mV imbalance`;

    this.shadowRoot.innerHTML = `<style>${TOKENS}</style><ha-card><div class="pad">
      ${enabled(cfg, "show_header") ? `<div class="head"><div class="sq ${pillClass}"><ha-icon icon="${icon}"></ha-icon></div>
      <div class="grow"><div class="name">${esc(cfg.title || "DALY BMS")}</div>${enabled(cfg, "show_summary") ? `<div class="sub">${esc(sub)}</div>` : ""}</div>
      ${enabled(cfg, "show_status") ? `<span class="pill ${pillClass}"><span class="dot"></span>${esc(pillText)}</span>` : ""}</div>` : ""}
      ${banners.map(([cls, ic, text]) => `<div class="banner ${cls}"><ha-icon icon="${ic}"></ha-icon>${esc(text)}</div>`).join("")}
      ${enabled(cfg, "show_soc") && this._ids.soc ? `<div class="section"><span>State of charge</span><span class="aside">${soc === null ? "–" : soc.toFixed(1) + " %"}</span></div>
        <div class="soc" role="progressbar" aria-label="State of charge" aria-valuenow="${soc ?? 0}" aria-valuemin="0" aria-valuemax="100"><i style="width:${Math.min(100, Math.max(0, soc ?? 0))}%;background:${socColor}"></i></div>` : ""}
      ${enabled(cfg, "show_tiles") && tiles ? `<div class="stats">${tiles}</div>` : ""}
      ${enabled(cfg, "show_cells") ? `<div class="section"><span>Cells</span><span class="aside">${esc(delta)}</span></div>${cellRows ? `<div class="cells">${cellRows}</div>` : `<div class="empty">${offline ? "Cell data unavailable" : "No cell readings yet"}</div>`}` : ""}
      ${enabled(cfg, "show_temps") && temps ? `<div class="kv">${temps}</div>` : ""}
      ${enabled(cfg, "show_switches") && switches ? `<div class="section">MOSFETs</div><div class="switches">${switches}</div>` : ""}
      ${enabled(cfg, "show_info") && info ? `<div class="section">Bridge</div><div class="kv">${info}</div>` : ""}
      </div></ha-card>`;

    const moreInfo = (entityId) => this.dispatchEvent(new CustomEvent("hass-more-info", {
      detail: { entityId }, bubbles: true, composed: true,
    }));
    this.shadowRoot.querySelectorAll(".stat, .cell").forEach(el => {
      el.setAttribute("role", "button");
      el.setAttribute("tabindex", "0");
      el.addEventListener("click", () => moreInfo(el.dataset.entity));
      el.addEventListener("keydown", event => {
        if (["Enter", " "].includes(event.key)) { event.preventDefault(); moreInfo(el.dataset.entity); }
      });
    });
    this.shadowRoot.querySelectorAll(".sw").forEach(el => el.addEventListener("click", () => {
      const entityId = el.dataset.switch;
      const turnOff = this._hass.states[entityId]?.state === "on";
      // Cutting the load or charging by mistake is costly; ask before switching off.
      if (turnOff && !window.confirm(`Switch off ${el.textContent.split(":")[0]}?`)) return;
      this._hass.callService("switch", turnOff ? "turn_off" : "turn_on", {entity_id: entityId});
    }));
  }
}

if (!customElements.get("daly-bms-card")) {
  customElements.define("daly-bms-card", DalyBmsCard);
  window.customCards = window.customCards || [];
  window.customCards.push({type: "daly-bms-card", name: "DALY BMS",
    description: "Battery state, cell voltages, MOSFET switches and bridge status for the DALY BMS bridge."});
}
