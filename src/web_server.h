/*
 * Configuration web UI ("WiFi manager") served on the SoftAP address.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     wan_up;
    char     wan_ip[16];
    char     wan_gw[16];
    char     wan_dns[16];
    int      rssi;               /* AT+CSQ 0..31, 99 = unknown */
    char     operator_name[40];
    char     modem_state[32];    /* short human-readable WAN phase */
    uint32_t uptime_s;
    int      clients;
    uint32_t heap_free;
    uint32_t heap_min;
} router_status_t;

/* Implemented in main.c — snapshots whatever the WAN task last observed. */
void router_status_get(router_status_t *out);

/* Start the HTTP server. Safe to call once WiFi is up. */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
