#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cr_http_start(void);
esp_err_t cr_http_stop(void);

// App extension hook: invoked once near the end of cr_http_start() after all
// chassis routes are installed. Apps override this to register their own
// HTTP endpoints on the same server. The chassis ships a weak no-op default
// so a bare crino build still boots and serves the chassis UI.
//
// Example app implementation:
//
//     static esp_err_t my_status_get(httpd_req_t *req) { ... }
//
//     void cr_app_register_routes(httpd_handle_t server) {
//         httpd_uri_t r = { .uri = "/api/app/status",
//                           .method = HTTP_GET,
//                           .handler = my_status_get };
//         httpd_register_uri_handler(server, &r);
//     }
void cr_app_register_routes(httpd_handle_t server);

// App extension hook: invoked once near the end of app_main() after the
// chassis is fully initialized (config, wifi, http, time, storage, led).
// Apps put their own initialization here — sensor drivers, BLE init,
// custom timers, etc. The chassis ships a weak no-op default.
void cr_app_init(void);

// App extension hook: invoked from inside the chassis /api/system/status
// handler with the cJSON object that's about to be returned. Apps add
// their own fields to it so the public status payload (which the Web
// UI's Overview tab polls every 5 s) carries app-side state without
// needing a separate /api/app/status route. Useful for things like a
// sensor's last reading, a queue depth, a "session active" flag, etc.
//
// Keep what you add small — /status fires every 5 s on every authed
// browser tab.
//
// Example:
//     void cr_app_status_json(cJSON *root) {
//         cJSON_AddNumberToObject(root, "app_temp_c", read_sensor());
//         cJSON_AddBoolToObject(root, "app_session_active", g_session);
//     }
void cr_app_status_json(cJSON *root);

#ifdef __cplusplus
}
#endif
