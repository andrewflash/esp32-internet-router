/*
 * TTGO T-Internet-COM — 4G → WiFi Router
 *
 * Topology:
 *   [4G Network] ←→ [A7670E / Mini PCIE] ←→ [UART1 PPP] ←→ [ESP32 lwIP NAPT] ←→ [WiFi SoftAP] ←→ [Clients]
 *
 * Board pins (from T-Internet-COM schematic):
 *   PCIE-TX  = GPIO33  (ESP32 UART TX → Modem RX)
 *   PCIE-RX  = GPIO35  (Modem TX → ESP32 UART RX, input-only GPIO — fine for UART RX)
 *   GPIO32   = Modem PWRKEY, asserted HIGH (see the note at MODEM_PWRKEY below)
 *
 * SSID, password, APN and the rest are stored in NVS and edited from the web UI
 * at the SoftAP address — see router_config.h for the factory defaults.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_mac.h"
#include "esp_modem_api.h"
#include "lwip/lwip_napt.h"         /* ip_napt_enable() — needs CONFIG_LWIP_IPV4_NAPT=y */

#include "router_config.h"
#include "web_server.h"

/* ── Board pins ──────────────────────────────────────────────── */
#define MODEM_UART_TX   33
#define MODEM_UART_RX   35
#define MODEM_UART_PORT UART_NUM_1

/*
 * GPIO32 is PWRKEY, not a reset line — the board pinout image calls it
 * "PCIE-RST", but every LilyGo T-Internet-COM example defines it as
 * MODEM_PWRKEY, and there is no other modem control GPIO on this board.
 *
 * It is asserted by driving the ESP32 pin HIGH: the A7670E's PWRKEY is
 * internally pulled up to VBAT and triggers on a LOW, so an inverting stage
 * sits between the two. Driving it LOW asserts nothing at all, which is why
 * earlier "reset pulses" here appeared to do nothing whatsoever.
 */
#define MODEM_PWRKEY    32
#define PWRKEY_ASSERT   1
#define PWRKEY_RELEASE  0

/* Fixed. AT+IPR=460800 was tried and the modem did not follow: the link filled
 * with Rx Break and framing garbage, and it then needed a power-cycle. There is
 * no RTS/CTS on the Mini PCIE slot to make a higher rate reliable, so 115200 is
 * the supported rate and the WAN ceiling is ~11 kB/s. */
#define MODEM_BAUD_INIT 115200

/* ── Timing ──────────────────────────────────────────────────── */
/* From A7672X/A7670X Series Hardware Design V1.03 §3.2. The module measures
 * these at its own pin; the numbers below add margin on top of the minimums. */
#define PWRKEY_ON_MS        300     /* ≥50 ms typ. to power on */
#define PWRKEY_OFF_MS       3000    /* ≥2.5 s to power off — a short pulse is ignored */
#define MODEM_OFF_ON_GAP_MS 1500    /* Toff-on buffer; the datasheet leaves it blank */
#define MODEM_UART_READY_MS 9000    /* UART ready ~8 s after power-on (STATUS high ~7 s) */

#define MODEM_BOOT_MS       10000   /* wait after AT+CRESET before retrying AT */
#define MODEM_SYNC_MS       30000   /* max time to get an "OK" back from AT */
#define MODEM_REG_MS        60000   /* max time to attach to the network */
#define PPP_TIMEOUT_MS      60000   /* max time to get PPP IP */
#define PPP_MODE_RETRIES    5       /* ATD*99# attempts before giving up */
#define RECONNECT_DELAY_MS  3000    /* shorter gap: the outage is visible to clients */
#define WAN_RETRY_DELAY_MS  15000   /* pause before restarting a failed bring-up */

/* ── Event bits ──────────────────────────────────────────────── */
#define BIT_PPP_GOT_IP  BIT0
#define BIT_PPP_LOST_IP BIT1

static const char *TAG = "4G-Router";

