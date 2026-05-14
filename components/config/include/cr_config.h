#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CR_BOOT_FIRST_RUN = 0,  // No admin password set → SoftAP first-run wizard
    CR_BOOT_NO_WIFI,        // Admin set, but no WiFi creds → SoftAP reconfig
    CR_BOOT_NORMAL,         // Admin + WiFi present → STA mode
} cr_boot_mode_t;

#define CR_WIFI_SSID_MAX 32
#define CR_WIFI_PASS_MAX 64

esp_err_t cr_config_init(void);

cr_boot_mode_t cr_config_boot_mode(void);
const char *cr_boot_mode_str(cr_boot_mode_t m);

bool      cr_config_has_admin(void);
esp_err_t cr_config_set_admin_password(const char *plaintext);
esp_err_t cr_config_verify_admin_password(const char *plaintext, bool *out_ok);

// Raw admin credential blob accessors for backup/restore. Salt is 16 bytes,
// hash is 32 bytes (PBKDF2-SHA256 of the password). NEVER expose these to
// untrusted clients.
#define CR_ADMIN_SALT_LEN 16
#define CR_ADMIN_HASH_LEN 32
esp_err_t cr_config_get_admin_blob(uint8_t salt[CR_ADMIN_SALT_LEN],
                                    uint8_t hash[CR_ADMIN_HASH_LEN]);
esp_err_t cr_config_set_admin_blob(const uint8_t salt[CR_ADMIN_SALT_LEN],
                                    const uint8_t hash[CR_ADMIN_HASH_LEN]);

bool      cr_config_has_wifi(void);
esp_err_t cr_config_set_wifi(const char *ssid, const char *password);
esp_err_t cr_config_get_wifi(char *ssid_out, size_t ssid_cap,
                              char *pass_out, size_t pass_cap);

// BLE advertising name (visible in iPhone Bluetooth settings).
// Default is "crino-XXXX" with last 2 BT MAC bytes if not set.
#define CR_DEVICE_NAME_MAX 20
esp_err_t cr_config_get_device_name(char *out, size_t cap);
esp_err_t cr_config_set_device_name(const char *name);

esp_err_t cr_config_factory_reset(void);

// Reliability counters persisted across reboots.
typedef struct {
    uint32_t boot_count;       // increments at every boot
    uint64_t total_uptime_s;   // accumulates over device lifetime
    uint32_t last_uptime_s;    // last session's uptime when it died
} cr_metrics_t;

esp_err_t cr_metrics_load(cr_metrics_t *out);
esp_err_t cr_metrics_record_boot(void);   // increments boot_count, returns new
esp_err_t cr_metrics_save_uptime(uint32_t seconds);  // total += seconds, last = seconds
esp_err_t cr_metrics_reset(void);          // zeros boot_count + total/last uptime

// Restart-cause hint persisted across reboots so we can distinguish OTA /
// admin-triggered / factory / setup / heap-critical / unknown (= crash if
// esp_reset_reason() is sw without us setting anything). Each callsite that
// triggers esp_restart() should call cr_metrics_set_restart_cause() right
// before. At boot, cr_metrics_consume_restart_cause() reads + erases the
// NVS entry so the value reflects the IMMEDIATE prior shutdown only.
typedef enum {
    CR_RESTART_UNKNOWN       = 0,   // power-on, crash, or pre-set-cause boot
    CR_RESTART_OTA           = 1,
    CR_RESTART_ADMIN         = 2,   // /api/system/restart
    CR_RESTART_FACTORY       = 3,   // /api/system/factory_reset OR boot button long-press
    CR_RESTART_SETUP         = 4,   // first-run /api/setup
    CR_RESTART_HEAP_CRITICAL = 5,   // heap watchdog forced restart
} cr_restart_cause_t;

void cr_metrics_set_restart_cause(cr_restart_cause_t c);
void cr_metrics_consume_restart_cause(void);  // call once at boot
cr_restart_cause_t cr_metrics_get_last_cause(void);
const char *cr_restart_cause_str(cr_restart_cause_t c);

#ifdef __cplusplus
}
#endif
