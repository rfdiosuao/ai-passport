/* Network transport for the same bounded media frames used over USB. */
#include "summon_network.h"
#include "provisioning.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static esp_websocket_client_handle_t client;
static QueueHandle_t incoming,outgoing;
static summon_receive_fn receiver;
static volatile bool connected,enabled;
static char token[96],headers[160];
static char assembly[2048];
static size_t assembled;

bool summon_network_online(void) {return connected;}
bool summon_network_enabled(void) {return enabled;}
void summon_network_enable(void) {enabled=true;}
bool summon_network_set_token(const char *value) {
    size_t n=strlen(value);
    if(n<32 || n>=sizeof(token) || client) return false;
    for(size_t i=0;i<n;i++) if(!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-",value[i])) return false;
    nvs_handle_t h;
    if(nvs_open("summon",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_str(h,"device_token",value);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK) snprintf(token,sizeof(token),"%s",value);
    return err==ESP_OK;
}
bool summon_network_send(const char *json) {
    if(!connected || strlen(json)>=2048) return false;
    char *copy=strdup(json);
    if(!copy) return false;
    if(xQueueSend(outgoing,&copy,pdMS_TO_TICKS(100))!=pdTRUE) {free(copy);return false;}
    return true;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;
    if(id==WEBSOCKET_EVENT_CONNECTED) {connected=true;assembled=0;}
    else if(id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_ERROR) {connected=false;assembled=0;}
    else if(id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *e=data;
        if(e->op_code!=1 || e->payload_len<=0 || e->payload_len>=sizeof(assembly)) return;
        if(e->payload_offset==0) assembled=0;
        if(e->payload_offset!=assembled || assembled+e->data_len>=sizeof(assembly)) {assembled=0;return;}
        memcpy(assembly+assembled,e->data_ptr,e->data_len);assembled+=e->data_len;
        if(assembled==e->payload_len) {
            assembly[assembled]=0;char *copy=strdup(assembly);
            if(copy && xQueueSend(incoming,&copy,0)!=pdTRUE) free(copy);
            assembled=0;
        }
    }
}
static void rx_task(void *arg) {
    (void)arg;char *value;
    for(;;) if(xQueueReceive(incoming,&value,portMAX_DELAY)==pdTRUE) {if(connected) receiver(value);free(value);}
}
static void tx_task(void *arg) {
    (void)arg;char *value;
    for(;;) if(xQueueReceive(outgoing,&value,portMAX_DELAY)==pdTRUE) {
        if(connected) esp_websocket_client_send_text(client,value,strlen(value),pdMS_TO_TICKS(2000));
        free(value);
    }
}
static void network_task(void *arg) {
    (void)arg;bool clock_started=false;unsigned retry=0;
    for(;;) {
        if(enabled && provisioning_configured()) {
            if(provisioning_state()!=PROV_CONNECTED) {
                if(retry++%10==0) provisioning_connect_saved();
            } else if(token[0]) {
                if(!clock_started) {
                    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                    esp_sntp_setservername(0,"pool.ntp.org");esp_sntp_setservername(1,"time.cloudflare.com");
                    esp_sntp_init();clock_started=true;
                }
                if(time(NULL)>1700000000 && !client) {
                    snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",token);
                    esp_websocket_client_config_t cfg={
                        .uri="wss://summon.entermodetwo.com/v1/passport/connect",
                        .headers=headers,.crt_bundle_attach=esp_crt_bundle_attach,
                        .buffer_size=2048,.task_stack=6144,.reconnect_timeout_ms=5000,.network_timeout_ms=5000};
                    client=esp_websocket_client_init(&cfg);
                    if(client) {esp_websocket_register_events(client,WEBSOCKET_EVENT_ANY,event,NULL);esp_websocket_client_start(client);}
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void summon_network_init(summon_receive_fn receive) {
    receiver=receive;nvs_handle_t h;
    if(nvs_open("summon",NVS_READONLY,&h)==ESP_OK) {size_t n=sizeof(token);if(nvs_get_str(h,"device_token",token,&n)!=ESP_OK) token[0]=0;nvs_close(h);}
    enabled=provisioning_configured();incoming=xQueueCreate(12,sizeof(char*));outgoing=xQueueCreate(16,sizeof(char*));
    if(!incoming || !outgoing) return;
    xTaskCreate(rx_task,"cloud_rx",6144,NULL,3,NULL);
    xTaskCreate(tx_task,"cloud_tx",4096,NULL,4,NULL);
    xTaskCreate(network_task,"cloud_connect",4096,NULL,2,NULL);
}
