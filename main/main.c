#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cr_config.h"
#include "cr_wifi.h"
#include "cr_http.h"
#include "cr_session.h"
#include "cr_time.h"
#include "cr_storage.h"
#include "cr_led.h"
#include "cr_log.h"
#include "cr_util.h"

static const char *TAG = "crino";

// After 60s of healthy uptime: mark current OTA image as valid so bootloader
// won't roll back next boot, AND clear the boot-loop counter so the next
// boot doesn't think we're crashing. Both are tied to the same "we made it
// 60s without dying" criterion.
static void ota_mark_valid_cb(void *arg)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(p, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "OTA: marked image '%s' as valid (was pending): %s",
                 p->label, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "OTA: image '%s' state=%d (no action)", p->label, (int)state);
    }
    cr_metrics_boot_loop_clear();
}

static void schedule_ota_validation(void)
{
    static esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = &ota_mark_valid_cb,
        .name = "ota_validate",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 60ULL * 1000 * 1000);  // 60s
    }
}

// Periodic heap watchdog. Logs warnings and restarts if heap is critically low.
#define HEAP_WARN_LIMIT     20480   // 20 KB
#define HEAP_CRITICAL_LIMIT 6144    // 6 KB
static int s_heap_warn_count = 0;

static void heap_check_cb(void *arg)
{
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < HEAP_CRITICAL_LIMIT) {
        ESP_LOGE(TAG, "HEAP CRITICAL: %u bytes free, restarting!", (unsigned)free_heap);
        cr_metrics_set_restart_cause(CR_RESTART_HEAP_CRITICAL);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    if (free_heap < HEAP_WARN_LIMIT && s_heap_warn_count++ % 6 == 0) {
        // Log every minute (we tick every 10s × 6 = 60s)
        ESP_LOGW(TAG, "heap low: %u bytes free (min ever %u)",
                 (unsigned)free_heap,
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}

static void schedule_heap_watchdog(void)
{
    static esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = &heap_check_cb,
        .name = "heap_check",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 10ULL * 1000 * 1000);  // every 10s
    }
}

// Physical recovery: long-press BOOT button (active-low) for 5 seconds →
// factory reset + restart. Useful when the admin password is forgotten or
// the device is wedged. GPIO comes from the board preset via
// boards/<board>.mk → CR_BOOT_BUTTON_GPIO=<N>. A value of -1 disables the
// task entirely (e.g. boards without a button, headless installations).
#ifndef CR_BOOT_BUTTON_GPIO
#define CR_BOOT_BUTTON_GPIO 9
#endif
#define LONG_PRESS_MS    5000

