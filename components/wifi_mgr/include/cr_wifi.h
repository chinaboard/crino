#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CR_WIFI_STATE_DOWN = 0,
    CR_WIFI_STATE_AP,           // SoftAP up
    CR_WIFI_STATE_STA_CONNECTING,
    CR_WIFI_STATE_STA_GOT_IP,
} cr_wifi_state_t;

// Decides STA vs SoftAP from cr_config_boot_mode().
// Must be called after cr_config_init().
esp_err_t cr_wifi_start(void);

cr_wifi_state_t cr_wifi_state(void);
const char *cr_wifi_state_str(cr_wifi_state_t s);

// Returns IPv4 of whichever interface is currently active (STA's local IP, or
// AP gateway IP). Empty string if no interface up.
void cr_wifi_get_ip(char *out, size_t cap);

// Re-read the device_name from cr_config and push it into the mDNS instance
// name (the human-friendly label Bonjour browsers show alongside the
// `crino-XXXX.local` hostname). Safe to call even before mDNS is up — no-op
// in that case. Falls back to "Crino" when no device_name is set.
void cr_wifi_mdns_refresh_instance(void);

#ifdef __cplusplus
}
#endif
