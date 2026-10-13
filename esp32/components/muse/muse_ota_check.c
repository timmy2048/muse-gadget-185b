/*
 * Fetch public GitHub Release latest.json (HTTPS over WireGuard) and compare
 * to esp_app_get_description()->version. Applying an update goes through
 * muse_ota_apply() (main/muse_glue.c → ota_start).
 */

#include "muse_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "muse_ota";

#define MANIFEST_MAX 2048
#define BOOT_DELAY_MS 20000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_ota_check_info_t s_info;
static volatile bool s_check_running;

static void set_running_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    strlcpy(s_info.running, d && d->version[0] ? d->version : "?", sizeof(s_info.running));
}

static void copy_info(muse_ota_check_info_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_info;
    portEXIT_CRITICAL(&s_lock);
}

void muse_ota_check_get(muse_ota_check_info_t *out)
{
    if (!out) {
        return;
    }
    copy_info(out);
    if (!out->running[0]) {
        const esp_app_desc_t *d = esp_app_get_description();
        strlcpy(out->running, d && d->version[0] ? d->version : "?", sizeof(out->running));
    }
}

bool muse_ota_version_is_newer(const char *cand, const char *cur)
{
    long a[3] = {0}, b[3] = {0};
    const char *s = cand;
    if (s && (*s == 'v' || *s == 'V')) {
        s++;
    }
    for (int i = 0; i < 3 && s && *s; i++) {
        char *end;
        a[i] = strtol(s, &end, 10);
        if (end == s) {
            break;
        }
        s = end;
        if (*s == '.') {
            s++;
        } else {
            break;
        }
    }
    s = cur;
    if (s && (*s == 'v' || *s == 'V')) {
        s++;
    }
    for (int i = 0; i < 3 && s && *s; i++) {
        char *end;
        b[i] = strtol(s, &end, 10);
        if (end == s) {
            break;
        }
        s = end;
        if (*s == '.') {
            s++;
        } else {
            break;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    if (cand && cur && strcmp(cand, cur) != 0) {
        return true;
    }
    return false;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !r || !r->buf) {
        return ESP_OK;
    }
    if (r->len + evt->data_len >= r->cap) {
        return ESP_OK;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static void parse_manifest(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        portENTER_CRITICAL(&s_lock);
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "manifest JSON", sizeof(s_info.error));
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    const cJSON *sha = cJSON_GetObjectItem(root, "sha256");
    const cJSON *notes = cJSON_GetObjectItem(root, "notes");
    const cJSON *size = cJSON_GetObjectItem(root, "size");

    char remote[32] = {0};
    char u[192] = {0};
    char hash[72] = {0};
    char note[96] = {0};
    int64_t sz = -1;

    if (cJSON_IsString(ver) && ver->valuestring) {
        strlcpy(remote, ver->valuestring, sizeof(remote));
    }
    if (cJSON_IsString(url) && url->valuestring) {
        strlcpy(u, url->valuestring, sizeof(u));
    }
    if (cJSON_IsString(sha) && sha->valuestring) {
        strlcpy(hash, sha->valuestring, sizeof(hash));
    }
    if (cJSON_IsString(notes) && notes->valuestring) {
        strlcpy(note, notes->valuestring, sizeof(note));
    }
    if (cJSON_IsNumber(size)) {
        sz = (int64_t)size->valuedouble;
    }
    cJSON_Delete(root);

    set_running_version();
    bool newer = remote[0] && u[0] && muse_ota_version_is_newer(remote, s_info.running);

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_info.remote, remote, sizeof(s_info.remote));
    strlcpy(s_info.url, u, sizeof(s_info.url));
    strlcpy(s_info.sha256, hash, sizeof(s_info.sha256));
    strlcpy(s_info.notes, note, sizeof(s_info.notes));
    s_info.size = sz;
    s_info.error[0] = '\0';
    if (!remote[0] || !u[0]) {
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "manifest缺字段", sizeof(s_info.error));
    } else if (newer) {
        s_info.state = MUSE_OTA_CHECK_UPDATE;
        s_info.boot_badge = true;
    } else {
        s_info.state = MUSE_OTA_CHECK_LATEST;
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "manifest remote=%s running=%s newer=%d url=%s", remote, s_info.running, (int)newer, u);
}

