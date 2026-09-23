/* Network transport for the same bounded media frames used over USB. */
#include "summon_network.h"
#include "provisioning.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static esp_websocket_client_handle_t client;
static QueueHandle_t incoming,outgoing;
static SemaphoreHandle_t client_lock;
static summon_receive_fn receiver;
static volatile bool connected,enabled;
static char token[96],headers[160];
static char assembly[2048];
static size_t assembled;
static volatile unsigned generation;
static const char *TAG="summon_net";
typedef struct {char *text;unsigned generation;} queued_frame;

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
    queued_frame frame={.text=strdup(json),.generation=generation};
    if(!frame.text) return false;
    /* Audio frames arrive every 32 ms. A deep queue retains many 1.4 KB
     * JSON strings while mbedTLS needs a contiguous allocation for writes.
     * Apply backpressure to the recorder instead of accumulating frames. */
    if(xQueueSend(outgoing,&frame,pdMS_TO_TICKS(1000))!=pdTRUE) {
        ESP_LOGE(TAG,"outbound frame queue stalled");free(frame.text);return false;
    }
    return true;
}
static void event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;(void)base;
    if(id==WEBSOCKET_EVENT_CONNECTED) {connected=true;assembled=0;}
    else if(id==WEBSOCKET_EVENT_DISCONNECTED || id==WEBSOCKET_EVENT_ERROR) {connected=false;assembled=0;generation++;}
    else if(id==WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *e=data;
        if(e->op_code!=1 || e->payload_len<=0 || e->payload_len>=sizeof(assembly)) return;
        if(e->payload_offset==0) assembled=0;
        if(e->payload_offset!=assembled || assembled+e->data_len>=sizeof(assembly)) {assembled=0;return;}
        memcpy(assembly+assembled,e->data_ptr,e->data_len);assembled+=e->data_len;
        if(assembled==e->payload_len) {
            if(strstr(assembly,"remote.begin")) ESP_LOGI(TAG,"received remote.begin frame (%u bytes)",(unsigned)assembled);
            assembly[assembled]=0;queued_frame frame={.text=strdup(assembly),.generation=generation};
            if(!frame.text) ESP_LOGE(TAG,"inbound frame allocation failed");
            else if(xQueueSend(incoming,&frame,0)!=pdTRUE) {
                ESP_LOGE(TAG,"inbound frame queue full; dropping frame");free(frame.text);
            }
            assembled=0;
        }
    }
}
static void rx_task(void *arg) {
    (void)arg;queued_frame frame;
    for(;;) if(xQueueReceive(incoming,&frame,portMAX_DELAY)==pdTRUE) {
        /* Once a complete websocket frame is queued, a momentary transport
         * state transition must not discard it. The voice state machine owns
         * deciding whether to accept/reject it; generation is used only to
         * suppress frames from a connection that was already replaced. */
        if(frame.generation==generation) receiver(frame.text);
        else ESP_LOGW(TAG,"discarding stale inbound frame (queued generation %u, current %u)",
                      frame.generation,generation);
        free(frame.text);
    }
}
static void tx_task(void *arg) {
    (void)arg;queued_frame frame;
    for(;;) if(xQueueReceive(outgoing,&frame,portMAX_DELAY)==pdTRUE) {
        xSemaphoreTake(client_lock,portMAX_DELAY);
        if(client && connected && frame.generation==generation) esp_websocket_client_send_text(client,frame.text,strlen(frame.text),pdMS_TO_TICKS(2000));
        xSemaphoreGive(client_lock);
        free(frame.text);
    }
}
static void network_task(void *arg) {
    (void)arg;bool clock_started=false;unsigned retry=0,last_generation=0;
    for(;;) {
        if(last_generation!=generation) {last_generation=generation;receiver("{\"type\":\"network.lost\"}");}
        if(provisioning_active() && client) {
            xSemaphoreTake(client_lock,portMAX_DELAY);
            connected=false;generation++;
            esp_websocket_client_stop(client);esp_websocket_client_destroy(client);client=NULL;connected=false;
            xSemaphoreGive(client_lock);
        }
        if(enabled && provisioning_configured() && !provisioning_active()) {
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
    enabled=provisioning_configured();incoming=xQueueCreate(12,sizeof(queued_frame));outgoing=xQueueCreate(2,sizeof(queued_frame));
    client_lock=xSemaphoreCreateMutex();
    if(!incoming || !outgoing || !client_lock) return;
    xTaskCreate(rx_task,"cloud_rx",6144,NULL,3,NULL);
    xTaskCreate(tx_task,"cloud_tx",4096,NULL,4,NULL);
    xTaskCreate(network_task,"cloud_connect",4096,NULL,2,NULL);
}
