// Internal header shared between cr_http.c and cr_http_system.c.
// Not exported to other components — those still consume the public cr_http.h.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#define COOKIE_NAME "cr_session"

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

// ---------- shared helpers (defined in cr_http.c) ----------

esp_err_t reply_json_status(httpd_req_t *req, const char *status, cJSON *body);
esp_err_t reply_text(httpd_req_t *req, const char *status, const char *msg);
cJSON    *recv_json_body(httpd_req_t *req);
bool      is_authed(httpd_req_t *req);
esp_err_t require_auth(httpd_req_t *req);
bool      extract_cookie_value(httpd_req_t *req, const char *name,
                                char *out, size_t cap);
void      schedule_restart(int delay_ms);

// ---------- chassis handlers (defined in cr_http_system.c) ----------
//
// Every handler here is part of the WiFi/OTA/UI chassis. App-specific routes
// register themselves via the weak cr_app_register_routes() hook in cr_http.c
// — they should never be added to this header.

esp_err_t health_get(httpd_req_t *req);
esp_err_t metrics_get(httpd_req_t *req);
esp_err_t status_get(httpd_req_t *req);
esp_err_t diag_get(httpd_req_t *req);
esp_err_t restart_post(httpd_req_t *req);
esp_err_t factory_reset_post(httpd_req_t *req);
esp_err_t time_resync_post(httpd_req_t *req);
esp_err_t device_name_get(httpd_req_t *req);
esp_err_t device_name_post(httpd_req_t *req);
esp_err_t led_get(httpd_req_t *req);
esp_err_t led_post(httpd_req_t *req);
esp_err_t ntp_server_get(httpd_req_t *req);
esp_err_t ntp_server_post(httpd_req_t *req);
esp_err_t tz_get(httpd_req_t *req);
esp_err_t tz_post(httpd_req_t *req);
esp_err_t metrics_reset_post(httpd_req_t *req);
esp_err_t logs_get(httpd_req_t *req);
esp_err_t logs_clear_post(httpd_req_t *req);
esp_err_t backup_get(httpd_req_t *req);
esp_err_t restore_post(httpd_req_t *req);
esp_err_t ota_post(httpd_req_t *req);
esp_err_t auth_login_post(httpd_req_t *req);
esp_err_t auth_logout_post(httpd_req_t *req);
esp_err_t auth_me_get(httpd_req_t *req);
esp_err_t auth_change_password_post(httpd_req_t *req);
esp_err_t setup_post(httpd_req_t *req);
esp_err_t wifi_scan_get(httpd_req_t *req);
esp_err_t wifi_get(httpd_req_t *req);
esp_err_t wifi_post(httpd_req_t *req);
esp_err_t rollback_post(httpd_req_t *req);
