// HTTP server lifecycle, route table, shared helpers, and the root handler
// that serves the embedded Web UI. All chassis handlers live in
// cr_http_system.c. Apps add their own routes via the weak
// cr_app_register_routes() hook (see bottom of cr_http_start).

#include "cr_http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "cr_session.h"

#include "http_internal.h"

static const char *TAG = "http";

static httpd_handle_t s_server = NULL;

// Embedded Web UI (see web/index.html, gzipped + linked as a binary blob via
// the custom CMake step in CMakeLists.txt).
extern const uint8_t INDEX_HTML_GZ_START[] asm("_binary_index_html_gz_start");
extern const uint8_t INDEX_HTML_GZ_END[]   asm("_binary_index_html_gz_end");

// ---------- shared helpers ----------

static void delayed_restart_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGW(TAG, "restarting (delayed %d ms)", delay_ms);
    esp_restart();
}

void schedule_restart(int delay_ms)
{
    BaseType_t rc = xTaskCreate(delayed_restart_task, "restart", 2048,
                                (void *)(intptr_t)delay_ms, 5, NULL);
    if (rc != pdPASS) {
        // Low heap — can't spawn the delayed task. Restart inline so the
        // caller's "OK, restarting" reply isn't a lie. Tiny delay so the
        // HTTP response has a chance to flush.
        ESP_LOGE(TAG, "xTaskCreate(restart) failed (rc=%d) — restarting inline",
                 (int)rc);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

esp_err_t reply_json_status(httpd_req_t *req, const char *status, cJSON *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char *s = cJSON_PrintUnformatted(body);
    esp_err_t r;
    if (s) {
        r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
        free(s);
    } else {
        // Low heap — cJSON couldn't serialize. Don't pass NULL to
        // httpd_resp_send (crash). Reply with a small error string so
        // the caller knows it failed instead of getting nothing back.
        ESP_LOGE(TAG, "reply_json_status: cJSON_PrintUnformatted returned NULL");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        r = httpd_resp_send(req, "oom", HTTPD_RESP_USE_STRLEN);
    }
    cJSON_Delete(body);
    return r;
}

esp_err_t reply_text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

cJSON *recv_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    // 8KB cap — enough for the chassis backup JSON (wifi + admin blob ~600B)
    // plus generous headroom for any app endpoint that wants to POST larger
    // payloads. Apps with bigger payload needs should add their own
    // handler with a custom buffer.
    if (total <= 0 || total > 8192) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

bool extract_cookie_value(httpd_req_t *req, const char *name,
                          char *out, size_t cap)
{
    size_t hlen = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hlen == 0 || hlen >= 1024) return false;
    char *buf = malloc(hlen + 1);
    if (!buf) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, hlen + 1) != ESP_OK) {
        free(buf); return false;
    }
    char prefix[64];
    int pn = snprintf(prefix, sizeof(prefix), "%s=", name);
    if (pn <= 0 || pn >= (int)sizeof(prefix)) { free(buf); return false; }

    bool ok = false;
    char *p = buf;
    while ((p = strstr(p, prefix)) != NULL) {
        if (p == buf || p[-1] == ' ' || p[-1] == ';') {
            p += pn;
            char *end = strpbrk(p, ";\r\n");
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen < cap) {
                memcpy(out, p, vlen);
                out[vlen] = '\0';
                ok = true;
            }
            break;
        }
        p += pn;
    }
    free(buf);
    return ok;
}

bool is_authed(httpd_req_t *req)
{
    char tok[CR_SESSION_TOKEN_LEN];
    if (!extract_cookie_value(req, COOKIE_NAME, tok, sizeof(tok))) return false;
    return cr_session_is_valid(tok);
}

esp_err_t require_auth(httpd_req_t *req)
{
    if (is_authed(req)) return ESP_OK;
    reply_text(req, "401 Unauthorized", "auth required");
    return ESP_FAIL;
}

// ---------- root + favicon ----------

// Build-time-stable ETag for the embedded Web UI: first 16 hex chars of the
// app ELF SHA256 (esp_app_get_description gives us the raw 32 bytes). Same
// across reboots of the same binary; changes the moment any source file
// (including web/index.html) flips a byte. Lets the browser short-circuit
// repeat downloads via If-None-Match → 304 Not Modified.
static const char *web_ui_etag(void)
{
    static char etag[20] = "";  // "\"" + 16 hex chars + "\"" + NUL
    if (etag[0]) return etag;
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) { strcpy(etag, "\"unknown\""); return etag; }
    char hex[17];   // 8 bytes × 2 hex + NUL — exactly the bytes we use
    for (int i = 0; i < 8; i++) {
        snprintf(hex + i * 2, 3, "%02x", d->app_elf_sha256[i]);
    }
    snprintf(etag, sizeof(etag), "\"%.16s\"", hex);
    return etag;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const char *etag = web_ui_etag();

    // 304 short-circuit: if the browser sent an If-None-Match matching our
    // ETag, the gzip blob it has cached is still current — skip the body.
    char inm[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK
        && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "ETag", etag);
    // Cache-Control: must-revalidate forces the browser to re-check the
    // ETag on every load (no stale-while-revalidate). With no max-age it
    // hits us on every refresh, but the response is just a 304 if the
    // build hasn't changed — ~150 bytes vs ~25 KB.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
    return httpd_resp_send(req, (const char *)INDEX_HTML_GZ_START,
                           INDEX_HTML_GZ_END - INDEX_HTML_GZ_START);
}

