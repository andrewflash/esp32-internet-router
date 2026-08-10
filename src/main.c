/*
 * TTGO T-Internet-COM — 4G → WiFi Router
 *
 * Topology:
 *   [4G Network] ←→ [A7670E / Mini PCIE] ←→ [UART1 PPP] ←→ [ESP32 lwIP NAPT] ←→ [WiFi SoftAP] ←→ [Clients]
 *
 * Board pins (from T-Internet-COM schematic):
 *   PCIE-TX  = GPIO33  (ESP32 UART TX → Modem RX)
 *   PCIE-RX  = GPIO35  (Modem TX → ESP32 UART RX, input-only GPIO — fine for UART RX)
 *   PCIE-RST = GPIO32  (Modem hardware reset, active LOW)
 *   No dedicated PWRKEY GPIO; modem powers via Mini PCIE 3.8 V rail.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "esp_modem_api.h"
#include "lwip/lwip_napt.h"         /* ip_napt_enable() — needs CONFIG_LWIP_IPV4_NAPT=y */

/* ── Board pins ──────────────────────────────────────────────── */
#define MODEM_UART_TX   33
#define MODEM_UART_RX   35
#define MODEM_RST_PIN   32          /* PCIE-RST; pulse LOW 200 ms to reset */

/* ── Carrier settings ─────────────────────────────────────────
 * Common Indonesian APNs:
 *   Telkomsel: "internet"  | Indosat: "indosatgprs" | XL: "xlgprs"
 *   Three(3) : "3gprs"    | Smartfren: "smart"
 * ─────────────────────────────────────────────────────────── */
#define APN             "internet"
#define SIM_PIN         ""          /* leave empty if SIM has no PIN */

/* ── WiFi Access Point ────────────────────────────────────────── */
#define WIFI_SSID       "CHANGEME-SSID"
#define WIFI_PASSWORD   "CHANGEME-PASSWORD"
#define WIFI_CHANNEL    6
#define WIFI_MAX_STA    10

/* ── Timing ──────────────────────────────────────────────────── */
#define MODEM_RESET_MS      200     /* RST pulse width */
#define MODEM_BOOT_MS       8000    /* wait after RST before AT commands */
#define PPP_TIMEOUT_MS      60000   /* max time to get PPP IP */
#define RECONNECT_DELAY_MS  10000

/* ── Event bits ──────────────────────────────────────────────── */
#define BIT_PPP_GOT_IP  BIT0
#define BIT_PPP_LOST_IP BIT1

static const char *TAG = "4G-Router";

static EventGroupHandle_t  s_event_group;
static esp_netif_t        *s_ppp_netif = NULL;
static esp_netif_t        *s_ap_netif  = NULL;
static esp_modem_dce_t    *s_dce       = NULL;

/* ── Helpers ─────────────────────────────────────────────────── */

static void modem_reset(void)
{
    ESP_LOGI(TAG, "Resetting modem via GPIO%d ...", MODEM_RST_PIN);
    gpio_set_direction(MODEM_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MODEM_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_RESET_MS));
    gpio_set_level(MODEM_RST_PIN, 1);
    ESP_LOGI(TAG, "Waiting %d ms for modem boot ...", MODEM_BOOT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_MS));
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
        char ip_s[16], gw_s[16], dns_s[16];
        snprintf(ip_s, sizeof(ip_s), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(gw_s, sizeof(gw_s), IPSTR, IP2STR(&ev->ip_info.gw));

        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        snprintf(dns_s, sizeof(dns_s), IPSTR, IP2STR(&dns.ip.u_addr.ip4));

        ESP_LOGI(TAG, "PPP connected  IP=%s  GW=%s  DNS=%s", ip_s, gw_s, dns_s);

        /* Forward carrier DNS to WiFi clients */
        if (s_ap_netif) {
            ap_dhcps_set_dns(s_ap_netif, dns.ip.u_addr.ip4.addr);
        }

        xEventGroupSetBits(s_event_group, BIT_PPP_GOT_IP);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
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

static void wifi_ap_start(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid           = WIFI_SSID,
            .ssid_len       = strlen(WIFI_SSID),
            .password       = WIFI_PASSWORD,
            .channel        = WIFI_CHANNEL,
            .max_connection = WIFI_MAX_STA,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started  SSID=%-12s  CH=%d  IP=192.168.4.1",
             WIFI_SSID, WIFI_CHANNEL);
}

