# CLAUDE.md

Guidance for Claude Code working in this repo.

## What is crino?

ESP32-C3 / ESP32-C6 firmware **chassis** — the WiFi + OTA + Web-UI base
that any device-firmware project would otherwise have to write from
scratch. Lifted out of the dingdong-fw attendance project at
`~/workspace/dingdong-fw`, then stripped of all app-specific bits
(BLE / events / workers / attendance UI) and renamed `dd_*` → `cr_*`.

Crino is **not** a finished firmware on its own — it boots, serves the
chassis Web UI, and waits. Apps extend it through three weak symbols
declared in `cr_http.h`:

- `void cr_app_init(void)` — invoked from `app_main` after the chassis
  is fully up. Apps initialize their own subsystems here.
- `void cr_app_register_routes(httpd_handle_t)` — invoked late in
  `cr_http_start()`. Apps add their HTTP routes alongside the chassis
  routes.
- `bool cr_app_led_busy(void)` — return true to make the on-board LED
  show the BUSY state. Optional.

Default no-op implementations live where each hook is invoked, so a
bare crino build (no app) flashes and runs as the chassis-only
firmware described in README.md.

## Build / flash

Same Makefile pattern as dingdong-fw:

```bash
make build BOARD=supermini-c3
make flash BOARD=supermini-c3 PORT=/dev/cu.usbmodem101
make monitor
```

Builds run inside `espressif/idf:v6.0.1` Docker. Flash + monitor run on
the host with `espflash`. Two boards ship: `supermini-c3` (plain GPIO
LED via PWM on GPIO8) and `supermini-c6` (WS2812 RGB on GPIO8). Add a
new board by dropping `boards/<name>.mk` — no Makefile edit needed.

## DEBUG_WIFI build-time bypass

For dev boards, skip the SoftAP first-run wizard by baking creds in:

```bash
make build BOARD=supermini-c3 DEBUG_WIFI_SSID=MyHomeWiFi DEBUG_WIFI_PASS=mypw
```

`cr_config_get_wifi()` honors the macros, so the device boots straight
into STA mode. **Never** set these in CI — the password ends up in the
.bin.

## Top-level architecture

`main/main.c` wires the chassis in this fixed order:

```
cr_log_init           — hooks ESP_LOG into a 64 KB ring buffer (early)
cr_config_init        — NVS + PSA crypto
cr_session_init       — in-RAM admin session table
cr_metrics_record_boot
cr_metrics_consume_restart_cause
cr_metrics_boot_loop_inc — bumps boot-loop counter (recovery guard)
cr_time_init          — applies TZ, prepares SNTP (started after WiFi)
cr_storage_init       — LittleFS mount on "storage" partition → /storage
cr_led_init           — status LED state machine
cr_wifi_start         — SoftAP first-run / NORMAL STA / RECOVERY SoftAP
cr_http_start         — HTTP :80, gzipped Web UI, chassis routes,
                        then invokes cr_app_register_routes() weak hook
cr_time_sntp_start    — background NTP retries until WiFi up
cr_app_init           — weak hook: app initializes its own subsystems
schedule_ota_validation  (60s — marks image valid + clears boot-loop counter)
schedule_heap_watchdog   (every 10s — restart at <6 KB free)
boot_button_task         (5s long-press → factory reset)
```

The alive-tick loop hashes (wifi_state, ntp_synced) and only re-logs
when the hash changes (or every 6 ticks = 60s). Don't add things that
log every tick — silence-on-stable matters for serial readability.

## Boot modes (cr_config_boot_mode)

| Mode | Trigger | WiFi |
|------|---------|------|
| `CR_BOOT_FIRST_RUN` | no admin password set | SoftAP `crino-setup-XXXX` |
| `CR_BOOT_NO_WIFI`   | admin set, no WiFi creds | SoftAP `crino-setup-XXXX` |
| `CR_BOOT_NORMAL`    | both present | STA |

**Recovery override:** `cr_metrics_in_recovery_mode()` returns true when
the boot-loop counter is at or above `CR_BOOT_LOOP_RECOVERY_THRESHOLD`
(3). When that's the case, `cr_wifi_start()` ignores the boot mode
entirely and forces SoftAP with SSID `crino-rec-XXXX` so the operator
can OTA a working image back. Counter is cleared by the OTA-validate
callback after 60s of healthy uptime.

## Components

| Component | Role |
|-----------|------|
| `config` | NVS-backed admin password (PBKDF2-SHA256), WiFi creds, device name, lifetime metrics, boot-loop counter |
| `session` | Opaque 64-hex-char session tokens for admin cookie auth |
| `wifi_mgr` | SoftAP / STA lifecycle, mDNS as `crino-XXXX.local`, recovery override |
| `captive_portal` | DNS hijack on UDP/53 for the SoftAP captive UX |
| `time_sync` | SNTP + monotonic↔wall translation. TZ baked in at build via `CR_TZ`. |
| `storage` | LittleFS mount on the "storage" partition + `cr_storage_fs_info()` |
| `led` | Status LED state machine. Two backends: WS2812 (`CR_LED_KIND=1`) or PWM (`CR_LED_KIND=2`). Builds out completely with `CR_LED_ENABLE=0`. |
| `log_buf` | In-RAM 64 KB ring buffer hooked via `esp_log_set_vprintf`, served by `/api/system/logs` |
| `util` | Small helpers (mac fmt, chip-model name) |
| `http_app` | HTTP server lifecycle, route table, gzipped Web UI, chassis handlers (auth / OTA / system) |

## Adding an app on top of crino

```c
// components/myapp/myapp.c
#include "cr_http.h"
#include "cr_led.h"
#include "esp_log.h"

static esp_err_t my_status_get(httpd_req_t *req) {
    // ...
}

void cr_app_register_routes(httpd_handle_t server) {
    httpd_uri_t r = { .uri = "/api/app/status", .method = HTTP_GET, .handler = my_status_get };
    httpd_register_uri_handler(server, &r);
}

void cr_app_init(void) {
    ESP_LOGI("myapp", "starting");
    // ...
}
```

Drop the file under `components/myapp/`, add a `CMakeLists.txt` with
`idf_component_register(SRCS "myapp.c" ...)`, build. Done.

To add UI tabs, edit `components/http_app/web/index.html` directly —
it's plain HTML/CSS/JS, gzipped at build time, embedded as a binary
blob.

## Conventions worth preserving

- Public headers wrapped in `extern "C"`, named `cr_<component>.h`,
  in `include/`.
- Each `.c` defines a file-scope `static const char *TAG = "<short>"`
  for `ESP_LOG*`.
- Anything called from both an event/timer/HTTP context must be mutex-
  protected.
- New chassis HTTP routes: handler in `cr_http_system.c`, decl in
  `http_internal.h`, route entry in the table in `cr_http.c`. Apps
  register theirs through the weak hook — never add app routes to
  `http_internal.h` or to the chassis table.
- Commit messages are release-notes lines (subject line shows up in
  GitHub auto-generated release notes between tags). Imperative verb,
  scope obvious. Long-form goes in the commit body.

## Tag-driven versioning

`git describe --tags --dirty --always` runs in `CMakeLists.txt`. Clean
tag → bare semver. Off-tag → `0.5.5-3-gabc1234`. Dirty → trailing
`-dirty`. Falls back to `./VERSION` file when there's no git/tags. Cap
at 31 chars (esp_app_desc_t.version[32]). Cut a release with
`git tag v0.5.5 && git push --tags`.
