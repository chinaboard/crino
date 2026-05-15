// HTTP handlers: system status / auth / device admin / OTA / backup-restore /
// diagnostics. Split out of cr_http.c to keep that file focused on the server
// lifecycle.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cr_config.h"
#include "cr_session.h"
#include "cr_led.h"
#include "cr_log.h"
#include "cr_storage.h"
#include "cr_time.h"
#include "cr_util.h"
#include "cr_wifi.h"

#include "driver/temperature_sensor.h"

#include "http_internal.h"

static const char *TAG = "http.sys";

// ---------- chip temperature ----------
//
// ESP32-C6 on-die temperature sensor. Reports junction (silicon) temperature,
// not ambient — typically 5-15°C above room when idle, +20-30°C under WiFi/BLE
// load. Useful for "is the chip cooking" but NOT for room temperature.
// ±2-5°C accuracy via factory eFuse calibration.
//
// Lazy-init on first call: esp_http_server runs handlers on a single task by
// default, so no race protection needed. Returns 0.0 on install failure.

static temperature_sensor_handle_t s_tsens = NULL;

static float chip_temp_c(void)
{
    if (!s_tsens) {
        // Use a local handle until BOTH install and enable succeed —
        // otherwise install-OK + enable-FAIL would leave s_tsens
        // non-NULL but unusable, and the next call would skip the
        // install block and call get_celsius on a disabled sensor.
        temperature_sensor_handle_t h = NULL;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &h) != ESP_OK) {
            ESP_LOGW(TAG, "tsens install failed");
            return 0.0f;
        }
        if (temperature_sensor_enable(h) != ESP_OK) {
            ESP_LOGW(TAG, "tsens enable failed");
            temperature_sensor_uninstall(h);
            return 0.0f;
        }
        s_tsens = h;
    }
    float t = 0.0f;
    temperature_sensor_get_celsius(s_tsens, &t);
    return t;
}

// ---------- public health & metrics ----------

esp_err_t health_get(httpd_req_t *req)
{
    bool wifi_ok  = (cr_wifi_state() == CR_WIFI_STATE_AP ||
                     cr_wifi_state() == CR_WIFI_STATE_STA_GOT_IP);
    bool heap_ok  = esp_get_free_heap_size() > 20480;

    bool ok = wifi_ok && heap_ok;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_status(req, ok ? "200 OK" : "503 Service Unavailable");

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "%s wifi=%d heap_ok=%d uptime_s=%lld\n",
                     ok ? "OK" : "DEGRADED",
                     wifi_ok, heap_ok,
                     (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

esp_err_t metrics_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    cr_metrics_t m = { 0 };
    cr_metrics_load(&m);

    char buf[480];
    int n = snprintf(buf, sizeof(buf),
        "uptime_seconds %lld\n"
        "boot_count %u\n"
        "total_uptime_seconds %llu\n"
        "last_uptime_seconds %u\n"
        "heap_free_bytes %u\n"
        "heap_min_free_bytes %u\n"
        "wifi_state %d\n"
        "ntp_synced %d\n",
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)m.boot_count,
        (unsigned long long)m.total_uptime_s,
        (unsigned)m.last_uptime_s,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (int)cr_wifi_state(),
        cr_time_is_synced());

    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

esp_err_t status_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", cr_boot_mode_str(cr_config_boot_mode()));
    cJSON_AddBoolToObject(j, "recovery_mode", cr_metrics_in_recovery_mode());
    cJSON_AddNumberToObject(j, "boot_loop_count", (double)cr_metrics_boot_loop_count());
    cJSON_AddStringToObject(j, "wifi", cr_wifi_state_str(cr_wifi_state()));
    char ip[16]; cr_wifi_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddBoolToObject(j, "has_admin", cr_config_has_admin());
    cJSON_AddBoolToObject(j, "has_wifi_creds", cr_config_has_wifi());
    {
        char name[CR_DEVICE_NAME_MAX];
        cr_config_get_device_name(name, sizeof(name));
        cJSON_AddStringToObject(j, "device_name", name);
    }
    cJSON_AddBoolToObject(j, "ntp_synced", cr_time_is_synced());
    cJSON_AddNumberToObject(j, "ntp_last_sync_unix", (double)cr_time_last_sync_unix());
    cJSON_AddNumberToObject(j, "now_unix", (double)cr_time_now_unix());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "chip_temp_c", chip_temp_c());

    // mDNS hostname so the UI (and the setup-saved screen) can show users
    // a stable URL for after-reboot access. WiFi-MAC-derived; doesn't
    // track the user-changeable display name.
    {
        uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char host[24];
        snprintf(host, sizeof(host), "crino-%02x%02x", mac[4], mac[5]);
        cJSON_AddStringToObject(j, "mdns_host", host);
    }

    // Firmware version on the public status — useful in the header pre-login.
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) cJSON_AddStringToObject(j, "version", desc->version);

    // LittleFS usage so the Web UI can render a "storage" widget without
    // hitting /api/system/diag every poll.
    size_t fs_total = 0, fs_used = 0;
    if (cr_storage_fs_info(&fs_total, &fs_used) == ESP_OK) {
        cJSON_AddNumberToObject(j, "fs_total", (double)fs_total);
        cJSON_AddNumberToObject(j, "fs_used",  (double)fs_used);
    }
    return reply_json_status(req, "200 OK", j);
}

