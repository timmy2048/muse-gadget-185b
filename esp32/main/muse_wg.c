/*
 * WireGuard client: after Wi-Fi has an IP, sync time (needed for handshake
 * and TLS), bring up a tunnel, then make it the default route so Muse
 * HTTPS (api.muse.ai) exits via the peer.
 */
#include "sdkconfig.h"
#include "muse_wg.h"

#if CONFIG_MUSE_WG_ENABLE

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_wireguard.h"
#include "wifi_mgr.h"

static const char *TAG = "muse.wg";

static wireguard_ctx_t s_ctx;
static wireguard_config_t s_cfg;
static bool s_inited;
static bool s_up;
static bool s_task_running;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static bool time_is_sane(void)
{
    time_t now = time(NULL);
    /* WireGuard rejects handshakes far from UTC; anything after 2024 is fine. */
    return now > 1700000000;
}

static void sntp_sync(void)
{
    if (time_is_sane()) {
        ESP_LOGI(TAG, "clock already set");
        return;
    }

    ESP_LOGI(TAG, "syncing time via SNTP (ntp.aliyun.com)");
    /* Default IDF builds often allow only one SNTP server slot. */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = true;
    config.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(err));
    }

    for (int i = 0; i < 30 && !time_is_sane(); i++) {
        (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500));
        /* sync_wait can return immediately; always pace the loop. */
        vTaskDelay(pdMS_TO_TICKS(500));
        if (time_is_sane()) break;
    }

    if (time_is_sane()) {
        ESP_LOGI(TAG, "SNTP ok, unix=%lld", (long long)time(NULL));
    } else {
        ESP_LOGW(TAG, "SNTP timed out; WireGuard handshake may fail");
    }
}

static void set_dns_v4(const char *dotted)
{
    esp_netif_t *sta = wifi_mgr_get_netif();
    if (!sta || !dotted || !dotted[0]) return;

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(dotted, &dns.ip.u_addr.ip4) != ESP_OK) {
        ESP_LOGW(TAG, "bad DNS %s", dotted);
        return;
    }
    esp_err_t err = esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set DNS %s failed: %s", dotted, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DNS set to %s", dotted);
    }
}

static esp_err_t start_tunnel(const char *peer_pubkey)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg = (wireguard_config_t)ESP_WIREGUARD_CONFIG_DEFAULT();
    s_cfg.private_key = CONFIG_MUSE_WG_PRIVATE_KEY;
    s_cfg.public_key = peer_pubkey;
    s_cfg.address = CONFIG_MUSE_WG_ADDRESS;
    s_cfg.netmask = CONFIG_MUSE_WG_NETMASK;
    s_cfg.endpoint = CONFIG_MUSE_WG_ENDPOINT;
    s_cfg.port = (uint16_t)CONFIG_MUSE_WG_ENDPOINT_PORT;
    s_cfg.persistent_keepalive = (uint16_t)CONFIG_MUSE_WG_KEEPALIVE;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx = (wireguard_ctx_t)ESP_WIREGUARD_CONTEXT_DEFAULT();

    esp_err_t err = esp_wireguard_init(&s_cfg, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_init: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;

    for (int attempt = 0; attempt < 30; attempt++) {
        err = esp_wireguard_connect(&s_ctx);
        if (err == ESP_OK) break;
        if (err == ESP_ERR_RETRY) {
            ESP_LOGI(TAG, "endpoint DNS still resolving, retry %d", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        ESP_LOGE(TAG, "esp_wireguard_connect: %s", esp_err_to_name(err));
        esp_wireguard_disconnect(&s_ctx);
        s_inited = false;
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "endpoint DNS never resolved");
        esp_wireguard_disconnect(&s_ctx);
        s_inited = false;
        return err;
    }

    for (int i = 0; i < 40; i++) {
        if (esp_wireguard_peer_is_up(&s_ctx) == ESP_OK) {
            ESP_LOGI(TAG, "peer up after %d ms", i * 250);
            /* Need MAX_SRC_IPS>=2: slot0 is the tunnel address itself. */
            err = esp_wireguard_add_allowed_ip(&s_ctx, "0.0.0.0", "0.0.0.0");
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "add_allowed_ip 0.0.0.0/0: %s "
                         "(raise CONFIG_WIREGUARD_MAX_SRC_IPS)",
                         esp_err_to_name(err));
                return err;
            }
            err = esp_wireguard_set_default(&s_ctx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_default: %s", esp_err_to_name(err));
                return err;
            }
            set_dns_v4(CONFIG_MUSE_WG_DNS);
            /* Backup DNS on STA netif (queries go via default=WG). */
            {
                esp_netif_t *sta = wifi_mgr_get_netif();
                if (sta) {
                    esp_netif_dns_info_t dns2 = {0};
                    dns2.ip.type = ESP_IPADDR_TYPE_V4;
                    if (esp_netif_str_to_ip4("1.0.0.1", &dns2.ip.u_addr.ip4) == ESP_OK) {
                        (void)esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns2);
                    }
                }
            }
            s_up = true;
            ESP_LOGI(TAG, "tunnel default route active -> %s:%d",
                     CONFIG_MUSE_WG_ENDPOINT, CONFIG_MUSE_WG_ENDPOINT_PORT);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGW(TAG, "handshake timed out (peer still down)");
    esp_wireguard_disconnect(&s_ctx);
    s_inited = false;
    return ESP_ERR_TIMEOUT;
}

