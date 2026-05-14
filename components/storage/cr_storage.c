// Mounts the LittleFS partition labelled "storage" at CR_STORAGE_MOUNT.
// Apps that need to persist files (event logs, settings, etc.) just open
// files under that path — the chassis only owns the mount/unmount.

#include "cr_storage.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "storage";

#define PARTITION_LABEL "storage"

esp_err_t cr_storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CR_STORAGE_MOUNT,
        .partition_label = PARTITION_LABEL,
        // Auto-format on mount failure. Required so a freshly-flashed
        // device boots — there's no FS to mount until the first format.
        // Trade-off: a genuine corruption event (which IDF and littlefs
        // both log loudly at WARN) will also trigger a wipe. The chassis
        // accepts this in exchange for a working out-of-box experience;
        // apps with critical persisted data should ensure backups (the
        // chassis's /api/system/backup covers WiFi/admin state).
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: total=%u used=%u",
                 CR_STORAGE_MOUNT, (unsigned)total, (unsigned)used);
    }

    return ESP_OK;
}

esp_err_t cr_storage_fs_info(size_t *total, size_t *used)
{
    size_t t = 0, u = 0;
    esp_err_t err = esp_littlefs_info(PARTITION_LABEL, &t, &u);
    if (err != ESP_OK) return err;
    if (total) *total = t;
    if (used)  *used  = u;
    return ESP_OK;
}