// ---------- auth ----------

esp_err_t auth_login_post(httpd_req_t *req)
{
    if (!cr_config_has_admin()) {
        return reply_text(req, "409 Conflict", "no admin set; use /api/setup first");
    }
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsString(pw)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "password required");
    }

    bool ok = false;
    esp_err_t err = cr_config_verify_admin_password(pw->valuestring, &ok);
    cJSON_Delete(j);

    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "verify failed");
    if (!ok) {
        // crude rate-limit: sleep on miss
        vTaskDelay(pdMS_TO_TICKS(800));
        return reply_text(req, "401 Unauthorized", "bad password");
    }

    char tok[CR_SESSION_TOKEN_LEN];
    if (cr_session_create(tok, sizeof(tok)) != ESP_OK) {
        return reply_text(req, "500 Internal Server Error", "session alloc failed");
    }

    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=604800",
             tok);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return reply_text(req, "200 OK", "ok");
}

esp_err_t auth_logout_post(httpd_req_t *req)
{
    char tok[CR_SESSION_TOKEN_LEN];
    if (extract_cookie_value(req, COOKIE_NAME, tok, sizeof(tok))) {
        cr_session_destroy(tok);
    }
    httpd_resp_set_hdr(req, "Set-Cookie",
                       COOKIE_NAME "=; Path=/; HttpOnly; Max-Age=0");
    return reply_text(req, "200 OK", "ok");
}

esp_err_t auth_me_get(httpd_req_t *req)
{
    if (is_authed(req)) return reply_text(req, "200 OK", "authed");
    return reply_text(req, "401 Unauthorized", "not authed");
}

esp_err_t auth_change_password_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *old_j = cJSON_GetObjectItem(j, "old_password");
    const cJSON *new_j = cJSON_GetObjectItem(j, "new_password");
    if (!cJSON_IsString(old_j) || !cJSON_IsString(new_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "old_password + new_password required");
    }
    if (strlen(new_j->valuestring) < 8) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "new password too short");
    }

    bool ok = false;
    if (cr_config_verify_admin_password(old_j->valuestring, &ok) != ESP_OK || !ok) {
        cJSON_Delete(j);
        vTaskDelay(pdMS_TO_TICKS(800));
        return reply_text(req, "401 Unauthorized", "old password wrong");
    }

    esp_err_t err = cr_config_set_admin_password(new_j->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "set failed");

    cr_session_destroy_all();
    return reply_text(req, "200 OK", "password changed; all sessions revoked");
}

// ---------- system control ----------

esp_err_t restart_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cr_metrics_set_restart_cause(CR_RESTART_ADMIN);
    reply_text(req, "200 OK", "restarting in 1s");
    schedule_restart(1000);
    return ESP_OK;
}

esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *conf_j = cJSON_GetObjectItem(j, "confirm");
    bool confirmed = cJSON_IsString(conf_j) &&
                     strcmp(conf_j->valuestring, "FACTORY_RESET") == 0;
    cJSON_Delete(j);
    if (!confirmed) {
        return reply_text(req, "400 Bad Request",
                          "must POST {\"confirm\":\"FACTORY_RESET\"}");
    }

    ESP_LOGW(TAG, "factory reset requested via web");

    cr_config_factory_reset();

    cr_metrics_set_restart_cause(CR_RESTART_FACTORY);
    reply_text(req, "200 OK", "factory reset, restarting");
    schedule_restart(1500);
    return ESP_OK;
}

esp_err_t time_resync_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    esp_err_t err = cr_time_sntp_start();
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "sntp restart failed");
    return reply_text(req, "200 OK", "ntp resync triggered");
}

