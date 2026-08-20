#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web_server.h"
#include "router_config.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "web";

/* index.html is embedded by the EMBED_FILES entry in src/CMakeLists.txt */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ── Auth ────────────────────────────────────────────────────── */

/*
 * HTTP Basic over plain HTTP. This only keeps other clients on the LAN out of
 * the settings page — it is not confidential, since anyone able to sniff the
 * WiFi can read the header. That is the same trade-off every consumer router
 * in this class makes; the WPA2 key is the real boundary.
 */
static bool request_authorized(httpd_req_t *req)
{
    const router_config_t *cfg = router_config_get();

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0 || hdr_len > 200) {
        return false;
    }

    char hdr[201];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(hdr, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char expected[sizeof(cfg->admin_user) + sizeof(cfg->admin_pass)];
    int n = snprintf(expected, sizeof(expected), "%s:%s", cfg->admin_user, cfg->admin_pass);
    if (n < 0 || (size_t)n >= sizeof(expected)) {
        return false;
    }

    /* Length-independent compare is pointless here (the lengths leak anyway on
     * a LAN-local service), but avoid strcmp's early exit out of habit. */
    if (decoded_len != strlen(expected)) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < decoded_len; i++) {
        diff |= (uint8_t)(decoded[i] ^ (uint8_t)expected[i]);
    }
    return diff == 0;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"4G Router\"");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"authentication required\"}");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{\"error\":\"unknown\"}");
    free(body);
    return err;
}

/* ── Handlers ────────────────────────────────────────────────── */

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }

    router_status_t st;
    router_status_get(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "wan_up",    st.wan_up);
    cJSON_AddStringToObject(root, "wan_ip",    st.wan_ip);
    cJSON_AddStringToObject(root, "wan_gw",    st.wan_gw);
    cJSON_AddStringToObject(root, "wan_dns",   st.wan_dns);
    cJSON_AddNumberToObject(root, "rssi",      st.rssi);
    cJSON_AddStringToObject(root, "operator",  st.operator_name);
    cJSON_AddStringToObject(root, "state",     st.modem_state);
    cJSON_AddNumberToObject(root, "uptime_s",  st.uptime_s);
    cJSON_AddStringToObject(root, "reset_reason", st.reset_reason);
    cJSON_AddNumberToObject(root, "clients",   st.clients);
    cJSON_AddNumberToObject(root, "heap_free", st.heap_free);
    cJSON_AddNumberToObject(root, "heap_min",  st.heap_min);
    cJSON_AddStringToObject(root, "lan_ip",    router_config_get()->ap_ip);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
    return err;
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }

    const router_config_t *cfg = router_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid",  cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass",  cfg->wifi_pass);
    cJSON_AddNumberToObject(root, "wifi_ch",    cfg->wifi_channel);
    cJSON_AddNumberToObject(root, "wifi_max",   cfg->wifi_max_sta);
    cJSON_AddBoolToObject(root,   "wifi_hidden", cfg->wifi_hidden);
    cJSON_AddStringToObject(root, "ap_ip",      cfg->ap_ip);
    cJSON_AddStringToObject(root, "ap_netmask", cfg->ap_netmask);
    cJSON_AddStringToObject(root, "apn",        cfg->apn);
    cJSON_AddStringToObject(root, "ppp_user",   cfg->ppp_user);
    cJSON_AddStringToObject(root, "ppp_pass",   cfg->ppp_pass);
    cJSON_AddStringToObject(root, "sim_pin",    cfg->sim_pin);
    cJSON_AddStringToObject(root, "admin_user", cfg->admin_user);
    /* admin_pass is deliberately not returned; the form leaves it blank and
     * only sends it when the user types a new one. */

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{}");
    free(body);
    return err;
}

/* Copy a JSON string field into dst, leaving dst untouched when absent. */
static void json_str(const cJSON *root, const char *key, char *dst, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, len);
    }
}

static void json_u8(const cJSON *root, const char *key, uint8_t *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        *dst = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static void json_bool(const cJSON *root, const char *key, bool *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static esp_err_t read_body(httpd_req_t *req, char **out)
{
    if (req->content_len == 0 || req->content_len > 2048) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    *out = buf;
    return ESP_OK;
}

/*
 * Reboot from a task so the HTTP response is actually flushed first — a
 * restart inside the handler leaves the browser with a dead socket and no
 * confirmation that the save worked.
 */
static void deferred_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "Rebooting to apply new settings");
    esp_restart();
}

static esp_err_t schedule_restart(void)
{
    return xTaskCreate(deferred_restart_task, "reboot", 2048, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }

    char *body = NULL;
    esp_err_t err = read_body(req, &body);
    if (err != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "Could not read request body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_json_error(req, "400 Bad Request", "Malformed JSON");
    }

    /* Start from the running config so any field the form omits is preserved. */
    router_config_t cfg = *router_config_get();

    json_str (root, "wifi_ssid",   cfg.wifi_ssid,  sizeof(cfg.wifi_ssid));
    json_str (root, "wifi_pass",   cfg.wifi_pass,  sizeof(cfg.wifi_pass));
    json_u8  (root, "wifi_ch",    &cfg.wifi_channel);
    json_u8  (root, "wifi_max",   &cfg.wifi_max_sta);
    json_bool(root, "wifi_hidden", &cfg.wifi_hidden);
    json_str (root, "ap_ip",       cfg.ap_ip,      sizeof(cfg.ap_ip));
    json_str (root, "ap_netmask",  cfg.ap_netmask, sizeof(cfg.ap_netmask));
    json_str (root, "apn",         cfg.apn,        sizeof(cfg.apn));
    json_str (root, "ppp_user",    cfg.ppp_user,   sizeof(cfg.ppp_user));
    json_str (root, "ppp_pass",    cfg.ppp_pass,   sizeof(cfg.ppp_pass));
    json_str (root, "sim_pin",     cfg.sim_pin,    sizeof(cfg.sim_pin));
    json_str (root, "admin_user",  cfg.admin_user, sizeof(cfg.admin_user));

    /* Blank admin_pass means "keep the current one" — the form never shows it. */
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(root, "admin_pass");
    if (cJSON_IsString(ap) && ap->valuestring && ap->valuestring[0] != '\0') {
        strlcpy(cfg.admin_pass, ap->valuestring, sizeof(cfg.admin_pass));
    }

    bool reboot = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reboot"));
    cJSON_Delete(root);

    const char *reason = "Invalid configuration";
    if (router_config_save(&cfg, &reason) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", reason);
    }

    if (reboot) {
        if (schedule_restart() != ESP_OK) {
            return send_json_error(req, "503 Service Unavailable",
                                   "Settings saved, but the reboot task could not start");
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }
    if (schedule_restart() != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable", "Reboot task could not start");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }
    if (router_config_factory_reset() != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Erase failed");
    }
    if (schedule_restart() != ESP_OK) {
        return send_json_error(req, "503 Service Unavailable",
                               "Settings erased, but the reboot task could not start");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ── Start ───────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size      = 6144;      /* cJSON + TLS-free handlers */
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    /* The UI polls /api/status, so keep sockets from lingering. */
    cfg.keep_alive_enable = false;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = index_get_handler },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/config",        .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",        .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = reboot_post_handler },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "Config UI at http://%s/", router_config_get()->ap_ip);
    return ESP_OK;
}
