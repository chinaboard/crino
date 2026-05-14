#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize on-board status LED + start a 100 ms tick that polls
// cr_wifi/cr_time state and updates colour/animation.
//
// No-op (and returns ESP_OK) when CR_LED_ENABLE=0 at build time.
esp_err_t cr_led_init(void);

// Runtime on/off (persisted in NVS namespace "led"). When disabled the tick
// keeps running but always pushes black. Default is enabled. Both calls are
// no-ops / return defaults when CR_LED_ENABLE=0 at build time.
esp_err_t cr_led_set_enabled(bool enabled);
bool      cr_led_is_enabled(void);

// App extension hook: return true to make the LED show the BUSY state
// (cyan fast-blink on RGB / DUTY_BLINK on PWM). Useful for transient
// foreground operations like "BLE pairing window open" or "OTA upload in
// progress". The chassis ships a weak default that always returns false.
//
// Override by defining a non-weak version in your app:
//
//     bool cr_app_led_busy(void) { return my_pairing_active; }
bool cr_app_led_busy(void);

#ifdef __cplusplus
}
#endif
