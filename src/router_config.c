#include <string.h>
#include <stdio.h>

#include "router_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "router"

static const char *TAG = "config";

static router_config_t s_cfg;

static void sanitize_loaded_config(void);

/* ── NVS helpers ─────────────────────────────────────────────── */

static void load_str(nvs_handle_t h, const char *key,
                     char *dst, size_t dst_len, const char *def)
{
    size_t len = dst_len;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, def, dst_len);
    }
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t def)
{
    if (nvs_get_u8(h, key, dst) != ESP_OK) {
        *dst = def;
    }
}

/*
 * "TTGO-4G-3A7C" — the last two MAC bytes keep several boards on a bench
 * distinguishable without anyone having to configure them first.
 */
static void default_ssid(char *dst, size_t len)
{
    if (ROUTER_DEFAULT_WIFI_SSID[0] != '\0') {
        strlcpy(dst, ROUTER_DEFAULT_WIFI_SSID, len);
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(dst, len, "TTGO-4G-%02X%02X", (unsigned)mac[4], (unsigned)mac[5]);
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t router_config_init(void)
{
    char ssid_def[33];
    default_ssid(ssid_def, sizeof(ssid_def));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* Nothing stored yet — first boot, or right after a factory reset. */
        ESP_LOGW(TAG, "No stored config (%s) — using factory defaults",
                 esp_err_to_name(err));
        memset(&s_cfg, 0, sizeof(s_cfg));
        strlcpy(s_cfg.wifi_ssid,  ssid_def,                   sizeof(s_cfg.wifi_ssid));
        strlcpy(s_cfg.wifi_pass,  ROUTER_DEFAULT_WIFI_PASS,   sizeof(s_cfg.wifi_pass));
        s_cfg.wifi_channel = ROUTER_DEFAULT_WIFI_CHANNEL;
        s_cfg.wifi_max_sta = ROUTER_DEFAULT_WIFI_MAX_STA;
        s_cfg.wifi_hidden  = false;
        strlcpy(s_cfg.ap_ip,      ROUTER_DEFAULT_AP_IP,       sizeof(s_cfg.ap_ip));
        strlcpy(s_cfg.ap_netmask, ROUTER_DEFAULT_AP_NETMASK,  sizeof(s_cfg.ap_netmask));
        strlcpy(s_cfg.apn,        ROUTER_DEFAULT_APN,         sizeof(s_cfg.apn));
        strlcpy(s_cfg.admin_user, ROUTER_DEFAULT_ADMIN_USER,  sizeof(s_cfg.admin_user));
        strlcpy(s_cfg.admin_pass, ROUTER_DEFAULT_ADMIN_PASS,  sizeof(s_cfg.admin_pass));
        return ESP_OK;
    }

    load_str(h, "wifi_ssid",  s_cfg.wifi_ssid,  sizeof(s_cfg.wifi_ssid),  ssid_def);
    load_str(h, "wifi_pass",  s_cfg.wifi_pass,  sizeof(s_cfg.wifi_pass),  ROUTER_DEFAULT_WIFI_PASS);
    load_u8 (h, "wifi_ch",   &s_cfg.wifi_channel, ROUTER_DEFAULT_WIFI_CHANNEL);
    load_u8 (h, "wifi_max",  &s_cfg.wifi_max_sta, ROUTER_DEFAULT_WIFI_MAX_STA);

    uint8_t hidden = 0;
    load_u8(h, "wifi_hide", &hidden, 0);
    s_cfg.wifi_hidden = hidden != 0;

    load_str(h, "ap_ip",      s_cfg.ap_ip,      sizeof(s_cfg.ap_ip),      ROUTER_DEFAULT_AP_IP);
    load_str(h, "ap_mask",    s_cfg.ap_netmask, sizeof(s_cfg.ap_netmask), ROUTER_DEFAULT_AP_NETMASK);
    load_str(h, "apn",        s_cfg.apn,        sizeof(s_cfg.apn),        ROUTER_DEFAULT_APN);
    load_str(h, "ppp_user",   s_cfg.ppp_user,   sizeof(s_cfg.ppp_user),   "");
    load_str(h, "ppp_pass",   s_cfg.ppp_pass,   sizeof(s_cfg.ppp_pass),   "");
    load_str(h, "sim_pin",    s_cfg.sim_pin,    sizeof(s_cfg.sim_pin),    "");
    load_str(h, "admin_user", s_cfg.admin_user, sizeof(s_cfg.admin_user), ROUTER_DEFAULT_ADMIN_USER);
    load_str(h, "admin_pass", s_cfg.admin_pass, sizeof(s_cfg.admin_pass), ROUTER_DEFAULT_ADMIN_PASS);

    sanitize_loaded_config();
    nvs_close(h);
    return ESP_OK;
}

router_config_t *router_config_get(void)
{
    return &s_cfg;
}

static bool valid_ipv4(const char *s)
{
    esp_ip4_addr_t addr;
    return s[0] != '\0' && esp_netif_str_to_ip4(s, &addr) == ESP_OK;
}

static void sanitize_loaded_config(void)
{
    if (s_cfg.wifi_channel < 1 || s_cfg.wifi_channel > ROUTER_WIFI_CHANNEL_MAX) {
        ESP_LOGW(TAG, "Stored WiFi channel %u is unsupported; using channel %u",
                 (unsigned)s_cfg.wifi_channel, (unsigned)ROUTER_DEFAULT_WIFI_CHANNEL);
        s_cfg.wifi_channel = ROUTER_DEFAULT_WIFI_CHANNEL;
    }
    if (s_cfg.wifi_max_sta < 1 || s_cfg.wifi_max_sta > ROUTER_DEFAULT_WIFI_MAX_STA) {
        ESP_LOGW(TAG, "Stored max-client value %u is invalid; using %u",
                 (unsigned)s_cfg.wifi_max_sta, (unsigned)ROUTER_DEFAULT_WIFI_MAX_STA);
        s_cfg.wifi_max_sta = ROUTER_DEFAULT_WIFI_MAX_STA;
    }
    if (!valid_ipv4(s_cfg.ap_ip) || !valid_ipv4(s_cfg.ap_netmask)) {
        ESP_LOGW(TAG, "Stored LAN settings are invalid; using factory subnet");
        strlcpy(s_cfg.ap_ip, ROUTER_DEFAULT_AP_IP, sizeof(s_cfg.ap_ip));
        strlcpy(s_cfg.ap_netmask, ROUTER_DEFAULT_AP_NETMASK, sizeof(s_cfg.ap_netmask));
    }
}

esp_err_t router_config_save(const router_config_t *cfg, const char **err)
{
    const char *reason = NULL;

    size_t ssid_len = strlen(cfg->wifi_ssid);
    size_t pass_len = strlen(cfg->wifi_pass);

    if (ssid_len == 0 || ssid_len > 32) {
        reason = "SSID must be 1-32 characters";
    } else if (pass_len != 0 && (pass_len < 8 || pass_len > 63)) {
        reason = "WiFi password must be empty (open) or 8-63 characters";
    } else if (cfg->wifi_channel < 1 || cfg->wifi_channel > ROUTER_WIFI_CHANNEL_MAX) {
        reason = "WiFi channel must be 1-11";
    } else if (cfg->wifi_max_sta < 1 || cfg->wifi_max_sta > 10) {
        reason = "Max clients must be 1-10";
    } else if (!valid_ipv4(cfg->ap_ip)) {
        reason = "Router IP is not a valid IPv4 address";
    } else if (!valid_ipv4(cfg->ap_netmask)) {
        reason = "Netmask is not a valid IPv4 address";
    } else if (strlen(cfg->apn) == 0) {
        reason = "APN must not be empty";
    } else if (strlen(cfg->admin_user) == 0 || strlen(cfg->admin_pass) == 0) {
        /* An empty admin password would leave the config UI open to every
         * client on the LAN, which includes anyone who guesses the WiFi key. */
        reason = "Admin username and password must not be empty";
    }

    if (reason) {
        if (err) *err = reason;
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        if (err) *err = "Could not open NVS for writing";
        return ret;
    }

    ret  = nvs_set_str(h, "wifi_ssid",  cfg->wifi_ssid);
    ret |= nvs_set_str(h, "wifi_pass",  cfg->wifi_pass);
    ret |= nvs_set_u8 (h, "wifi_ch",    cfg->wifi_channel);
    ret |= nvs_set_u8 (h, "wifi_max",   cfg->wifi_max_sta);
    ret |= nvs_set_u8 (h, "wifi_hide",  cfg->wifi_hidden ? 1 : 0);
    ret |= nvs_set_str(h, "ap_ip",      cfg->ap_ip);
    ret |= nvs_set_str(h, "ap_mask",    cfg->ap_netmask);
    ret |= nvs_set_str(h, "apn",        cfg->apn);
    ret |= nvs_set_str(h, "ppp_user",   cfg->ppp_user);
    ret |= nvs_set_str(h, "ppp_pass",   cfg->ppp_pass);
    ret |= nvs_set_str(h, "sim_pin",    cfg->sim_pin);
    ret |= nvs_set_str(h, "admin_user", cfg->admin_user);
    ret |= nvs_set_str(h, "admin_pass", cfg->admin_pass);

    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    if (ret != ESP_OK) {
        if (err) *err = "Failed to write configuration to flash";
        return ret;
    }

    s_cfg = *cfg;
    ESP_LOGI(TAG, "Configuration saved (SSID=%s APN=%s)", s_cfg.wifi_ssid, s_cfg.apn);
    return ESP_OK;
}

esp_err_t router_config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "Factory reset: stored configuration erased");
    return err;
}