static void stop_tunnel(void)
{
    if (!s_inited) return;
    if (s_up) {
        (void)esp_wireguard_restore_default(&s_ctx);
        s_up = false;
    }
    (void)esp_wireguard_disconnect(&s_ctx);
    s_inited = false;
    ESP_LOGI(TAG, "tunnel stopped");
}

static void wg_task(void *arg)
{
    (void)arg;
    sntp_sync();

    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "Wi-Fi gone before tunnel start");
        s_task_running = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const char *keys[2] = {
        CONFIG_MUSE_WG_PEER_PUBLIC_KEY,
        CONFIG_MUSE_WG_PEER_PUBLIC_KEY_ALT,
    };

    esp_err_t err = ESP_FAIL;
    for (int k = 0; k < 2; k++) {
        if (!keys[k] || !keys[k][0]) continue;
        ESP_LOGI(TAG, "trying peer pubkey #%d (len=%u)", k + 1,
                 (unsigned)strlen(keys[k]));
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) break;
        stop_tunnel();
        err = start_tunnel(keys[k]);
        xSemaphoreGive(s_lock);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "pubkey #%d failed: %s", k + 1, esp_err_to_name(err));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WireGuard not up; Muse cloud may stay unreachable");
    }

    s_task_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

void muse_wg_notify_wifi_up(void)
{
    if (CONFIG_MUSE_WG_PRIVATE_KEY[0] == '\0'
        || CONFIG_MUSE_WG_PEER_PUBLIC_KEY[0] == '\0'
        || CONFIG_MUSE_WG_ENDPOINT[0] == '\0') {
        ESP_LOGW(TAG, "enabled but keys/endpoint empty — skip");
        return;
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_task_running) return;
    s_task_running = true;
    if (xTaskCreate(wg_task, "muse_wg", 8192, NULL, 5, &s_task)
        != pdPASS) {
        s_task_running = false;
        ESP_LOGE(TAG, "failed to start muse_wg task");
    } else {
        ESP_LOGI(TAG, "Wi-Fi up; starting WireGuard");
    }
}

void muse_wg_notify_wifi_down(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        stop_tunnel();
        xSemaphoreGive(s_lock);
    } else {
        stop_tunnel();
    }
}

bool muse_wg_is_up(void)
{
    return s_up && s_inited
        && esp_wireguard_peer_is_up(&s_ctx) == ESP_OK;
}

#else /* !CONFIG_MUSE_WG_ENABLE */

void muse_wg_notify_wifi_up(void) {}
void muse_wg_notify_wifi_down(void) {}
bool muse_wg_is_up(void) { return false; }

#endif /* CONFIG_MUSE_WG_ENABLE */
