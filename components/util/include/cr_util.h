// Tiny header-only utilities shared across components — purely formatting,
// no state. If something here grows beyond ~5 lines or needs state, promote
// it to its own component instead of polluting this header.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_mac.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brand prefix baked into chassis-derived identifiers — mDNS hostname
// (`<prefix>-XXXX.local`), SoftAP SSIDs (`<prefix>-setup-XXXX` /
// `<prefix>-rec-XXXX`), and the prelogin device label in /api/system/status
// (`mdns_host` field). Override at build time so a downstream app can
// re-brand without forking the chassis:
//
//     make build BOARD=supermini-c3 HOSTNAME_PREFIX=fooboard
//
// Stays "crino" by default. Keep it short — "<prefix>-setup-XXXX" must
// fit in the 32-char WiFi SSID limit.
#ifndef CR_HOSTNAME_PREFIX
#define CR_HOSTNAME_PREFIX "crino"
#endif

// "ESP32-C6" / "ESP32-C3" / "?" — used in boot banner, /api/system/diag,
// and anywhere else we want to show what chip this firmware is running on.
static inline const char *cr_chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32C3: return "ESP32-C3";
    default:           return "?";
    }
}

// Format a 6-byte MAC address as "xx:xx:xx:xx:xx:xx" (17 chars + NUL).
// Caller passes a buffer of at least 18 bytes.
static inline void cr_format_mac(const uint8_t addr[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// Stable per-device chassis identifier: "<prefix>-xxxx" using the last
// two bytes of the WiFi STA MAC. Used as mDNS hostname and as the base
// for SoftAP SSIDs. Buffer should be ≥ strlen(prefix) + 6 bytes.
static inline void cr_chassis_hostname(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%s-%02x%02x", CR_HOSTNAME_PREFIX, mac[4], mac[5]);
}

#ifdef __cplusplus
}
#endif
