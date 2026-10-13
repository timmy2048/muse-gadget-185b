/*
 * On-device OTA check: fetch public GitHub Release latest.json, compare versions,
 * and hand a confirmed URL to Home Link's ota_start via muse_ota_apply.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_MUSE_OTA_MANIFEST_URL
#define MUSE_OTA_MANIFEST_URL_DEFAULT "https://github.com/timmy2048/muse-gadget-185b/releases/latest/download/latest.json"
#else
#define MUSE_OTA_MANIFEST_URL_DEFAULT CONFIG_MUSE_OTA_MANIFEST_URL
#endif

typedef enum {
    MUSE_OTA_CHECK_IDLE = 0,
    MUSE_OTA_CHECK_BUSY,
    MUSE_OTA_CHECK_LATEST,
    MUSE_OTA_CHECK_UPDATE,
    MUSE_OTA_CHECK_ERROR,
} muse_ota_check_state_t;

typedef struct {
    muse_ota_check_state_t state;
    char running[32];
    char remote[32];
    char url[192];
    char sha256[72];
    char notes[96];
    int64_t size;
    char error[64];
    bool boot_badge;   /* true once a newer remote was seen (boot or menu) */
} muse_ota_check_info_t;

/* Mirror of main/ota.h result for the Muse UI (implemented in muse_glue.c). */
typedef enum {
    MUSE_OTA_APPLIED = 0,
    MUSE_OTA_SKIPPED,
    MUSE_OTA_FAILED,
} muse_ota_apply_result_t;

typedef struct {
    muse_ota_apply_result_t result;
    const char *detail;
    const char *new_version;
    const char *running_version;
} muse_ota_apply_event_t;

typedef void (*muse_ota_apply_cb_t)(const muse_ota_apply_event_t *ev, void *user);
typedef void (*muse_ota_progress_cb_t)(int percent, void *user);

bool muse_ota_is_enabled(void);
void muse_ota_apply(const char *url, bool force, muse_ota_apply_cb_t cb, void *user);
void muse_ota_set_progress_cb(muse_ota_progress_cb_t cb, void *user);
int muse_ota_progress_percent(void);

/* Snapshot of the last check (any task). */
void muse_ota_check_get(muse_ota_check_info_t *out);

/* Start a background HTTP GET of the manifest. Safe to call from UI. */
void muse_ota_check_start(void);

/* Once after boot: wait for IP, then check once for the settings badge. */
void muse_ota_check_schedule_boot(void);

/* Semver-ish compare shared with the menu (true if cand > cur). */
bool muse_ota_version_is_newer(const char *cand, const char *cur);

#ifdef __cplusplus
}
#endif
