#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount LittleFS partition (label "storage") at CR_STORAGE_MOUNT.
esp_err_t cr_storage_init(void);

// Filesystem mount path. Apps open their files under this prefix using stdio.
#define CR_STORAGE_MOUNT  "/storage"

// Total / used bytes on the LittleFS partition. Either pointer may be NULL.
esp_err_t cr_storage_fs_info(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif
