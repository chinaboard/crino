#include "cr_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "time";

#define NTP_SERVER_DEFAULT "ntp.aliyun.com"
#define NTP_NVS_NS         "time"
#define NTP_NVS_KEY        "ntp_server"
#define TZ_NVS_KEY         "tz"

// POSIX TZ baked in at build time. Used as the DEFAULT when NVS has no
// override. Override via Makefile `-DCR_TZ='"JST-9"'` for the build-time
// default; at runtime use cr_time_set_tz() (web UI) — that path persists
// to NVS and survives reboots, no reflash needed.
#ifndef CR_TZ
#define CR_TZ "CST-8"
#endif

// Mutex protects the 64-bit anchor pair, the NTP server buffer, AND the TZ
// buffer. Anchor pair is written from the SNTP `on_sync` callback (FreeRTOS
// task context) and read from HTTP handlers — without the lock, 64-bit
// reads on the 32-bit RISC-V chip can tear. NTP/TZ strings are similarly
// mutated by HTTP POST while another HTTP GET reads them.
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_synced = false;
static int64_t       s_wall_at_sync_us = 0;
static int64_t       s_mono_at_sync_us = 0;
static char          s_ntp_server[CR_NTP_SERVER_MAX] = NTP_SERVER_DEFAULT;
static char          s_tz[CR_TZ_MAX] = CR_TZ;
static bool          s_sntp_inited = false;

#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static void on_sync(struct timeval *tv)
{
    int64_t wall = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec;
    int64_t mono = esp_timer_get_time();
    LOCK();
    s_synced = true;
    s_wall_at_sync_us = wall;
    s_mono_at_sync_us = mono;
    UNLOCK();
    ESP_LOGI(TAG, "NTP sync OK: unix=%lld", (long long)tv->tv_sec);
}

static void load_ntp_server(void)
{
    nvs_handle_t h;
    if (nvs_open(NTP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[CR_NTP_SERVER_MAX];
    size_t sz = sizeof(buf);
    if (nvs_get_str(h, NTP_NVS_KEY, buf, &sz) == ESP_OK && buf[0]) {
        buf[sizeof(buf) - 1] = '\0';
        LOCK();
        strncpy(s_ntp_server, buf, sizeof(s_ntp_server) - 1);
        s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
        UNLOCK();
    }
    nvs_close(h);
}

// Loaded from the same "time" NVS namespace as the NTP server. Apply BEFORE
// the first setenv(TZ)+tzset() in cr_time_init so the very first localtime()
// uses the persisted offset, not the build-time fallback.
static void load_tz(void)
{
    nvs_handle_t h;
    if (nvs_open(NTP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[CR_TZ_MAX];
    size_t sz = sizeof(buf);
    if (nvs_get_str(h, TZ_NVS_KEY, buf, &sz) == ESP_OK && buf[0]) {
        buf[sizeof(buf) - 1] = '\0';
        LOCK();
        strncpy(s_tz, buf, sizeof(s_tz) - 1);
        s_tz[sizeof(s_tz) - 1] = '\0';
        UNLOCK();
    }
    nvs_close(h);
}

esp_err_t cr_time_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    load_tz();
    setenv("TZ", s_tz, 1);
    tzset();
    load_ntp_server();
    ESP_LOGI(TAG, "tz=%s, ntp=%s, awaiting sync", s_tz, s_ntp_server);
    return ESP_OK;
}

esp_err_t cr_time_sntp_start(void)
{
    // First call configures + starts. Subsequent calls (e.g. after server
    // change) deinit then re-init so the new server takes effect.
    if (s_sntp_inited) {
        esp_netif_sntp_deinit();
        s_sntp_inited = false;
    }
    // ESP_NETIF_SNTP_DEFAULT_CONFIG stores the server pointer (not a copy)
    // and SNTP keeps using it across re-resolutions. Pass the static
    // s_ntp_server directly — it lives forever; mutations go through
    // cr_time_set_ntp_server which deinits before mutating then reinits.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    cfg.start = true;
    cfg.sync_cb = on_sync;
    cfg.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_inited = true;
        ESP_LOGI(TAG, "SNTP started, server=%s", s_ntp_server);
    } else {
        ESP_LOGE(TAG, "sntp_init=%s", esp_err_to_name(err));
    }
    return err;
}

bool cr_time_is_synced(void) { return s_synced; }

int64_t cr_time_now_unix(void)
{
    if (!s_synced) return 0;
    return (int64_t)time(NULL);
}

int64_t cr_time_mono_us(void)
{
    return esp_timer_get_time();
}

int64_t cr_time_mono_to_unix(int64_t mono_us)
{
    if (!s_synced) return 0;
    LOCK();
    int64_t delta_us = mono_us - s_mono_at_sync_us;
    int64_t wall_us  = s_wall_at_sync_us + delta_us;
    UNLOCK();
    return wall_us / 1000000;
}

const char *cr_time_get_ntp_server(void)
{
    // Returns pointer into shared buffer — caller copies if it needs stability
    // beyond the immediate use. HTTP handlers serialize replies anyway.
    return s_ntp_server;
}

esp_err_t cr_time_set_ntp_server(const char *server)
{
    if (!server || !server[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(server) >= CR_NTP_SERVER_MAX) return ESP_ERR_INVALID_SIZE;

    LOCK();
    strncpy(s_ntp_server, server, sizeof(s_ntp_server) - 1);
    s_ntp_server[sizeof(s_ntp_server) - 1] = '\0';
    UNLOCK();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NTP_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, NTP_NVS_KEY, server);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "ntp server -> %s", server);

    // Reset s_synced so the badge flips to "syncing" until the new server
    // answers; resync immediately.
    s_synced = false;
    cr_time_sntp_start();
    return err;
}

int64_t cr_time_last_sync_unix(void)
{
    LOCK();
    int64_t v = s_wall_at_sync_us;
    UNLOCK();
    return v / 1000000;
}

const char *cr_time_get_tz(void)
{
    // Pointer into shared buffer — same caveats as cr_time_get_ntp_server().
    return s_tz;
}

esp_err_t cr_time_set_tz(const char *tz)
{
    if (!tz || !tz[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(tz) >= CR_TZ_MAX) return ESP_ERR_INVALID_SIZE;

    LOCK();
    strncpy(s_tz, tz, sizeof(s_tz) - 1);
    s_tz[sizeof(s_tz) - 1] = '\0';
    UNLOCK();

    // Apply to libc immediately — any subsequent localtime_r() will pick up
    // the new offset. tzset() reads the TZ env var, so setenv must come
    // first. _r=1 to overwrite the existing value.
    setenv("TZ", s_tz, 1);
    tzset();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NTP_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, TZ_NVS_KEY, s_tz);
        err = nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "tz -> %s", s_tz);
    return err;
}
