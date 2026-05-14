# crino

ESP32 firmware template (chassis): WiFi + OTA + Web UI + admin auth + NTP + status LED.
Apps fork or extend this with their own components and HTTP routes.

## Status

Chassis boots cleanly on ESP32-C3 / ESP32-C6:

- SoftAP first-run wizard with captive-portal DNS hijack
- Admin password + WiFi creds set via Web UI
- mDNS as `crino-XXXX.local`
- NTP time sync
- HTTP server on :80 with auth, OTA upload, system status, log viewer
- Status LED (WS2812 RGB or plain GPIO LED via PWM)
- Long-press BOOT button (5s) → factory reset
- Web UI: Overview tab (device resources) + System tab (LED / device name /
  NTP / OTA / backup / logs / restart / factory reset)

Bare image is ~905 KB (about 60% of the 1.5 MB OTA slot), leaving ~620 KB
of headroom for app code + ~131 KB free heap on ESP32-C3.

## Build

```bash
make build BOARD=supermini-c3       # ESP32-C3 SuperMini, plain blue LED on GPIO8
make build BOARD=supermini-c6       # ESP32-C6 SuperMini, WS2812 RGB on GPIO8
make flash BOARD=supermini-c3 PORT=/dev/cu.usbmodem101
make monitor
```

Build runs inside `espressif/idf:v6.0.1` Docker; flash/monitor run on the host
with `espflash` (`brew install espflash`).

### Skipping the SoftAP wizard for development

Bake WiFi creds straight into the binary so a fresh-flashed board boots
straight into STA mode (no admin / no SoftAP wizard):

```bash
make build BOARD=supermini-c3 \
    DEBUG_WIFI_SSID=MyHomeWiFi DEBUG_WIFI_PASS='myhomepass'
```

`cr_config_get_wifi()` and `cr_config_boot_mode()` honor the macros, so the
device skips the FIRST_RUN wizard. **Never set these in CI** — the password
goes straight into the .bin. Use only for your own dev boards.

## Extending crino — app hooks

Three weak symbols in the chassis let an app extend without forking:

```c
#include "cr_http.h"
#include "cr_led.h"

// 1. Register your HTTP routes here. Called once near the end of cr_http_start
//    after all chassis routes are installed.
void cr_app_register_routes(httpd_handle_t server) {
    httpd_uri_t r = { .uri = "/api/app/foo", .method = HTTP_GET, .handler = my_foo_get };
    httpd_register_uri_handler(server, &r);
}

// 2. Initialize your app subsystems here. Called once near the end of app_main
//    after the chassis is fully up (config, wifi, http, time, storage, led).
void cr_app_init(void) {
    my_subsystem_start();
}

// 3. (Optional) Tell the chassis LED to show the BUSY state (cyan fast-blink
//    on RGB / DUTY_BLINK on PWM) for transient foreground operations:
//    pairing windows, OTA upload progress, sensor calibration, etc.
bool cr_app_led_busy(void) {
    return my_op_in_progress;
}
```

Drop your `.c`/`.h` into `components/<your_app>/` (or anywhere under
`components/`) — IDF auto-discovers. Define one or more of the hooks above
and your app boots on top of the chassis.

## Repo layout

```
components/
  config/         admin password + wifi creds (NVS)
  session/        cookie-based admin session tokens
  wifi_mgr/       SoftAP first-run, STA normal mode, mDNS
  captive_portal/ DNS hijack for SoftAP captive UX
  time_sync/      NTP + monotonic↔wall translation
  storage/        LittleFS mount + fs_info
  led/            status LED state machine (WS2812 or PWM)
  log_buf/        in-RAM log ring buffer for /api/system/logs
  util/           small helpers (mac fmt, chip-model name, etc.)
  http_app/       HTTP server, routes, gzipped Web UI
main/             chassis boot order + app hooks
boards/           per-board overrides (TARGET, LED_KIND, GPIO, etc.)
```

## License

Same as dingdong-fw (MIT).