esp_err_t device_name_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    char name[CR_DEVICE_NAME_MAX];
    cr_config_get_device_name(name, sizeof(name));
    cJSON_AddStringToObject(r, "name", name);
    return reply_json_status(req, "200 OK", r);
}

esp_err_t device_name_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *n = cJSON_GetObjectItem(j, "name");
    if (!cJSON_IsString(n) || strlen(n->valuestring) == 0 ||
        strlen(n->valuestring) >= CR_DEVICE_NAME_MAX) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request",
                          "name required (1.." STRINGIFY(CR_DEVICE_NAME_MAX) " chars)");
    }
    esp_err_t err = cr_config_set_device_name(n->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    // Push to mDNS so Bonjour browsers see the new name without a reboot.
    cr_wifi_mdns_refresh_instance();
    // Apps that surface the device name elsewhere (BLE adv name, mDNS instance,
    // sensor labels, etc.) should re-read from cr_config; the chassis itself
    // doesn't push.
    return reply_text(req, "200 OK", "ok");
}

// ---------- WiFi reconfigure (authenticated) ----------
//
// Lets an authenticated admin change WiFi credentials without going through
// factory reset. The chassis's first-run /api/setup is locked once admin +
// wifi are both set, so this is the only sanctioned runtime path. Password
// is never returned over GET — the device only ever exposes the SSID.
//
// POST persists the new creds to NVS and restarts. On reboot the chassis
// boots in NORMAL mode and start_sta() picks up the new creds. If they're
// wrong the existing 10-min STA-give-up + boot-loop recovery catches it.

esp_err_t wifi_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *r = cJSON_CreateObject();

    char ssid[CR_WIFI_SSID_MAX] = {0};
    char pass[CR_WIFI_PASS_MAX] = {0};
    if (cr_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        cJSON_AddStringToObject(r, "saved_ssid", ssid);
    } else {
        cJSON_AddStringToObject(r, "saved_ssid", "");
    }
    cJSON_AddStringToObject(r, "wifi_state", cr_wifi_state_str(cr_wifi_state()));
    char ip[16]; cr_wifi_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(r, "ip", ip);
    return reply_json_status(req, "200 OK", r);
}

esp_err_t wifi_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *ssid_j = cJSON_GetObjectItem(j, "ssid");
    const cJSON *pass_j = cJSON_GetObjectItem(j, "password");

    if (!cJSON_IsString(ssid_j) || ssid_j->valuestring[0] == '\0') {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "ssid required");
    }
    if (strlen(ssid_j->valuestring) >= CR_WIFI_SSID_MAX) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "ssid too long");
    }
    const char *pwstr = cJSON_IsString(pass_j) ? pass_j->valuestring : "";
    if (strlen(pwstr) >= CR_WIFI_PASS_MAX) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "password too long");
    }

    esp_err_t err = cr_config_set_wifi(ssid_j->valuestring, pwstr);
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");

    ESP_LOGW(TAG, "wifi reconfigured via web → restart");
    cr_metrics_set_restart_cause(CR_RESTART_SETUP);
    reply_text(req, "200 OK", "ok, restarting to join new SSID");
    schedule_restart(2000);
    return ESP_OK;
}

// ---------- Manual partition rollback (authenticated) ----------
//
// Lets an admin force-switch the boot partition to either:
//   - "factory":   the immutable chassis (always present and bootable;
//                  the catch-all rescue path)
//   - "other_ota": whatever esp_ota_get_next_update_partition() points
//                  at (i.e. the "other" OTA slot, the one we'd write to
//                  on the next OTA upload). Useful when the user already
//                  knows the previous image was good — bypasses the
//                  10-min STA-giveup + 3-strike boot-loop wait.
//
// Differs from cr_recovery_handoff_to_factory() in that it's user-driven
// (no boot-loop counter precondition) and accepts the "other_ota" target
// for fast manual rollback past the 60s OTA-validate window.

