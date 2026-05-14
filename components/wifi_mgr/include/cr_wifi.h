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

#ifdef __cplusplus
}
#endif
