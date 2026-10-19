// HTTP endpoints:
//   GET  /             dashboard
//   GET  /status.json  full reading, consumed by the dashboard
//   GET  /log          recent diagnostics (UART0 is taken by the BMS)
//   POST /limits       change the BMS voltage limits (form: pin + any of the
//                      cell_*_mv / pack_*_v fields of status.json "limits")
//   POST /reboot       restart (POST-only: a GET would let a link prefetch
//                      reboot the bridge)
#pragma once

#include "daly_bms.h"

namespace webserver {

void begin(DalyBms* bms);
void loop();

} // namespace webserver