static EventGroupHandle_t  s_event_group;
static esp_netif_t        *s_ppp_netif = NULL;
static esp_netif_t        *s_ap_netif  = NULL;
static esp_modem_dce_t    *s_dce       = NULL;

/* Status shown in the web UI. Written only by the WAN task and the IP event
 * handler; the reader takes a plain snapshot, which is good enough for a
 * status page. */
static router_status_t s_status = { .rssi = 99, .modem_state = "starting" };

static void status_set_state(const char *state)
{
    strlcpy(s_status.modem_state, state, sizeof(s_status.modem_state));
}

void router_status_get(router_status_t *out)
{
    *out = s_status;

    out->uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000);
    out->heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    wifi_sta_list_t sta_list;
    out->clients = (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) ? sta_list.num : 0;
}

/* ── Helpers ─────────────────────────────────────────────────── */

/*
 * Quieten the components that log per-packet or per-association once the
 * router is running.
 *
 * "esp-netif_lwip-ppp" is set to NONE rather than WARN because the
 * pppos_input_tcpip ERR_MEM line is logged at ERROR level and would still come
 * through at WARN. Nothing is lost: PPP up/down still surfaces through our own
 * IP_EVENT handler as "PPP connected" / "PPP lost IP".
 */
static void quiet_noisy_logs(void)
{
    esp_log_level_set("esp-netif_lwip-ppp", ESP_LOG_NONE);
    esp_log_level_set("esp_modem_netif", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("uart", ESP_LOG_WARN);
    esp_log_level_set("httpd_uri", ESP_LOG_WARN);
}

/*
 * Push the carrier's DNS to the SoftAP DHCP server so WiFi clients
 * receive a working DNS server address automatically.
 */
static void ap_dhcps_set_dns(esp_netif_t *ap_netif, uint32_t dns_addr)
{
    esp_netif_dhcps_stop(ap_netif);

    /* OFFER_DNS = 0x02 from dhcpserver.h; use raw value to avoid internal header */
    uint8_t offer_dns = 0x02;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));

    esp_netif_dns_info_t dns = { .ip = { .type = ESP_IPADDR_TYPE_V4 } };
    dns.ip.u_addr.ip4.addr = dns_addr;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    esp_netif_dhcps_start(ap_netif);
    char dns_s[16];
    snprintf(dns_s, sizeof(dns_s), IPSTR, IP2STR((ip4_addr_t *)&dns_addr));
    ESP_LOGI(TAG, "DHCP-DNS set to %s", dns_s);
}

/* ── Event handlers ──────────────────────────────────────────── */

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base != IP_EVENT) return;

    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *ev = data;

        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);

        snprintf(s_status.wan_ip,  sizeof(s_status.wan_ip),  IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(s_status.wan_gw,  sizeof(s_status.wan_gw),  IPSTR, IP2STR(&ev->ip_info.gw));
        snprintf(s_status.wan_dns, sizeof(s_status.wan_dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));

        ESP_LOGI(TAG, "PPP connected  IP=%s  GW=%s  DNS=%s",
                 s_status.wan_ip, s_status.wan_gw, s_status.wan_dns);

        /* Forward carrier DNS to WiFi clients */
        if (s_ap_netif) {
            ap_dhcps_set_dns(s_ap_netif, dns.ip.u_addr.ip4.addr);
        }

        xEventGroupSetBits(s_event_group, BIT_PPP_GOT_IP);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        s_status.wan_up = false;
        status_set_state("disconnected");
        xEventGroupSetBits(s_event_group, BIT_PPP_LOST_IP);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    /* MACSTR/MAC2STR cause preprocessor errors inside IDF 6.x multi-branch
     * ESP_LOG_LEVEL macro. Format MAC into a buffer first. */
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = data;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned)ev->mac[0], (unsigned)ev->mac[1], (unsigned)ev->mac[2],
                 (unsigned)ev->mac[3], (unsigned)ev->mac[4], (unsigned)ev->mac[5]);
        ESP_LOGI(TAG, "Client joined  MAC=%s  AID=%d", mac, ev->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = data;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned)ev->mac[0], (unsigned)ev->mac[1], (unsigned)ev->mac[2],
                 (unsigned)ev->mac[3], (unsigned)ev->mac[4], (unsigned)ev->mac[5]);
        ESP_LOGI(TAG, "Client left    MAC=%s  AID=%d", mac, ev->aid);
    }
}