esp_err_t rollback_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *t_j = cJSON_GetObjectItem(j, "target");
    if (!cJSON_IsString(t_j)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request",
                          "target required: \"factory\" or \"other_ota\"");
    }

    const esp_partition_t *target = NULL;
    const char *target_s = t_j->valuestring;
    if (strcmp(target_s, "factory") == 0) {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (!target) {
            cJSON_Delete(j);
            return reply_text(req, "404 Not Found",
                              "no factory partition in this build");
        }
    } else if (strcmp(target_s, "other_ota") == 0) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        target = esp_ota_get_next_update_partition(NULL);
        if (!target || target == running) {
            cJSON_Delete(j);
            return reply_text(req, "409 Conflict",
                              "no alternate ota slot available");
        }
        // Refuse if the slot was never written or got marked invalid by
        // the bootloader. esp_ota_set_boot_partition would itself reject
        // a corrupt image but the friendlier error message is worth it.
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(target, &st) == ESP_OK &&
            (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED ||
             st == ESP_OTA_IMG_UNDEFINED)) {
            cJSON_Delete(j);
            return reply_text(req, "409 Conflict",
                              "alternate slot has no valid image");
        }
    } else {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request",
                          "target must be \"factory\" or \"other_ota\"");
    }
    cJSON_Delete(j);

    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback set_boot(%s): %s",
                 target->label, esp_err_to_name(err));
        return reply_text(req, "500 Internal Server Error",
                          "set_boot_partition failed");
    }

    ESP_LOGW(TAG, "manual rollback → '%s', restarting", target->label);
    // Clear boot-loop counter — the user just made an explicit choice;
    // start counting fresh against whatever boots next.
    cr_metrics_boot_loop_clear();
    cr_metrics_set_restart_cause(CR_RESTART_ADMIN);
    char msg[64];
    snprintf(msg, sizeof(msg), "ok, rebooting into '%s'", target->label);
    reply_text(req, "200 OK", msg);
    schedule_restart(1500);
    return ESP_OK;
}

// ---------- LED runtime toggle ----------

esp_err_t led_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cr_led_is_enabled());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t led_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *en = cJSON_GetObjectItem(j, "enabled");
    if (!cJSON_IsBool(en)) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "enabled (bool) required");
    }
    esp_err_t err = cr_led_set_enabled(cJSON_IsTrue(en));
    cJSON_Delete(j);
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- NTP server ----------

esp_err_t ntp_server_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "server", cr_time_get_ntp_server());
    cJSON_AddBoolToObject  (r, "synced", cr_time_is_synced());
    cJSON_AddNumberToObject(r, "last_sync_unix", (double)cr_time_last_sync_unix());
    return reply_json_status(req, "200 OK", r);
}

esp_err_t ntp_server_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");
    const cJSON *s = cJSON_GetObjectItem(j, "server");
    if (!cJSON_IsString(s) || !s->valuestring[0]) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "server (string) required");
    }
    esp_err_t err = cr_time_set_ntp_server(s->valuestring);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_SIZE) return reply_text(req, "400 Bad Request", "server too long");
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "save failed");
    return reply_text(req, "200 OK", "ok, resyncing");
}

esp_err_t metrics_reset_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    esp_err_t err = cr_metrics_reset();
    if (err != ESP_OK) return reply_text(req, "500 Internal Server Error", "reset failed");
    return reply_text(req, "200 OK", "ok");
}

// ---------- runtime logs ----------
//
// Returns the tail of the in-RAM ring buffer captured by cr_log. Caller may
// pass `?bytes=N` to cap the response (default = full buffer).

esp_err_t logs_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    size_t cap = CR_LOG_BUF_BYTES;
    char qs[64];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qs, "bytes", val, sizeof(val)) == ESP_OK) {
            int n = atoi(val);
            if (n > 0 && (size_t)n < cap) cap = n;
        }
    }

    char *buf = malloc(cap + 1);
    if (!buf) return reply_text(req, "500 Internal Server Error", "no mem");
    size_t got = cr_log_read_tail(buf, cap);
    buf[got] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, buf, got);
    free(buf);
    return r;
}

esp_err_t logs_clear_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;
    cr_log_clear();
    return reply_text(req, "200 OK", "ok");
}


// ---------- backup / restore ----------
//
// Backup contains everything needed to recreate the chassis's logical state
// on a fresh install: wifi creds + admin pw hash+salt. App-side data (NVS
// namespaces other than the chassis "crino" one, app-managed files under
// /storage, BLE bond keys, etc.) is the app's responsibility — extend
// /api/system/backup via a custom route if needed.
//
// Restore is permitted ONLY when no admin is set (i.e. fresh install or after
// factory reset), so it can't be abused to overwrite an active config.

static char hex_nibble(uint8_t n)
{
    return n < 10 ? '0' + n : 'a' + (n - 10);
}

static void hex_encode_str(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex_nibble(in[i] >> 4);
        out[i * 2 + 1] = hex_nibble(in[i] & 0xF);
    }
    out[n * 2] = '\0';
}

