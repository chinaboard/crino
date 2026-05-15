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

// Friendly device name shown in the chassis Web UI status panel and
// embedded in /api/system/status. Apps that need this name elsewhere
// (BLE advertising name, mDNS instance, sensor labels, …) read it
// via cr_config_get_device_name().
#define CR_DEVICE_NAME_MAX 20
esp_err_t cr_config_get_device_name(char *out, size_t cap);
esp_err_t cr_config_set_device_name(const char *name);

esp_err_t cr_config_factory_reset(void);

// Weak app hook: invoked from inside cr_config_factory_reset() right
// before the chassis namespaces are wiped. Apps that store state in
// their own NVS namespaces, LittleFS files under /storage, BLE bond
// keys, etc. should override this to wipe their own data — otherwise
// app state survives a "factory reset" that the user expects to be
// total. The chassis ships a no-op default.
//
// Example app implementation:
//
//     void cr_app_factory_reset(void) {
//         nvs_handle_t h;
//         if (nvs_open("myapp", NVS_READWRITE, &h) == ESP_OK) {
//             nvs_erase_all(h); nvs_commit(h); nvs_close(h);
//         }
//         unlink("/storage/myapp_calibration.bin");
//     }
void cr_app_factory_reset(void);

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

// ---------- boot-loop recovery ("unbrickable" guard) ----------
//
// Persisted counter incremented at app_main entry, cleared after the same
// 60s window that marks the OTA image valid. If a freshly booted image
// reaches CR_BOOT_LOOP_RECOVERY_THRESHOLD before clearing the counter,
// the chassis forces SoftAP recovery mode (regardless of saved WiFi
// creds) so the user can OTA a working image back without USB access.
//
// When the partition table includes a `factory` slot the recovery flow
// is stronger: instead of just forcing SoftAP from whatever ota slot
// happens to be running, the chassis hands off to the immutable factory
// image (which OTA can never overwrite). See cr_recovery_handoff().
//
// Triggered by anything that prevents reaching the 60s healthy mark:
// panic loops, watchdog resets, hangs that prevent the OTA validate
// timer from running, brownouts, etc. Reset by cr_metrics_boot_loop_clear().
#define CR_BOOT_LOOP_RECOVERY_THRESHOLD 3
uint32_t  cr_metrics_boot_loop_inc(void);   // bump + return new value
uint32_t  cr_metrics_boot_loop_count(void); // peek current value (no mutation)
void      cr_metrics_boot_loop_clear(void); // call once we know boot is healthy
bool      cr_metrics_in_recovery_mode(void); // counter ≥ THRESHOLD

// If running from a non-factory partition AND boot-loop counter is past
// the recovery threshold, switch the boot partition to the factory image
// and restart. Returns false (and does nothing) when either condition
// isn't met — including: counter below threshold, already running from
// factory, or no factory partition exists in the table. Called once at
// the very top of app_main (after cr_metrics_record_boot / consume), so
// the handoff happens before the bad app initializes anything heavy.
bool cr_recovery_handoff_to_factory(void);

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
    CR_RESTART_BOOT_LOOP     = 6,   // forced into recovery after N failed boots
} cr_restart_cause_t;

void cr_metrics_set_restart_cause(cr_restart_cause_t c);
void cr_metrics_consume_restart_cause(void);  // call once at boot
cr_restart_cause_t cr_metrics_get_last_cause(void);
const char *cr_restart_cause_str(cr_restart_cause_t c);

#ifdef __cplusplus
}
#endif