/* ── WiFi SoftAP init ────────────────────────────────────────── */

/* Move the AP off the 192.168.4.1 default if the user picked another subnet.
 * Must run before esp_wifi_start() so the DHCP server hands out the new pool. */
static void ap_apply_static_ip(const router_config_t *cfg)
{
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(cfg->ap_ip, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->ap_netmask, &ip_info.netmask) != ESP_OK) {
        ESP_LOGW(TAG, "Stored LAN address is invalid — keeping the default");
        return;
    }
    ip_info.gw = ip_info.ip;   /* the router is its own gateway for clients */

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
}

static void wifi_ap_start(const router_config_t *cfg)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);

    ap_apply_static_ip(cfg);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid_len       = strlen(cfg->wifi_ssid),
            .channel        = cfg->wifi_channel,
            .max_connection = cfg->wifi_max_sta,
            .ssid_hidden    = cfg->wifi_hidden ? 1 : 0,
            /* An empty password is the documented way to run the AP open. */
            .authmode       = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_cfg.ap.ssid,     cfg->wifi_ssid, sizeof(wifi_cfg.ap.ssid));
    strlcpy((char *)wifi_cfg.ap.password, cfg->wifi_pass, sizeof(wifi_cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started  SSID=%s  CH=%d  IP=%s  security=%s",
             cfg->wifi_ssid, cfg->wifi_channel, cfg->ap_ip,
             cfg->wifi_pass[0] ? "WPA2" : "OPEN");
}

/* ── Modem power control (PWRKEY) ────────────────────────────── */

static void modem_pwrkey_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MODEM_PWRKEY,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(MODEM_PWRKEY, PWRKEY_RELEASE);
}

static void modem_pwrkey_pulse(int assert_ms)
{
    gpio_set_level(MODEM_PWRKEY, PWRKEY_ASSERT);
    vTaskDelay(pdMS_TO_TICKS(assert_ms));
    gpio_set_level(MODEM_PWRKEY, PWRKEY_RELEASE);
}

/*
 * Power-on pulse. Harmless on a module that is already running: powering off
 * needs the PWRKEY held for 2.5 s, so this shorter pulse is ignored.
 */
static void modem_power_on(void)
{
    ESP_LOGI(TAG, "PWRKEY power-on pulse (%d ms)", PWRKEY_ON_MS);
    modem_pwrkey_pulse(PWRKEY_ON_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_UART_READY_MS));
}

/*
 * The only recovery that works on a modem which has stopped answering AT.
 * AT+CRESET and AT+CPOF both need a responsive modem, and rebooting the ESP32
 * does not touch the module's power at all.
 *
 * SIMCom warns against cutting VBAT on a running module (it can corrupt flash);
 * the PWRKEY long-press is the sanctioned way to do this from firmware.
 */
static void modem_power_cycle(void)
{
    ESP_LOGW(TAG, "Power-cycling modem via PWRKEY (off %d ms, on %d ms)",
             PWRKEY_OFF_MS, PWRKEY_ON_MS);

    modem_pwrkey_pulse(PWRKEY_OFF_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_OFF_ON_GAP_MS));
    modem_power_on();
}

/* ── Modem / PPP ─────────────────────────────────────────────── */

/*
 * A modem that is silent on AT is usually not broken — it is still in PPP
 * data mode from a previous run. An ESP32 reset does not touch it, and
 * PCIE-RST does nothing on this board. ESP_MODEM_MODE_DETECT
 * probes the link, and the follow-up COMMAND transition sends the "+++" escape
 * with the required guard times.
 */