// 204 No Content so browsers stop asking. Cheaper than serving an actual icon.
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Captive-portal catch-all: any unknown path → 302 to "/". Combined with the
// captive_portal DNS hijack, this triggers iOS / Android / Windows network
// connectivity probes (captive.apple.com/hotspot-detect.html,
// connectivitycheck.gstatic.com/generate_204, etc.) to pop the OS's mini
// browser pointed at the setup page. Harmless in STA mode (any 404 there
// just becomes a redirect to a valid page).
static esp_err_t not_found_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------- start/stop ----------

esp_err_t cr_http_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.max_uri_handlers  = 48;
    cfg.stack_size        = 8192;
    cfg.recv_wait_timeout = 30;  // longer for OTA upload
    cfg.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t routes[] = {
        { .uri = "/",                         .method = HTTP_GET,  .handler = root_get          },
        { .uri = "/favicon.ico",              .method = HTTP_GET,  .handler = favicon_get       },
        { .uri = "/health",                   .method = HTTP_GET,  .handler = health_get        },
        { .uri = "/metrics",                  .method = HTTP_GET,  .handler = metrics_get       },
        { .uri = "/api/system/status",        .method = HTTP_GET,  .handler = status_get        },
        { .uri = "/api/wifi/scan",            .method = HTTP_GET,  .handler = wifi_scan_get     },
        { .uri = "/api/setup",                .method = HTTP_POST, .handler = setup_post        },
        { .uri = "/api/auth/login",           .method = HTTP_POST, .handler = auth_login_post   },
        { .uri = "/api/auth/logout",          .method = HTTP_POST, .handler = auth_logout_post  },
        { .uri = "/api/auth/me",              .method = HTTP_GET,  .handler = auth_me_get       },
        { .uri = "/api/auth/change_password", .method = HTTP_POST, .handler = auth_change_password_post },
        { .uri = "/api/system/restart",       .method = HTTP_POST, .handler = restart_post      },
        { .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = factory_reset_post },
        { .uri = "/api/system/time_resync",   .method = HTTP_POST, .handler = time_resync_post  },
        { .uri = "/api/system/device_name",   .method = HTTP_GET,  .handler = device_name_get   },
        { .uri = "/api/system/device_name",   .method = HTTP_POST, .handler = device_name_post  },
        { .uri = "/api/system/led",           .method = HTTP_GET,  .handler = led_get           },
        { .uri = "/api/system/led",           .method = HTTP_POST, .handler = led_post          },
        { .uri = "/api/system/ntp_server",    .method = HTTP_GET,  .handler = ntp_server_get    },
        { .uri = "/api/system/ntp_server",    .method = HTTP_POST, .handler = ntp_server_post   },
        { .uri = "/api/system/tz",            .method = HTTP_GET,  .handler = tz_get            },
        { .uri = "/api/system/tz",            .method = HTTP_POST, .handler = tz_post           },
        { .uri = "/api/system/metrics_reset", .method = HTTP_POST, .handler = metrics_reset_post },
        { .uri = "/api/system/logs",          .method = HTTP_GET,  .handler = logs_get          },
        { .uri = "/api/system/logs/clear",    .method = HTTP_POST, .handler = logs_clear_post   },
        { .uri = "/api/system/backup",        .method = HTTP_GET,  .handler = backup_get        },
        { .uri = "/api/system/restore",       .method = HTTP_POST, .handler = restore_post      },
        { .uri = "/api/system/diag",          .method = HTTP_GET,  .handler = diag_get          },
        { .uri = "/api/system/ota",           .method = HTTP_POST, .handler = ota_post          },
        { .uri = "/api/system/wifi",          .method = HTTP_GET,  .handler = wifi_get          },
        { .uri = "/api/system/wifi",          .method = HTTP_POST, .handler = wifi_post         },
        { .uri = "/api/system/rollback",      .method = HTTP_POST, .handler = rollback_post     },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_redirect);

    // App extension hook: a downstream app overrides this weak symbol to
    // register its own routes (sensor endpoints, custom commands, etc.).
    // The default no-op lets crino boot standalone as a pure WiFi/OTA/UI
    // chassis with no app-specific endpoints.
    cr_app_register_routes(s_server);

    ESP_LOGI(TAG, "HTTP server listening on :80 (%d chassis routes)",
             (int)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}

esp_err_t cr_http_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

// Weak default — apps override by defining their own non-weak version.
__attribute__((weak)) void cr_app_register_routes(httpd_handle_t server)
{
    (void)server;
}

// Weak default — apps override to inject fields into /api/system/status.
__attribute__((weak)) void cr_app_status_json(cJSON *root)
{
    (void)root;
}
