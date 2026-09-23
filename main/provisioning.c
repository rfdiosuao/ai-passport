/* Saved Wi-Fi STA and volume. No SoftAP, scan, or HTTP server. */
#include "provisioning.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG="summon_wifi";
static char s_ssid[33],s_password[65],s_nameplate[64];
static int s_volume=65;
static bool s_volume_dirty,s_wifi_started;
static int64_t s_connect_started;
static volatile provisioning_state_t s_state=PROV_IDLE;

static bool valid_nameplate(const char *v) {
    if(strlen(v)!=13 || strncmp(v,"SMN-",4) || v[8]!='-') return false;
    for(int i=4;i<13;i++) if(i!=8 && !strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ",v[i])) return false;
    return true;
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;(void)data;
    if(id==WIFI_EVENT_STA_DISCONNECTED && s_state!=PROV_IDLE) s_state=PROV_WIFI_FAILED;
}
static void ip_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;(void)data;
    if(id==IP_EVENT_STA_GOT_IP) s_state=PROV_CONNECTED;
}
esp_err_t provisioning_init(void) {
    esp_err_t err=nvs_flash_init();
    if(err!=ESP_OK) return err; // Never erase saved credentials on a transient NVS error.
    nvs_handle_t h;err=nvs_open("summon",NVS_READONLY,&h);
    if(err!=ESP_OK) return err==ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    size_t n=sizeof(s_ssid);
    if(nvs_get_str(h,"ssid",s_ssid,&n)!=ESP_OK) s_ssid[0]=0;
    n=sizeof(s_password);
    if(nvs_get_str(h,"pass",s_password,&n)!=ESP_OK) s_password[0]=0;
    n=sizeof(s_nameplate);
    if(nvs_get_str(h,"nameplate",s_nameplate,&n)!=ESP_OK) s_nameplate[0]=0;
    uint8_t volume;
    if(nvs_get_u8(h,"volume",&volume)==ESP_OK && volume<=100) s_volume=volume;
    nvs_close(h);return ESP_OK;
}
bool provisioning_configured(void) {return s_ssid[0]!=0;}
provisioning_state_t provisioning_state(void) {return s_state;}
const char *provisioning_nameplate(void) {return s_nameplate;}
int provisioning_volume(void) {return s_volume;}
void provisioning_set_volume(int v) {
    if(v<0)v=0;if(v>100)v=100;
    if(v!=s_volume){s_volume=v;s_volume_dirty=true;}
}
void provisioning_persist_volume(void) {
    if(!s_volume_dirty)return;
    nvs_handle_t h;if(nvs_open("summon",NVS_READWRITE,&h)!=ESP_OK)return;
    if(nvs_set_u8(h,"volume",(uint8_t)s_volume)==ESP_OK && nvs_commit(h)==ESP_OK)s_volume_dirty=false;
    nvs_close(h);
}
esp_err_t provisioning_save(const char *ssid,const char *password,const char *nameplate) {
    if(ssid && (!ssid[0] || strlen(ssid)>32 || strchr(ssid,'\n')))return ESP_ERR_INVALID_ARG;
    if(password && (strlen(password)>63 || (password[0] && strlen(password)<8)))return ESP_ERR_INVALID_ARG;
    if(nameplate && !valid_nameplate(nameplate))return ESP_ERR_INVALID_ARG;
    if(password && !ssid && !s_ssid[0])return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t err=nvs_open("summon",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    if(ssid)err=nvs_set_str(h,"ssid",ssid);
    if(err==ESP_OK && password)err=nvs_set_str(h,"pass",password);
    if(err==ESP_OK && nameplate)err=nvs_set_str(h,"nameplate",nameplate);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);return err;
}
esp_err_t provisioning_connect_saved(void) {
    if(!s_ssid[0])return ESP_ERR_INVALID_STATE;
    if(s_state==PROV_CONNECTED)return ESP_OK;
    if(s_state==PROV_CONNECTING) {
        if(esp_timer_get_time()-s_connect_started<20000000)return ESP_OK;
        s_state=PROV_WIFI_FAILED;
        esp_wifi_disconnect();
    }
    if(!s_wifi_started) {
        esp_err_t err=esp_netif_init();
        if(err!=ESP_OK && err!=ESP_ERR_INVALID_STATE)return err;
        err=esp_event_loop_create_default();
        if(err!=ESP_OK && err!=ESP_ERR_INVALID_STATE)return err;
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
        err=esp_wifi_init(&init);if(err!=ESP_OK)return err;
        err=esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,wifi_event,NULL);
        if(err!=ESP_OK)return err;
        err=esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,ip_event,NULL);
        if(err!=ESP_OK)return err;
        err=esp_wifi_set_mode(WIFI_MODE_STA);if(err!=ESP_OK)return err;
        wifi_config_t config={0};
        memcpy(config.sta.ssid,s_ssid,strlen(s_ssid));
        memcpy(config.sta.password,s_password,strlen(s_password));
        config.sta.threshold.authmode=s_password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
        err=esp_wifi_set_config(WIFI_IF_STA,&config);if(err!=ESP_OK)return err;
        err=esp_wifi_start();if(err!=ESP_OK)return err;
        err=esp_wifi_set_ps(WIFI_PS_NONE);
        if(err!=ESP_OK){ESP_LOGE(TAG,"Wi-Fi voice power-save setup failed: %s",esp_err_to_name(err));return err;}
        s_wifi_started=true;
    }
    s_state=PROV_CONNECTING;
    s_connect_started=esp_timer_get_time();
    esp_err_t err=esp_wifi_connect();
    if(err!=ESP_OK)s_state=PROV_WIFI_FAILED;
    return err;
}