static void modem_escape_stale_data_mode(void)
{
    ESP_LOGW(TAG, "No AT reply — probing for a stale PPP session");
    esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DETECT);
    ESP_LOGI(TAG, "Mode detect returned %s", esp_err_to_name(err));

    /* Fails harmlessly if the probe already left us in command mode. */
    esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
}

/* The link is fixed at MODEM_BAUD_INIT — see the baud note at the top. */
static bool modem_wait_at_ready(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int round = 0;

    while (esp_timer_get_time() < deadline) {
        if (esp_modem_sync(s_dce) == ESP_OK) {
            ESP_LOGI(TAG, "Modem responds to AT");
            return true;
        }
        /* Give a booting modem a few seconds before assuming stale PPP */
        if (++round % 6 == 0) {
            modem_escape_stale_data_mode();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

/*
 * Reset the modem over AT rather than PCIE-RST. Pulsing GPIO32 does not reset
 * this module, so AT+CRESET is the only reset actually available to us — but it
 * needs a modem that still answers AT, hence it is a recovery step and not the
 * normal boot path.
 */
static bool modem_reset_via_at(void)
{
    char resp[64];

    ESP_LOGW(TAG, "Resetting modem with AT+CRESET ...");
    if (esp_modem_at(s_dce, "AT+CRESET", resp, 5000) != ESP_OK) {
        ESP_LOGW(TAG, "AT+CRESET not accepted — modem is not answering AT");
        return false;
    }

    ESP_LOGI(TAG, "Waiting %d ms for modem to come back ...", MODEM_BOOT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));
    return modem_wait_at_ready(MODEM_SYNC_MS);
}

/*
 * Escalating bring-up, cheapest first. The modem is normally already powered
 * and answering, so the common case costs one AT round-trip:
 *
 *   1. plain AT sync, with the "+++" stale-PPP escape folded in
 *   2. PWRKEY power-on pulse — covers a module that is simply off
 *   3. full PWRKEY power cycle — the only thing that reaches a wedged module
 */
static bool modem_bring_up(void)
{
    if (modem_wait_at_ready(MODEM_SYNC_MS)) {
        return true;
    }

    ESP_LOGW(TAG, "Modem silent after %d ms — sending power-on pulse", MODEM_SYNC_MS);
    status_set_state("powering on modem");
    modem_power_on();
    if (modem_wait_at_ready(MODEM_SYNC_MS)) {
        return true;
    }

    ESP_LOGW(TAG, "Still silent — power-cycling the modem");
    status_set_state("power-cycling modem");
    modem_power_cycle();
    if (modem_wait_at_ready(MODEM_SYNC_MS)) {
        return true;
    }

    ESP_LOGE(TAG, "Modem unreachable even after a PWRKEY power cycle — "
                  "check the SIM, the antenna and the 3.8 V rail.");
    return false;
}

/* Unlock the SIM if a PIN is configured. Wrong PINs are not retried: three
 * attempts lock the card and only a PUK gets it back. */
static void modem_unlock_sim(const router_config_t *cfg)
{
    if (cfg->sim_pin[0] == '\0') {
        return;
    }
    bool ready = false;
    esp_modem_read_pin(s_dce, &ready);
    if (ready) {
        return;
    }
    if (esp_modem_set_pin(s_dce, cfg->sim_pin) != ESP_OK) {
        ESP_LOGE(TAG, "SIM PIN rejected — check the PIN in the web UI");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
}

static void modem_read_operator(void)
{
    char resp[80];
    if (esp_modem_at(s_dce, "AT+COPS?", resp, 5000) != ESP_OK) {
        return;
    }
    /* +COPS: 0,0,"Telkomsel",7 — take what is between the quotes. */
    char *open = strchr(resp, '"');
    if (!open) return;
    char *close = strchr(open + 1, '"');
    if (!close) return;

    size_t len = (size_t)(close - open - 1);
    if (len >= sizeof(s_status.operator_name)) {
        len = sizeof(s_status.operator_name) - 1;
    }
    memcpy(s_status.operator_name, open + 1, len);
    s_status.operator_name[len] = '\0';
}

static bool modem_wait_registered(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)MODEM_REG_MS * 1000;
    char reg[64];

    while (esp_timer_get_time() < deadline) {
        /* +CEREG: <n>,<stat> — stat 1 = home, 5 = roaming */
        if (esp_modem_at(s_dce, "AT+CEREG?", reg, 5000) == ESP_OK) {
            char *stat = strchr(reg, ',');
            if (stat && (stat[1] == '1' || stat[1] == '5')) {
                int rssi = 99, ber = 0;
                esp_modem_get_signal_quality(s_dce, &rssi, &ber);
                s_status.rssi = rssi;
                modem_read_operator();
                ESP_LOGI(TAG, "Registered on network  RSSI=%d  operator=%s",
                         rssi, s_status.operator_name);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "Network registration timeout (%d ms)", MODEM_REG_MS);
    return false;
}

/* Dial into PPP, retrying — a single ATD*99# failure is not fatal. */
static bool modem_enter_data_mode(void)
{
    for (int i = 1; i <= PPP_MODE_RETRIES; i++) {
        esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        if (err == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "esp_modem_set_mode(DATA) failed (%s), attempt %d/%d",
                 esp_err_to_name(err), i, PPP_MODE_RETRIES);

        /* Drop back to command mode so the next dial starts from a known state */
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_modem_sync(s_dce);
    }
    return false;
}

static void modem_create(const router_config_t *cfg)
{
    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&ppp_cfg);
    assert(s_ppp_netif);

    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.tx_io_num      = MODEM_UART_TX;
    dte_cfg.uart_config.rx_io_num      = MODEM_UART_RX;
    dte_cfg.uart_config.rts_io_num     = -1;
    dte_cfg.uart_config.cts_io_num     = -1;
    dte_cfg.uart_config.baud_rate      = MODEM_BAUD_INIT;
    dte_cfg.uart_config.rx_buffer_size = 8192;
    /* PPP output runs on the lwIP TCPIP thread and uart_write_bytes() blocks
     * until the ring accepts the frame. With the 512-byte default a single
     * 1500-byte packet stalls the whole stack for >100 ms at 115200, starving
     * the PPP receive path — that is the pppos_input_tcpip ERR_MEM flood.
     * Size the ring to hold several full frames so the write returns at once. */
    dte_cfg.uart_config.tx_buffer_size = 8192;
    /* One full PPP frame per read, instead of ~50-byte chunks each costing a
     * pbuf and a TCPIP in-packet message. */
    dte_cfg.dte_buffer_size            = 1600;
    dte_cfg.task_stack_size            = 4096;
    dte_cfg.task_priority              = 10;

    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);

    /* A7670E is SIM7600-compatible at the AT-command level */
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, s_ppp_netif);
    assert(s_dce);

    /* Carriers that need PAP/CHAP credentials on the APN; most leave these
     * empty, in which case esp_modem skips authentication entirely. */
    if (cfg->ppp_user[0] || cfg->ppp_pass[0]) {
        esp_netif_ppp_set_auth(s_ppp_netif, NETIF_PPP_AUTHTYPE_PAP,
                               cfg->ppp_user, cfg->ppp_pass);
    }
}

/* ── NAPT ────────────────────────────────────────────────────── */

static void napt_enable(void)
{
    /* esp_netif_napt_enable() also flags the netif inside esp_netif, which
     * ip_napt_enable() alone does not do. */
    ESP_ERROR_CHECK(esp_netif_napt_enable(s_ap_netif));
    ESP_LOGI(TAG, "NAPT enabled — WiFi→PPP routing active");
}

/* ── WAN task ────────────────────────────────────────────────── */

/*
 * Owns the modem for the lifetime of the board: bring-up, dial, and redial
 * after a drop. It deliberately never reboots the ESP32 on failure — a reboot
 * does not touch the modem anyway, and it would take the config UI down with
 * it, which is exactly when the user needs to fix a wrong APN or SIM PIN.
 */
static void wan_task(void *arg)
{
    const router_config_t *cfg = router_config_get();

    modem_pwrkey_init();
    modem_create(cfg);

    while (1) {
        status_set_state("waiting for modem");
        if (!modem_bring_up()) {
            status_set_state("modem unreachable");
            vTaskDelay(pdMS_TO_TICKS(WAN_RETRY_DELAY_MS));
            continue;
        }

        modem_unlock_sim(cfg);

        status_set_state("registering");
        if (!modem_wait_registered()) {
            status_set_state("no network");
            vTaskDelay(pdMS_TO_TICKS(WAN_RETRY_DELAY_MS));
            continue;
        }

        status_set_state("dialling");
        ESP_LOGI(TAG, "Starting PPP ...");
        if (!modem_enter_data_mode()) {
            /* AT+CRESET is gentler and keeps the ~10 s power-on wait off the
             * table, so try it first; fall back to PWRKEY when the modem has
             * stopped answering AT altogether. */
            ESP_LOGW(TAG, "Dial failed — resetting modem");
            status_set_state("dial failed");
            if (!modem_reset_via_at()) {
                status_set_state("power-cycling modem");
                modem_power_cycle();
            }
            continue;
        }

        xEventGroupClearBits(s_event_group, BIT_PPP_GOT_IP | BIT_PPP_LOST_IP);
        EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                               BIT_PPP_GOT_IP | BIT_PPP_LOST_IP,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(PPP_TIMEOUT_MS));

        if (bits & BIT_PPP_GOT_IP) {
            napt_enable();
            s_status.wan_up = true;
            status_set_state("connected");
            ESP_LOGI(TAG, "Router operational  SSID=%s  LAN=%s",
                     cfg->wifi_ssid, cfg->ap_ip);

            /* Block until the link drops, then fall through and redial. */
            xEventGroupWaitBits(s_event_group, BIT_PPP_LOST_IP,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            s_status.wan_up = false;
            ESP_LOGW(TAG, "PPP dropped — reconnecting in %d s ...",
                     RECONNECT_DELAY_MS / 1000);
        } else {
            ESP_LOGE(TAG, "PPP did not come up within %d s", PPP_TIMEOUT_MS / 1000);
            status_set_state("PPP timeout");
        }

        /* Back to a known state before the next dial. */
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* ── app_main ────────────────────────────────────────────────── */

void app_main(void)
{
    /* NVS — required by WiFi and by the stored configuration */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(router_config_init());
    const router_config_t *cfg = router_config_get();

    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  TTGO T-Internet-COM 4G Router   ║");
    ESP_LOGI(TAG, "║  SSID : %-24s║", cfg->wifi_ssid);
    ESP_LOGI(TAG, "║  APN  : %-24s║", cfg->apn);
    ESP_LOGI(TAG, "║  Setup: http://%-17s║", cfg->ap_ip);
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    quiet_noisy_logs();

    /* WiFi and the config UI come up first and stay up regardless of the WAN,
     * so a wrong APN or a missing SIM can always be corrected from a browser. */
    wifi_ap_start(cfg);
    ESP_ERROR_CHECK(web_server_start());

    xTaskCreate(wan_task, "wan", 4096, NULL, 5, NULL);

    /* Periodic status log */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        router_status_t st;
        router_status_get(&st);
        /* pbufs come from the internal DRAM heap (MEM_LIBC_MALLOC=1), so a
         * collapsing internal-free number means ERR_MEM is real exhaustion
         * rather than the TCPIP thread stalling. */
        ESP_LOGI(TAG, "[STATUS] wan=%s  clients=%d  heap_int=%u  heap_min=%u",
                 st.modem_state, st.clients,
                 (unsigned)st.heap_free, (unsigned)st.heap_min);
    }
}