static int hex_decode(const char *in, uint8_t *out, size_t out_cap)
{
    size_t in_len = strlen(in);
    if (in_len % 2 != 0 || in_len / 2 > out_cap) return -1;
    for (size_t i = 0; i < in_len / 2; i++) {
        char hi = in[i*2], lo = in[i*2+1];
        int v = 0;
        if      (hi >= '0' && hi <= '9') v |= (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') v |= (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') v |= (hi - 'A' + 10) << 4;
        else return -1;
        if      (lo >= '0' && lo <= '9') v |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') v |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') v |= (lo - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(in_len / 2);
}

esp_err_t backup_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_mac", mac_str);

    cJSON *cfg = cJSON_CreateObject();
    char ssid[CR_WIFI_SSID_MAX] = {0};
    char pass[CR_WIFI_PASS_MAX] = {0};
    if (cr_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        cJSON_AddStringToObject(cfg, "wifi_ssid", ssid);
        cJSON_AddStringToObject(cfg, "wifi_pass", pass);
    }

    uint8_t salt[CR_ADMIN_SALT_LEN], hash[CR_ADMIN_HASH_LEN];
    if (cr_config_get_admin_blob(salt, hash) == ESP_OK) {
        char hex[CR_ADMIN_HASH_LEN * 2 + 1];
        hex_encode_str(salt, CR_ADMIN_SALT_LEN, hex);
        cJSON_AddStringToObject(cfg, "admin_salt_hex", hex);
        hex_encode_str(hash, CR_ADMIN_HASH_LEN, hex);
        cJSON_AddStringToObject(cfg, "admin_hash_hex", hex);
    }

    // Surface settings the user explicitly customised so a restore
    // brings the device back to the same shape — not just authenticatable.
    char dev_name[CR_DEVICE_NAME_MAX] = {0};
    if (cr_config_get_device_name(dev_name, sizeof(dev_name)) == ESP_OK &&
        dev_name[0]) {
        cJSON_AddStringToObject(cfg, "device_name", dev_name);
    }
    const char *ntp = cr_time_get_ntp_server();
    if (ntp && ntp[0]) cJSON_AddStringToObject(cfg, "ntp_server", ntp);

    cJSON_AddItemToObject(root, "config", cfg);

    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"crino-backup.json\"");
    return reply_json_status(req, "200 OK", root);
}

esp_err_t restore_post(httpd_req_t *req)
{
    if (cr_config_has_admin()) {
        return reply_text(req, "409 Conflict",
            "device already configured; factory_reset first");
    }

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *ver = cJSON_GetObjectItem(j, "version");
    if (!cJSON_IsNumber(ver) || (int)ver->valuedouble != 1) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "unsupported version");
    }

    cJSON *cfg = cJSON_GetObjectItem(j, "config");
    if (cJSON_IsObject(cfg)) {
        const cJSON *ssid_j = cJSON_GetObjectItem(cfg, "wifi_ssid");
        const cJSON *pass_j = cJSON_GetObjectItem(cfg, "wifi_pass");
        const cJSON *salt_j = cJSON_GetObjectItem(cfg, "admin_salt_hex");
        const cJSON *hash_j = cJSON_GetObjectItem(cfg, "admin_hash_hex");
        const cJSON *name_j = cJSON_GetObjectItem(cfg, "device_name");
        const cJSON *ntp_j  = cJSON_GetObjectItem(cfg, "ntp_server");

        if (cJSON_IsString(ssid_j)) {
            cr_config_set_wifi(ssid_j->valuestring,
                cJSON_IsString(pass_j) ? pass_j->valuestring : "");
        }

        if (cJSON_IsString(salt_j) && cJSON_IsString(hash_j)) {
            uint8_t salt[CR_ADMIN_SALT_LEN], hash[CR_ADMIN_HASH_LEN];
            if (hex_decode(salt_j->valuestring, salt, sizeof(salt)) == CR_ADMIN_SALT_LEN &&
                hex_decode(hash_j->valuestring, hash, sizeof(hash)) == CR_ADMIN_HASH_LEN) {
                cr_config_set_admin_blob(salt, hash);
            }
        }

        if (cJSON_IsString(name_j) && name_j->valuestring[0] &&
            strlen(name_j->valuestring) < CR_DEVICE_NAME_MAX) {
            cr_config_set_device_name(name_j->valuestring);
        }
        if (cJSON_IsString(ntp_j) && ntp_j->valuestring[0]) {
            cr_time_set_ntp_server(ntp_j->valuestring);
        }
    }

    cJSON_Delete(j);
    cr_metrics_set_restart_cause(CR_RESTART_ADMIN);
    reply_text(req, "200 OK", "restored, restarting");
    schedule_restart(1500);
    return ESP_OK;
}

