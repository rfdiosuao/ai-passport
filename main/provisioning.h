#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum { PROV_IDLE=0, PROV_CONNECTING, PROV_CONNECTED, PROV_WIFI_FAILED } provisioning_state_t;
esp_err_t provisioning_init(void);
bool provisioning_configured(void);
esp_err_t provisioning_connect_saved(void);
provisioning_state_t provisioning_state(void);
const char *provisioning_nameplate(void);
int provisioning_volume(void);
void provisioning_set_volume(int value);
void provisioning_persist_volume(void);
// USB maintenance only: NULL keeps the saved field. Reboot after a successful update.
esp_err_t provisioning_save(const char *ssid, const char *password, const char *nameplate);
