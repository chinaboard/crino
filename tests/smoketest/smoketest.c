// Smoke test for the five chassis weak-symbol app hooks. Defines a
// STRONG version of each hook that, if successfully overriding the
// chassis weak default, will:
//
//   1. Log a banner from cr_app_init() so we can verify it's invoked
//      after the chassis init order finishes.
//   2. Register one HTTP route at /api/smoketest/ping so we can verify
//      cr_app_register_routes() got called with the live server handle
//      (curl returns "pong\n").
//   3. Make cr_app_led_busy() return true for the first 30s of uptime
//      so we can verify the chassis LED actually picks up the override
//      (LED stays on the BUSY state for 30s after boot, then drops
//      back to OK/AP/etc.).
//   4. Log a banner from cr_app_factory_reset() so we can verify it's
//      invoked when the chassis processes a factory-reset request
//      (boot button long-press, /api/system/factory_reset, recovery
//      arming via /api/system/rollback). Triggered manually during
//      the smoketest run.
//   5. Inject `smoketest_uptime_us` into /api/system/status via
//      cr_app_status_json() — `curl /api/system/status` should show
//      that field, proving the hook fires.
//
// To use: build crino with this component compiled in (it's a normal
// component, just exists in components/smoketest/). To remove for a
// real app build, just rm -rf components/smoketest/.

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "cr_config.h"
#include "cr_http.h"
#include "cr_led.h"

static const char *TAG = "smoketest";

static esp_err_t ping_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "pong\n", HTTPD_RESP_USE_STRLEN);
}

// Strong overrides — these must beat the chassis weak defaults at
// link time. If they don't, the chassis prints its silent no-ops
// instead and the verifications above all fail.

void cr_app_init(void)
{
    ESP_LOGW(TAG, "===== smoketest cr_app_init() invoked =====");
}

void cr_app_register_routes(httpd_handle_t server)
{
    httpd_uri_t r = {
        .uri      = "/api/smoketest/ping",
        .method   = HTTP_GET,
        .handler  = ping_get,
    };
    esp_err_t err = httpd_register_uri_handler(server, &r);
    ESP_LOGW(TAG, "===== smoketest cr_app_register_routes() invoked, /api/smoketest/ping rc=%s =====",
             err == ESP_OK ? "OK" : "FAIL");
}

bool cr_app_led_busy(void)
{
    // First 30s of uptime: report busy so the chassis LED picks up
    // the override and shows the BUSY animation. After that drop back
    // to false so the LED returns to its normal state.
    int64_t uptime_us = esp_timer_get_time();
    return uptime_us < (30LL * 1000 * 1000);
}

void cr_app_factory_reset(void)
{
    ESP_LOGW(TAG, "===== smoketest cr_app_factory_reset() invoked =====");
}

void cr_app_status_json(cJSON *root)
{
    cJSON_AddNumberToObject(root, "smoketest_uptime_us",
                            (double)esp_timer_get_time());
}
