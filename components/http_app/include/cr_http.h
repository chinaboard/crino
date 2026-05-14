#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

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
// Apps put their own initialization here — BLE start, sensor tasks, custom
// timers, etc. The chassis ships a weak no-op default.
void cr_app_init(void);

#ifdef __cplusplus
}
#endif
