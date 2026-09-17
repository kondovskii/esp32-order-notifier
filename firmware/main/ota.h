#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Called during the download so the UI can show progress.
// percent is -1 when there's no number to show yet.
typedef void (*ota_progress_cb_t)(int percent, const char *msg);

// Reads the manifest, and if it names a different version, downloads and installs it.
// On success the device reboots into the new firmware and this never returns.
// ESP_ERR_NOT_FOUND means "already up to date".
esp_err_t ota_check_and_apply(ota_progress_cb_t cb);

// True when the running firmware was just installed and hasn't proven itself yet.
bool ota_is_pending_verify(void);

// Confirms the new firmware works. Until this is called, a reboot rolls back.
void ota_mark_valid(void);

// Gives up on the new firmware: marks it bad and reboots into the previous version.
void ota_rollback_now(void);