// ---------- diag ----------

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

esp_err_t diag_get(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *j = cJSON_CreateObject();

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        cJSON *fw = cJSON_CreateObject();
        cJSON_AddStringToObject(fw, "project_name", desc->project_name);
        cJSON_AddStringToObject(fw, "version", desc->version);
        cJSON_AddStringToObject(fw, "idf_ver", desc->idf_ver);
        cJSON_AddStringToObject(fw, "compile_date", desc->date);
        cJSON_AddStringToObject(fw, "compile_time", desc->time);
        cJSON_AddItemToObject(j, "firmware", fw);
    }

    esp_chip_info_t info; esp_chip_info(&info);
    cJSON *chip = cJSON_CreateObject();
    cJSON_AddStringToObject(chip, "model", cr_chip_model_str(info.model));
    cJSON_AddNumberToObject(chip, "rev_major", info.revision / 100);
    cJSON_AddNumberToObject(chip, "rev_minor", info.revision % 100);
    cJSON_AddNumberToObject(chip, "cores", info.cores);
    {
        uint8_t mac[6];
        char buf[18];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        cr_format_mac(mac, buf);
        cJSON_AddStringToObject(chip, "mac_wifi", buf);
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        cr_format_mac(mac, buf);
        cJSON_AddStringToObject(chip, "mac_bt", buf);
        char host[24];
        snprintf(host, sizeof(host), "crino-%02x%02x", mac[4], mac[5]);
        cJSON_AddStringToObject(chip, "mdns", host);
    }
    cJSON_AddNumberToObject(chip, "temp_c", chip_temp_c());
    cJSON_AddItemToObject(j, "chip", chip);

    cJSON *heap = cJSON_CreateObject();
    cJSON_AddNumberToObject(heap, "free",        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(heap, "min_free",    (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(heap, "largest_free",
        (double)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    cJSON_AddItemToObject(j, "heap", heap);

    size_t fs_total = 0, fs_used = 0;
    if (cr_storage_fs_info(&fs_total, &fs_used) == ESP_OK) {
        cJSON *fs = cJSON_CreateObject();
        cJSON_AddNumberToObject(fs, "total", (double)fs_total);
        cJSON_AddNumberToObject(fs, "used",  (double)fs_used);
        cJSON_AddItemToObject(j, "fs", fs);
    }

    cJSON_AddStringToObject(j, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(j, "restart_cause",
                             cr_restart_cause_str(cr_metrics_get_last_cause()));

    cr_metrics_t m;
    if (cr_metrics_load(&m) == ESP_OK) {
        cJSON *mj = cJSON_CreateObject();
        cJSON_AddNumberToObject(mj, "boot_count",     (double)m.boot_count);
        cJSON_AddNumberToObject(mj, "total_uptime_s", (double)m.total_uptime_s);
        cJSON_AddNumberToObject(mj, "last_uptime_s",  (double)m.last_uptime_s);
        cJSON_AddItemToObject(j, "metrics", mj);
    }

    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);

    // Per-slot view: walk every app partition (factory + ota_0/ota_1
    // when present), read each app_desc and report version + which one
    // is running / which one OTA would write to next.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    cJSON *ota = cJSON_CreateObject();
    cJSON *slots = cJSON_CreateArray();
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        bool is_factory = (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY);
        bool is_ota_slot = (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                            p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15);
        if (is_factory || is_ota_slot) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "label", p->label);
            cJSON_AddStringToObject(s, "kind", is_factory ? "factory" : "ota");
            cJSON_AddNumberToObject(s, "size", (double)p->size);
            esp_app_desc_t pdesc;
            if (esp_ota_get_partition_description(p, &pdesc) == ESP_OK) {
                cJSON_AddStringToObject(s, "version", pdesc.version);
            } else {
                cJSON_AddStringToObject(s, "version", "");
            }
            if (is_ota_slot) {
                // Factory has no otadata state — it's always implicitly valid.
                esp_ota_img_states_t st;
                if (esp_ota_get_state_partition(p, &st) == ESP_OK) {
                    const char *st_str =
                        st == ESP_OTA_IMG_NEW            ? "new" :
                        st == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify" :
                        st == ESP_OTA_IMG_VALID          ? "valid" :
                        st == ESP_OTA_IMG_INVALID        ? "invalid" :
                        st == ESP_OTA_IMG_ABORTED        ? "aborted" :
                        st == ESP_OTA_IMG_UNDEFINED      ? "undefined" : "?";
                    cJSON_AddStringToObject(s, "state", st_str);
                }
            } else {
                cJSON_AddStringToObject(s, "state", "immutable");
            }
            cJSON_AddBoolToObject(s, "running", running && p == running);
            cJSON_AddBoolToObject(s, "next",    next    && p == next);
            cJSON_AddItemToArray(slots, s);
        }
        it = esp_partition_next(it);
    }
    // esp_partition_next() frees the iterator when it returns NULL, so
    // by here `it == NULL` and an explicit release would be redundant.
    cJSON_AddItemToObject(ota, "slots", slots);
    if (running) cJSON_AddStringToObject(ota, "running", running->label);
    if (next)    cJSON_AddStringToObject(ota, "next",    next->label);
    cJSON_AddItemToObject(j, "ota", ota);

    return reply_json_status(req, "200 OK", j);
}

