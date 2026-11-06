const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");

const elements = new Map();
const context = vm.createContext({
  HTMLElement: class {
    attachShadow() { this.shadowRoot = {innerHTML: "", querySelectorAll: () => []}; }
  },
  CustomEvent: class {},
  customElements: {get: name => elements.get(name), define: (name, cls) => elements.set(name, cls)},
  window: {},
});
vm.runInContext(fs.readFileSync(path.join(__dirname, "../dist/daly-bms-card.js"), "utf8"), context);

function hassFixture(overrides = {}) {
  const hass = {entities: {}, states: {}, devices: {d1: {name: "DALY BMS"}, d2: {name: "Other"}}};
  const add = (id, state, device = "d1") => {
    hass.entities[id] = {platform: "mqtt", device_id: device};
    hass.states[id] = {state, attributes: {}};
  };
  const values = {
    "sensor.daly_bms_pack_voltage": "13.24", "sensor.daly_bms_pack_current": "-4.1",
    "sensor.daly_bms_pack_power": "-54", "sensor.daly_bms_state_of_charge": "72.5",
    "sensor.daly_bms_battery_state": "discharging", "binary_sensor.daly_bms_bms_data_fresh": "on",
    "switch.daly_bms_charge_mosfet": "on", "switch.daly_bms_discharge_mosfet": "on",
    "select.daly_bms_pack_voltage_profile": "normal", "sensor.daly_bms_cell_1": "3310",
    "sensor.daly_bms_cell_2": "3290", ...overrides,
  };
  Object.entries(values).forEach(([id, state]) => add(id, state));
  add("sensor.other_pack_voltage", "99", "d2");
  return hass;
}
const make = (config, hass) => {
  const card = new (elements.get("daly-bms-card"))();
  card.setConfig(config);
  card.hass = hass;
  return card;
};

test("finds the DALY device, not another MQTT device or the profile select", () => {
  const card = make({}, hassFixture());
  assert.equal(card._ids.voltage, "sensor.daly_bms_pack_voltage");
  assert.equal(card._ids.profile, "select.daly_bms_pack_voltage_profile");
  assert.deepEqual(Array.from(card._ids.cells), ["sensor.daly_bms_cell_1", "sensor.daly_bms_cell_2"]);
  assert.match(card.shadowRoot.innerHTML, /13\.24 V/);
  assert.match(card.shadowRoot.innerHTML, /discharging/);
});

test("stale BMS shows a warning, offline bridge shows offline", () => {
  const stale = make({}, hassFixture({"binary_sensor.daly_bms_bms_data_fresh": "off"}));
  assert.match(stale.shadowRoot.innerHTML, /stale/);
  assert.match(stale.shadowRoot.innerHTML, /has not answered/);
  const offline = make({}, hassFixture({"sensor.daly_bms_pack_voltage": "unavailable",
    "sensor.daly_bms_state_of_charge": "unavailable", "binary_sensor.daly_bms_bms_data_fresh": "unavailable"}));
  assert.match(offline.shadowRoot.innerHTML, /offline/);
});

test("low-voltage guard counting is announced; show_* and escaping work", () => {
  const hass = hassFixture({"sensor.daly_bms_low_voltage_guard": "counting"});
  const card = make({title: "<b>x</b>", show_cells: false}, hass);
  assert.match(card.shadowRoot.innerHTML, /deep-sleep/);
  assert.match(card.shadowRoot.innerHTML, /&lt;b&gt;x/);
  assert.doesNotMatch(card.shadowRoot.innerHTML, /Cell 1/);
});

test("device_id and entity overrides pick another source", () => {
  const card = make({device_id: "d1", entities: {voltage: "sensor.other_pack_voltage"}}, hassFixture());
  assert.equal(card._ids.voltage, "sensor.other_pack_voltage");
});

test("a silent BMS (values unavailable, bms_ok off) is stale, not offline", () => {
  const card = make({}, hassFixture({"sensor.daly_bms_pack_voltage": "unavailable",
    "sensor.daly_bms_state_of_charge": "unavailable", "binary_sensor.daly_bms_bms_data_fresh": "off"}));
  assert.match(card.shadowRoot.innerHTML, /stale/);
  assert.doesNotMatch(card.shadowRoot.innerHTML, /offline/);
});

test("cells keep their physical numbers when one is missing", () => {
  const hass = hassFixture();
  delete hass.entities["sensor.daly_bms_cell_1"];
  const card = make({}, hass);
  assert.match(card.shadowRoot.innerHTML, /Cell 2/);
  assert.doesNotMatch(card.shadowRoot.innerHTML, /Cell 1</);
});

test("last update comes from the uptime sensor's last_updated, not other sensors", () => {
  const hass = hassFixture({"sensor.daly_bms_uptime": "3 h 20 m"});
  hass.states["sensor.daly_bms_uptime"].last_updated = new Date(Date.now() - 2 * 3600e3).toISOString();
  hass.states["sensor.daly_bms_pack_voltage"].last_updated = new Date().toISOString();
  const card = make({}, hass);
  assert.match(card.shadowRoot.innerHTML, /Last update <b>2 h ago<\/b>/);
});

test("SOC tile comes before the voltage tile", () => {
  const html = make({}, hassFixture()).shadowRoot.innerHTML;
  const tiles = [...html.matchAll(/<div class="k">([^<]+)<\/div>/g)].map(m => m[1]);
  assert.deepEqual(Array.from(tiles).slice(0, 2), ["SOC", "Voltage"]);
  assert.match(html, /72\.5 <small>%<\/small>/);
});