static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CR_BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int64_t press_start_us = 0;
    int     last_log_sec = 0;
    while (1) {
        int level = gpio_get_level(CR_BOOT_BUTTON_GPIO);
        if (level == 0) {
            // pressed (active-low)
            if (press_start_us == 0) {
                press_start_us = esp_timer_get_time();
                last_log_sec = 0;
                ESP_LOGW(TAG, "BOOT button pressed (hold 5s for factory reset)");
            }
            int dur_ms = (int)((esp_timer_get_time() - press_start_us) / 1000);
            int dur_sec = dur_ms / 1000;
            if (dur_sec > last_log_sec) {
                last_log_sec = dur_sec;
                ESP_LOGW(TAG, "BOOT button held %ds/5s", dur_sec);
            }
            if (dur_ms >= LONG_PRESS_MS) {
                ESP_LOGE(TAG, "BOOT long-press → FACTORY RESET");
                cr_config_factory_reset();
                cr_metrics_set_restart_cause(CR_RESTART_FACTORY);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (press_start_us != 0) {
                ESP_LOGI(TAG, "BOOT button released (no action)");
                press_start_us = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void dump_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, " chip=%s rev=%d.%d cores=%d",
             cr_chip_model_str(info.model),
             info.revision / 100, info.revision % 100, info.cores);
    ESP_LOGI(TAG, " MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Weak app start hook: invoked once near the end of app_main, after the
// chassis (config / wifi / http / time / storage / led) is fully up. This
// is where downstream apps initialize their own subsystems (sensor
// drivers, BLE init, custom timers, etc.). The chassis ships a no-op
// default so a bare crino build still boots and serves the chassis
// Web UI.
__attribute__((weak)) void cr_app_init(void) { }

// Boot-loop recovery hand-off: switch the boot partition to the factory
// image and restart. The factory partition holds the immutable crino
// chassis; OTA never touches it. Called only when (a) the boot-loop
// counter has armed recovery AND (b) we are NOT already running from
// factory. After the handoff the bootloader picks up the factory entry
// from otadata and boots a known-good chassis. The recovery banner / UI
// still works there because the chassis is what's running. User then
// uses /api/system/ota to write a working image to ota_0 and the
// chassis automatically jumps back to it after upload.
bool cr_recovery_handoff_to_factory(void)
{
    if (!cr_metrics_in_recovery_mode()) return false;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        // Already in factory — the chassis's SoftAP recovery path
        // handles the rest, no need to switch partitions again.
        return false;
    }

    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        // No factory partition in this build's table — fall back to
        // the SoftAP-only recovery path that doesn't require it.
        ESP_LOGW(TAG, "recovery: no factory partition, falling back to SoftAP-only");
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "recovery: set_boot_partition(factory) rc=%s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGE(TAG, "*** boot-loop recovery: jumping to factory partition '%s' ***",
             factory->label);
    cr_metrics_set_restart_cause(CR_RESTART_BOOT_LOOP);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return true;  // unreachable
}

void app_main(void)
{
    // Give USB Serial/JTAG host time to re-enumerate after a flash so we can
    // capture early boot logs from a non-TTY shell.
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Hook ESP_LOG into the in-RAM ring buffer first so the boot banner +
    // every subsequent log call lands in the buffer that /api/system/logs
    // serves.
    cr_log_init();

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " crino v%s booting", esp_app_get_description()->version);
    {
        const esp_partition_t *p = esp_ota_get_running_partition();
        if (p) {
            const char *kind =
                p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ? "FACTORY (rescue)" :
                p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0   ? "ota_0"            :
                p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1   ? "ota_1"            : "?";
            ESP_LOGI(TAG, " running from partition '%s' (%s)", p->label, kind);
        }
    }
    dump_chip();

    ESP_ERROR_CHECK(cr_config_init());
    ESP_ERROR_CHECK(cr_session_init());
    cr_metrics_record_boot();
    cr_metrics_consume_restart_cause();

    // Bump the boot-loop counter BEFORE bringing up anything that could
    // crash. ota_mark_valid_cb() (60s) clears it. If we never make 60s
    // (panic loop, watchdog, hang), the counter accumulates and the next
    // wifi_start forces SoftAP recovery mode.
    {
        uint32_t bl = cr_metrics_boot_loop_inc();
        if (bl >= CR_BOOT_LOOP_RECOVERY_THRESHOLD) {
            ESP_LOGE(TAG, "*** BOOT-LOOP RECOVERY ARMED *** count=%u (≥%d)",
                     (unsigned)bl, CR_BOOT_LOOP_RECOVERY_THRESHOLD);
            // Hand off to factory partition if we're not already there.
            // If this returns (no factory, or already in factory) we
            // continue boot and fall back to the SoftAP-only recovery
            // path in cr_wifi_start.
            cr_recovery_handoff_to_factory();
            cr_metrics_set_restart_cause(CR_RESTART_BOOT_LOOP);
        } else if (bl > 1) {
            ESP_LOGW(TAG, "boot-loop counter=%u (recovery at %d)",
                     (unsigned)bl, CR_BOOT_LOOP_RECOVERY_THRESHOLD);
        }
    }

    ESP_ERROR_CHECK(cr_time_init());
    ESP_ERROR_CHECK(cr_storage_init());

    // Quiet down chatty subsystems for serial monitor readability.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);

    cr_boot_mode_t mode = cr_config_boot_mode();
    ESP_LOGI(TAG, " boot mode: %s (admin=%d wifi=%d)",
             cr_boot_mode_str(mode),
             cr_config_has_admin(), cr_config_has_wifi());
    ESP_LOGI(TAG, "==========================================");

    ESP_ERROR_CHECK(cr_led_init());
    ESP_ERROR_CHECK(cr_wifi_start());
    ESP_ERROR_CHECK(cr_http_start());
    // SNTP requires WiFi/lwip up. Always start; if STA isn't connected yet,
    // it'll keep retrying.
    cr_time_sntp_start();

    // App takes over. Apps that register HTTP routes use the
    // cr_app_register_routes() hook in cr_http.c (called inside cr_http_start);
    // anything else (BLE init, sensor tasks, custom timers) goes here.
    cr_app_init();

    // Now that everything's wired, schedule OTA validation. If anything
    // panics in the next 60s the bootloader will revert to previous image.
    schedule_ota_validation();
    schedule_heap_watchdog();
#if CR_BOOT_BUTTON_GPIO >= 0
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 5, NULL);
#endif

    char ip[16];
    int tick = 0;
    int last_logged_state = -1;
    while (1) {
        cr_wifi_get_ip(ip, sizeof(ip));

        // Pack key state into a hash so we can suppress identical alive logs.
        int state_hash = (cr_wifi_state() << 16)
                       | (cr_time_is_synced() << 7);
        bool state_changed = (state_hash != last_logged_state);
        bool tick_anchor   = (tick % 6 == 0);  // log at least every 60s

        if (state_changed || tick_anchor) {
            ESP_LOGI(TAG, "alive tick=%d wifi=%s ip=%s ntp=%d heap=%uK",
                     tick, cr_wifi_state_str(cr_wifi_state()),
                     ip[0] ? ip : "-",
                     cr_time_is_synced(),
                     (unsigned)(esp_get_free_heap_size() / 1024));
            last_logged_state = state_hash;
        }
        tick++;
        // Every minute, persist current uptime so we don't lose more than 60s
        // of accounting if power dies or we panic.
        if (tick_anchor) {
            cr_metrics_save_uptime((uint32_t)(esp_timer_get_time() / 1000000));
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