// ---------- OTA upload ----------
//
// POST raw .bin body to /api/system/ota. Streams to next OTA partition via
// esp_ota_*. On success, sets boot partition and reboots into the new image.

esp_err_t ota_post(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    int total = req->content_len;
    if (total <= 0) return reply_text(req, "400 Bad Request", "no content");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) return reply_text(req, "500 Internal Server Error", "no ota slot");

    if ((uint32_t)total > update->size) {
        char msg[80];
        snprintf(msg, sizeof(msg), "image %d > slot %u",
                 total, (unsigned)update->size);
        return reply_text(req, "413 Payload Too Large", msg);
    }

    ESP_LOGW(TAG, "OTA: receiving %d bytes into '%s' (slot %u)",
             total, update->label, (unsigned)update->size);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return reply_text(req, "500 Internal Server Error", "ota_begin failed");
    }

    char buf[1024];
    int received = 0;
    int last_log = 0;
    while (received < total) {
        int want = total - received;
        if (want > (int)sizeof(buf)) want = sizeof(buf);
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "recv failed at %d/%d (n=%d)", received, total, n);
            return reply_text(req, "500 Internal Server Error", "recv aborted");
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            return reply_text(req, "500 Internal Server Error", "write failed");
        }
        received += n;
        if (received - last_log > 65536) {
            ESP_LOGI(TAG, "OTA progress: %d/%d (%d%%)",
                     received, total, received * 100 / total);
            last_log = received;
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        return reply_text(req, "400 Bad Request",
                          err == ESP_ERR_OTA_VALIDATE_FAILED
                              ? "image validation failed"
                              : "ota_end failed");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err));
        return reply_text(req, "500 Internal Server Error", "set_boot failed");
    }

    ESP_LOGW(TAG, "OTA complete (%d bytes), rebooting in 2s", received);
    // Reset the boot-loop counter — the user just told us they have a
    // working image, so the next boot is "user-blessed" and shouldn't
    // start halfway up the recovery ramp.
    cr_metrics_boot_loop_clear();
    cr_metrics_set_restart_cause(CR_RESTART_OTA);
    reply_text(req, "200 OK", "ok, rebooting into new image");
    schedule_restart(2000);
    return ESP_OK;
}

// ---------- first-run setup + WiFi scan ----------

