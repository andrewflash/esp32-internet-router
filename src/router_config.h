/*
 * Persistent router configuration.
 *
 * Everything a user would normally set on a consumer router lives here and is
 * stored in NVS. Nothing in this header is a secret: the defaults are what a
 * freshly flashed board comes up with, and they are meant to be changed from
 * the web UI on first boot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Factory defaults ─────────────────────────────────────────
 * Override at build time with -DROUTER_DEFAULT_x=\"...\" if you flash a batch
 * of boards and want them personalised without touching this file.
 * ─────────────────────────────────────────────────────────── */

/* Empty SSID means "derive from the MAC" → e.g. "TTGO-4G-3A7C". */
#ifndef ROUTER_DEFAULT_WIFI_SSID
#define ROUTER_DEFAULT_WIFI_SSID    ""
#endif
/* Must be >= 8 chars for WPA2, or empty for an open network. */
#ifndef ROUTER_DEFAULT_WIFI_PASS
#define ROUTER_DEFAULT_WIFI_PASS    "12345678"
#endif
#ifndef ROUTER_DEFAULT_APN
#define ROUTER_DEFAULT_APN          "internet"
#endif
#ifndef ROUTER_DEFAULT_ADMIN_USER
#define ROUTER_DEFAULT_ADMIN_USER   "admin"
#endif
#ifndef ROUTER_DEFAULT_ADMIN_PASS
#define ROUTER_DEFAULT_ADMIN_PASS   "admin"
#endif
#ifndef ROUTER_DEFAULT_AP_IP
#define ROUTER_DEFAULT_AP_IP        "192.168.4.1"
#endif
#ifndef ROUTER_DEFAULT_AP_NETMASK
#define ROUTER_DEFAULT_AP_NETMASK   "255.255.255.0"
#endif
#define ROUTER_DEFAULT_WIFI_CHANNEL 6
#define ROUTER_DEFAULT_WIFI_MAX_STA 10

typedef struct {
    /* WiFi access point */
    char     wifi_ssid[33];     /* 32 chars + NUL */
    char     wifi_pass[65];     /* empty = open network */
    uint8_t  wifi_channel;      /* 1..13 */
    uint8_t  wifi_max_sta;      /* 1..10 */
    bool     wifi_hidden;

    /* LAN side */
    char     ap_ip[16];
    char     ap_netmask[16];

    /* Cellular WAN */
    char     apn[65];
    char     ppp_user[33];      /* most carriers leave these empty */
    char     ppp_pass[33];
    char     sim_pin[9];        /* empty if the SIM has no PIN */

    /* Web UI login */
    char     admin_user[17];
    char     admin_pass[33];
} router_config_t;

/*
 * Load the stored configuration into the process-wide instance, filling in
 * factory defaults for anything that was never written. Call once at boot,
 * before router_config_get().
 */
esp_err_t router_config_init(void);

/* The live configuration. Never NULL after router_config_init(). */
router_config_t *router_config_get(void);

/*
 * Validate and persist. Returns ESP_ERR_INVALID_ARG (and leaves the stored
 * config untouched) if a field is out of range; `err` receives a short
 * human-readable reason for the web UI when non-NULL.
 *
 * Changes only take effect after a reboot — the WiFi and PPP stacks are set up
 * once at boot from this struct.
 */
esp_err_t router_config_save(const router_config_t *cfg, const char **err);

/* Erase the stored config so the next boot comes up on factory defaults. */
esp_err_t router_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