static void check_task(void *arg)
{
    (void)arg;
    const char *manifest_url =
#ifdef CONFIG_MUSE_OTA_MANIFEST_URL
        CONFIG_MUSE_OTA_MANIFEST_URL;
#else
        MUSE_OTA_MANIFEST_URL_DEFAULT;
#endif

    portENTER_CRITICAL(&s_lock);
    set_running_version();
    s_info.state = MUSE_OTA_CHECK_BUSY;
    s_info.error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);

    char *buf = calloc(1, MANIFEST_MAX);
    if (!buf) {
        portENTER_CRITICAL(&s_lock);
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "内存不足", sizeof(s_info.error));
        portEXIT_CRITICAL(&s_lock);
        s_check_running = false;
        vTaskDelete(NULL);
        return;
    }
    resp_t r = { .buf = buf, .len = 0, .cap = MANIFEST_MAX };

    esp_http_client_config_t cfg = {
        .url = manifest_url,
        .timeout_ms = 30000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .event_handler = on_http,
        .user_data = &r,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(buf);
        portENTER_CRITICAL(&s_lock);
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "HTTP初始化失败", sizeof(s_info.error));
        portEXIT_CRITICAL(&s_lock);
        s_check_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200 || r.len == 0) {
        ESP_LOGW(TAG, "manifest fetch err=%s status=%d len=%u", esp_err_to_name(err), status, (unsigned)r.len);
        portENTER_CRITICAL(&s_lock);
        s_info.state = MUSE_OTA_CHECK_ERROR;
        if (status == 0) {
            strlcpy(s_info.error, "网络不可达", sizeof(s_info.error));
        } else {
            snprintf(s_info.error, sizeof(s_info.error), "HTTP %d", status);
        }
        portEXIT_CRITICAL(&s_lock);
    } else {
        parse_manifest(buf, r.len);
    }
    free(buf);
    s_check_running = false;
    vTaskDelete(NULL);
}

void muse_ota_check_start(void)
{
    if (!muse_ota_is_enabled()) {
        portENTER_CRITICAL(&s_lock);
        set_running_version();
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "OTA未启用", sizeof(s_info.error));
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (s_check_running) {
        return;
    }
    s_check_running = true;
    if (xTaskCreate(check_task, "ota_chk", 6144, NULL, 4, NULL) != pdPASS) {
        s_check_running = false;
        portENTER_CRITICAL(&s_lock);
        s_info.state = MUSE_OTA_CHECK_ERROR;
        strlcpy(s_info.error, "无法启动检查", sizeof(s_info.error));
        portEXIT_CRITICAL(&s_lock);
    }
}

static void boot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
    muse_ota_check_start();
    vTaskDelete(NULL);
}

void muse_ota_check_schedule_boot(void)
{
    static bool once;
    if (once || !muse_ota_is_enabled()) {
        return;
    }
    once = true;
    xTaskCreate(boot_task, "ota_boot", 3072, NULL, 3, NULL);
}

/* Weak stubs: muse_glue.c provides the real ones when Link OTA is linked. */
__attribute__((weak)) bool muse_ota_is_enabled(void)
{
    return false;
}

__attribute__((weak)) void muse_ota_apply(const char *url, bool force, muse_ota_apply_cb_t cb, void *user)
{
    (void)url;
    (void)force;
    if (cb) {
        muse_ota_apply_event_t ev = {
            .result = MUSE_OTA_SKIPPED,
            .detail = "OTA glue missing",
            .new_version = NULL,
            .running_version = esp_app_get_description()->version,
        };
        cb(&ev, user);
    }
}