/* ── Modem / PPP init ────────────────────────────────────────── */

static void ppp_start(void)
{
    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&ppp_cfg);
    assert(s_ppp_netif);

    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.tx_io_num      = MODEM_UART_TX;
    dte_cfg.uart_config.rx_io_num      = MODEM_UART_RX;
    dte_cfg.uart_config.rts_io_num     = -1;
    dte_cfg.uart_config.cts_io_num     = -1;
    dte_cfg.uart_config.baud_rate      = 115200;
    dte_cfg.uart_config.rx_buffer_size = 16384;
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.task_stack_size            = 4096;
    dte_cfg.task_priority              = 5;

    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(APN);

    /* A7670E is SIM7600-compatible at the AT-command level */
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, s_ppp_netif);
    assert(s_dce);

    /* Unlock SIM if PIN is set */
    if (strlen(SIM_PIN) > 0) {
        bool ready = false;
        esp_modem_read_pin(s_dce, &ready);
        if (!ready) {
            ESP_ERROR_CHECK(esp_modem_set_pin(s_dce, SIM_PIN));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    /* Switch to PPP (data) mode */
    ESP_LOGI(TAG, "Starting PPP ...");
    ESP_ERROR_CHECK(esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA));
}

/* ── NAPT enable ─────────────────────────────────────────────── */

static void napt_enable(void)
{
    /* Read the actual configured SoftAP IP, then enable NAPT on it.
     * ip_napt_enable() is void in IDF 6.x — no return value to check. */
    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
    ip_napt_enable(ip_info.ip.addr, 1);
    ESP_LOGI(TAG, "NAPT enabled — WiFi→PPP routing active");
}

/* ── Reconnect task ──────────────────────────────────────────── */

static void reconnect_task(void *arg)
{
    while (1) {
        /* Wait for PPP to drop */
        xEventGroupWaitBits(s_event_group, BIT_PPP_LOST_IP,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        ESP_LOGW(TAG, "PPP dropped — reconnecting in %d s ...",
                 RECONNECT_DELAY_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));

        /* Stop PPP mode, restart, wait for IP */
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);

        EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                               BIT_PPP_GOT_IP | BIT_PPP_LOST_IP,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(PPP_TIMEOUT_MS));
        if (bits & BIT_PPP_GOT_IP) {
            napt_enable();
        } else {
            ESP_LOGE(TAG, "Reconnect timeout — rebooting");
            esp_restart();
        }
    }
}

/* ── app_main ────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════╗");
    ESP_LOGI(TAG, "║  TTGO T-Internet-COM 4G Router   ║");
    ESP_LOGI(TAG, "║  SSID : %-24s║", WIFI_SSID);
    ESP_LOGI(TAG, "║  APN  : %-24s║", APN);
    ESP_LOGI(TAG, "╚══════════════════════════════════╝");

    /* NVS — required by WiFi */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    /* 1. Hard-reset modem for a clean start */
    modem_reset();

    /* 2. Start WiFi AP (clients can associate while PPP is connecting) */
    wifi_ap_start();

    /* 3. Start PPP */
    ppp_start();

    /* 4. Wait for PPP IP (blocks up to PPP_TIMEOUT_MS) */
    ESP_LOGI(TAG, "Waiting for PPP IP (timeout %d s) ...", PPP_TIMEOUT_MS / 1000);
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           BIT_PPP_GOT_IP | BIT_PPP_LOST_IP,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(PPP_TIMEOUT_MS));

    if (!(bits & BIT_PPP_GOT_IP)) {
        ESP_LOGE(TAG, "PPP connect timeout — rebooting");
        esp_restart();
    }

    /* 5. Enable NAT routing: SoftAP subnet → PPP upstream */
    napt_enable();

    ESP_LOGI(TAG, "Router operational  SSID=%s  AP=192.168.4.1", WIFI_SSID);

    /* 6. Background task handles PPP drops and reconnects */
    xTaskCreate(reconnect_task, "reconnect", 4096, NULL, 5, NULL);

    /* 7. Periodic status log */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        wifi_sta_list_t sta_list;
        esp_wifi_ap_get_sta_list(&sta_list);
        ESP_LOGI(TAG, "[STATUS] WiFi clients: %d", sta_list.num);
    }
}
