#pragma once

void ota_manager_init(void);

/** Start a background OTA pull from the given firmware URL. */
void ota_manager_handle_trigger(const char *url);