// /api/setup handles two cases:
//
//   1. First run (no admin yet): require admin_password + accept optional
//      wifi_ssid / wifi_password. Setup goes to NORMAL mode if WiFi
//      provided, NO_WIFI otherwise.
//
//   2. Admin exists but device is in NO_WIFI mode (admin set, no WiFi
//      creds — typically the user's WiFi password changed, or they
//      moved the device): accept wifi_ssid + wifi_password without
//      admin_password. This is the recovery path that lets a user
//      reconfigure WiFi from the SoftAP without losing their admin
//      password. Recovery is identified by checking `recovery_mode`
//      via cr_metrics_in_recovery_mode() OR by simple no-wifi-creds
//      check — either way, the device is offering SoftAP and the
//      user is trying to reconnect it.
//
// Once admin exists AND wifi creds exist, /api/setup is locked out —
// callers should use the authenticated endpoints from the System tab.
esp_err_t setup_post(httpd_req_t *req)
{
    const bool has_admin = cr_config_has_admin();
    const bool has_wifi  = cr_config_has_wifi();

    // Case 3: fully configured and not in recovery → reject. The
    // authenticated endpoints from the System tab handle re-config.
    if (has_admin && has_wifi && !cr_metrics_in_recovery_mode()) {
        return reply_text(req, "409 Conflict", "already configured");
    }

    cJSON *j = recv_json_body(req);
    if (!j) return reply_text(req, "400 Bad Request", "bad json");

    const cJSON *adm  = cJSON_GetObjectItem(j, "admin_password");
    const cJSON *ssid = cJSON_GetObjectItem(j, "wifi_ssid");
    const cJSON *pass = cJSON_GetObjectItem(j, "wifi_password");

    // Decide whether this request will set admin. First-run REQUIRES it;
    // wifi-only update FORBIDS it (don't let an unauthenticated client
    // change an existing admin password via /api/setup).
    const bool setting_admin = !has_admin;
    if (setting_admin) {
        if (!cJSON_IsString(adm) || strlen(adm->valuestring) < 8) {
            cJSON_Delete(j);
            return reply_text(req, "400 Bad Request", "admin_password >= 8 chars");
        }
    } else if (cJSON_IsString(adm) && adm->valuestring[0] != '\0') {
        // Refuse to touch an existing admin password from an unauth
        // setup call. The Web UI's recovery wizard knows not to send
        // admin_password in this case; this guards a hand-crafted POST.
        cJSON_Delete(j);
        return reply_text(req, "403 Forbidden",
            "admin already set; use /api/auth/change_password");
    }

    const bool setting_wifi = cJSON_IsString(ssid) && ssid->valuestring[0] != '\0';
    if (!setting_admin && !setting_wifi) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request",
            "wifi_ssid required for wifi-only setup");
    }

    // Validate everything before persisting either field, so a partial
    // write can't strand the device. (We don't validate WiFi connectivity
    // here — only string presence/length — full STA validation happens
    // after reboot.)
    if (setting_wifi && strlen(ssid->valuestring) >= CR_WIFI_SSID_MAX) {
        cJSON_Delete(j);
        return reply_text(req, "400 Bad Request", "wifi_ssid too long");
    }

    // Persist in dependency order: admin first (NVS), then wifi. If WiFi
    // save fails we keep the admin password — caller can retry the wifi
    // save without re-sending admin (we'd hit the wifi-only path next time).
    if (setting_admin) {
        esp_err_t err = cr_config_set_admin_password(adm->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(j);
            return reply_text(req, "500 Internal Server Error", "set admin failed");
        }
    }
    if (setting_wifi) {
        const char *pwstr = cJSON_IsString(pass) ? pass->valuestring : "";
        esp_err_t err = cr_config_set_wifi(ssid->valuestring, pwstr);
        if (err != ESP_OK) {
            cJSON_Delete(j);
            return reply_text(req, "500 Internal Server Error", "set wifi failed");
        }
    }
    cJSON_Delete(j);

    cr_metrics_set_restart_cause(CR_RESTART_SETUP);
    reply_text(req, "200 OK", "ok, restarting in 2s");
    schedule_restart(2000);
    return ESP_OK;
}

esp_err_t wifi_scan_get(httpd_req_t *req)
{
    // Wizard / recovery / no-wifi modes need this unauthed (the setup
    // page calls it before the admin even exists). In NORMAL/STA mode
    // require auth — an open scan endpoint here would let anyone on the
    // LAN spam scans, which briefly disrupts the device's own STA
    // association each time.
    if (cr_config_boot_mode() == CR_BOOT_NORMAL &&
        !cr_metrics_in_recovery_mode()) {
        if (require_auth(req) != ESP_OK) return ESP_OK;
    }

    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return reply_text(req, "503 Service Unavailable", "scan failed");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;

    wifi_ap_record_t *recs = NULL;
    if (n > 0) {
        recs = calloc(n, sizeof(wifi_ap_record_t));
        if (!recs) return reply_text(req, "500 Internal Server Error", "oom");
        esp_wifi_scan_get_ap_records(&n, recs);
    }

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < n; ++i) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid",    (char *)recs[i].ssid);
        cJSON_AddNumberToObject(a, "rssi",    recs[i].rssi);
        cJSON_AddNumberToObject(a, "channel", recs[i].primary);
        cJSON_AddBoolToObject  (a, "secure",  recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, a);
    }
    free(recs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "aps", arr);
    return reply_json_status(req, "200 OK", root);
}
