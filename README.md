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
- **Boot-loop recovery**: 3 consecutive bad boots → forced SoftAP
  (see [Unbrickable recovery](#unbrickable-recovery) below)
- Web UI: Overview tab (device resources) + System tab (LED / device name /
  NTP / OTA / backup / logs / restart / factory reset)

Bare image is ~927 KB (about 60% of the 1.5 MB OTA slot), leaving ~620 KB
of headroom for app code + ~131 KB free heap on ESP32-C3.

## Unbrickable recovery

Three layered safety nets keep the device recoverable from any state
without USB access:

**Layer 1 — OTA rollback** (built into ESP-IDF, used by crino):
After an OTA update the bootloader marks the new image as "pending verify".
If the new image panics within 60 seconds, the bootloader auto-reverts to
the previous image at the next boot.

**Layer 2 — Boot-loop counter** (chassis):
Persistent NVS counter `boot_loop` is bumped at every `app_main` entry and
cleared by the same 60s timer that validates the OTA image. If three
consecutive boots bump the counter without ever reaching the 60s healthy
mark — anything from a panic loop after rollback to a watchdog hang to a
brownout cycle — the chassis arms recovery mode. The recovery SSID is
`crino-rec-XXXX` (vs the normal `crino-setup-XXXX`) so a phone scan
immediately shows the device is in trouble. The Web UI prints a red
banner on every page; recovery is just "upload a working .bin via System
→ Firmware OTA". Threshold is in `CR_BOOT_LOOP_RECOVERY_THRESHOLD`
(default 3).

**Layer 3 — Factory partition handoff** (chassis + `partitions.csv`):
The partition table reserves a 1 MB `factory` slot that holds an
immutable crino chassis image. OTA writes only ever target `ota_0` /
`ota_1` — the factory partition is read-only at runtime and cannot be
erased by any HTTP path. When Layer 2 recovery arms AND the bad app is
running from one of the OTA slots, the chassis calls
`esp_ota_set_boot_partition(factory)` + reboots. The bootloader honours
that and jumps to the immutable chassis. From there the user uploads a
fixed image via the chassis's own Web UI — the new image lands in
`ota_0` and the chassis hands control back. The factory image is reach-
able only via JTAG/USB re-flash. So even if `ota_0` AND `ota_1` are both
panic-loop images, the device is recoverable purely over WiFi.

The `make flash` command writes the chassis to the factory partition on
first flash, so a newly-flashed board boots straight into the rescue
image. Subsequent firmware updates use the OTA workflow and target the
ota_X slots.

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

Four weak symbols in the chassis let an app extend without forking:

```c
#include "cr_config.h"
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

// 4. (Optional) Wipe app-side state on factory reset. The chassis only
//    erases its own NVS namespaces; without this hook your app's NVS
//    namespace and any /storage files survive a "factory reset".
void cr_app_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open("myapp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    unlink("/storage/myapp_calibration.bin");
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

MIT.
