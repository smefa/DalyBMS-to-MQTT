// HTTP endpoints:
//   GET  /             dashboard
//   GET  /status.json  full reading, consumed by the dashboard
//   GET  /log          recent diagnostics (UART0 is taken by the BMS)
//   POST /reboot       restart (POST-only: a GET would let a link prefetch
//                      reboot the bridge)
#pragma once

#include "daly_bms.h"

namespace webserver {

void begin(const DalyBms* bms);
void loop();

} // namespace webserver
