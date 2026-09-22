/* SUMMON Passport application. USB media is separate from Hub control frames. */
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "provisioning.h"
#include "ui_font.h"
#include "lvgl.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static QueueHandle_t keys;
static SemaphoreHandle_t tx_lock;
static lv_obj_t *status_label, *plate_label, *battery_label;
static volatile bool recording, cancelled, configuring, playing;
static volatile unsigned turn;
static bool audio_ready;
typedef struct { bsp_btn_t key; bsp_btn_ev_t event; } key_event;

static void send_json(cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    if (s) {
        xSemaphoreTake(tx_lock, portMAX_DELAY);
        printf("\nSUMMON1 %s\n", s); fflush(stdout);
        xSemaphoreGive(tx_lock); free(s);
    }
    cJSON_Delete(j);
}
static cJSON *message(const char *type, unsigned id) {
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"type",type);
    cJSON_AddNumberToObject(j,"turn",id);
    return j;
}
static void status(const char *s) {
    if (bsp_lvgl_lock(500)) {
        lv_label_set_text(status_label,s);
        lv_label_set_text_fmt(plate_label,"%s",provisioning_nameplate()[0] ? provisioning_nameplate() : "手机配置 Agent 铭牌");
        int soc=bsp_battery_soc();
        if(soc >= 0) lv_label_set_text_fmt(battery_label,"%d%%",soc);
        else lv_label_set_text(battery_label,"--");
        bsp_lvgl_unlock();
    }
}
static void hello(void) {
    cJSON *j=message("hello",turn);
    cJSON_AddStringToObject(j,"firmware","summon-usb-voice-1");
    cJSON_AddStringToObject(j,"nameplate",provisioning_nameplate());
    cJSON_AddNumberToObject(j,"sample_rate",16000);
    cJSON_AddBoolToObject(j,"audio_ready",audio_ready);
    send_json(j);
}
static void record_task(void *unused) {
    (void)unused;
    int16_t pcm[512]; unsigned char encoded[1400];
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        unsigned id=turn; bool ok=true; unsigned count=0;
        cJSON *j=message("record.start",id);
        cJSON_AddStringToObject(j,"nameplate",provisioning_nameplate()); send_json(j);
        status("正在聆听 5 秒\n请说出你的需求\n双击确定取消");
        for(unsigned n=0;n<157 && !cancelled;n++) {
            size_t len=n==156 ? 256 : sizeof(pcm), out=0;
            if(bsp_audio_read(pcm,len)!=ESP_OK) {ok=false;break;}
            mbedtls_base64_encode(encoded,sizeof(encoded),&out,(unsigned char*)pcm,len);
            encoded[out]=0;
            j=message("record.chunk",id);
            cJSON_AddNumberToObject(j,"seq",n);
            cJSON_AddStringToObject(j,"pcm",(char*)encoded); send_json(j); count+=(unsigned)len;
        }
        j=message(ok && !cancelled ? "record.end" : "record.cancel",id);
        cJSON_AddNumberToObject(j,"bytes",count); send_json(j);
        recording=false;
        status(cancelled ? "已取消\n确定重新说话" : ok ? "正在识别与请求 Agent…" : "采音失败\n请重新连接设备");
    }
}
static TaskHandle_t recorder;
static void on_key(bsp_btn_t key,bsp_btn_ev_t event,void *arg) {
    (void)arg; key_event e={key,event}; (void)xQueueSend(keys,&e,0);
}
static void begin_record(void) {
    if(recording || playing || configuring || !audio_ready) return;
    cancelled=false;turn++;recording=true;xTaskNotifyGive(recorder);
}
static void key_task(void *unused) {
    (void)unused; key_event e;
    for (;;) {
        if(xQueueReceive(keys,&e,portMAX_DELAY)!=pdTRUE) continue;
        if(e.key==BSP_BTN_OK && (e.event==BSP_BTN_DOUBLE || e.event==BSP_BTN_LONG)) {
            cancelled=true;playing=false;send_json(message("cancel",turn));
            if(configuring) {provisioning_stop();configuring=false;}
            status("已返回\n确定说话 · 上键配网");
        } else if(e.event==BSP_BTN_CLICK && e.key==BSP_BTN_OK) begin_record();
        else if(e.event==BSP_BTN_CLICK && e.key==BSP_BTN_UP && !recording && !playing) {
            configuring=provisioning_start()==ESP_OK;
            char text[180];
            snprintf(text,sizeof(text),"手机连接热点\n%s\n密码 %s\n浏览器 192.168.4.1\n双击确定返回",provisioning_ap_ssid(),provisioning_ap_password());
            status(configuring ? text : "配网启动失败");
        }
    }
}
static void receive(const char *line) {
    cJSON *j=cJSON_Parse(line); if(!j) return;
    cJSON *op=cJSON_GetObjectItem(j,"type"), *id=cJSON_GetObjectItem(j,"turn");
    if(!cJSON_IsString(op)) {cJSON_Delete(j);return;}
    if(strcmp(op->valuestring,"hello")==0) hello();
    else if(strcmp(op->valuestring,"record")==0) begin_record();
    else if(cJSON_IsNumber(id) && (unsigned)id->valuedouble==turn && !cancelled) {
        if(strcmp(op->valuestring,"status")==0) {
            cJSON *s=cJSON_GetObjectItem(j,"text");
            if(cJSON_IsString(s) && strlen(s->valuestring)<900) status(s->valuestring);
        } else if(strcmp(op->valuestring,"play.chunk")==0 && !recording && !configuring && audio_ready) {
            cJSON *s=cJSON_GetObjectItem(j,"pcm"), *seq=cJSON_GetObjectItem(j,"seq");
            unsigned char pcm[1024];size_t len=0;
            if(cJSON_IsString(s) && cJSON_IsNumber(seq) && strlen(s->valuestring)<=1368 &&
                mbedtls_base64_decode(pcm,sizeof(pcm),&len,(unsigned char*)s->valuestring,strlen(s->valuestring))==0 && len%2==0) {
                playing=true;
                esp_err_t err=bsp_audio_write(pcm,len);
                cJSON *ack=message(err==ESP_OK ? "play.ack" : "play.error",turn);
                cJSON_AddNumberToObject(ack,"seq",seq->valuedouble);send_json(ack);
            }
        } else if(strcmp(op->valuestring,"play.end")==0) {playing=false;send_json(message("play.done",turn));}
    }
    cJSON_Delete(j);
}
static void usb_task(void *unused) {
    (void)unused; char line[2048];size_t n=0;bool overflow=false;
    for(;;) {
        char c; int got=read(STDIN_FILENO,&c,1);
        if(got!=1) {vTaskDelay(pdMS_TO_TICKS(5));continue;}
        if(c=='\n') {
            line[n]=0;
            if(!overflow && strncmp(line,"SUMMON1 ",8)==0) receive(line+8);
            n=0;overflow=false;
        } else if(c!='\r') {
            if(n+1<sizeof(line)) line[n++]=c; else overflow=true;
        }
    }
}
void app_main(void) {
    tx_lock=xSemaphoreCreateMutex(); keys=xQueueCreate(8,sizeof(key_event));
    if(!tx_lock || !keys) return;
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_display_init()); if(!bsp_lvgl_init()) return;
    ui_font_init();bsp_display_backlight(80);bsp_battery_init();
    ESP_ERROR_CHECK(provisioning_init());
    audio_ready=bsp_audio_init()==ESP_OK && bsp_audio_set_format(16000,16,1)==ESP_OK;
    if(audio_ready) bsp_audio_set_volume(65);
    if(!bsp_lvgl_lock(1000)) return;
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x10182A),0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xE2ECFF),0);
    lv_obj_set_style_text_font(screen,ui_font_body(),0);
    lv_obj_t *title=lv_label_create(screen);lv_label_set_text(title,"SUMMON / 唤名");lv_obj_align(title,LV_ALIGN_TOP_LEFT,14,18);
    battery_label=lv_label_create(screen);lv_obj_align(battery_label,LV_ALIGN_TOP_RIGHT,-12,44);
    plate_label=lv_label_create(screen);lv_obj_set_width(plate_label,216);lv_obj_align(plate_label,LV_ALIGN_TOP_LEFT,12,75);
    lv_obj_set_style_text_color(plate_label,lv_color_hex(0x6DE9D4),0);
    status_label=lv_label_create(screen);lv_obj_set_width(status_label,212);lv_obj_align(status_label,LV_ALIGN_TOP_LEFT,14,119);
    lv_label_set_long_mode(status_label,LV_LABEL_LONG_WRAP);
    lv_obj_t *footer=lv_label_create(screen);lv_label_set_text(footer,"确定说话 / 上键配置\n双击确定返回或取消");lv_obj_align(footer,LV_ALIGN_BOTTOM_LEFT,12,-15);
    lv_screen_load(screen);bsp_lvgl_unlock();
    status(audio_ready ? "USB 连接电脑网关\n等待你的第一句话" : "音频初始化失败");
    if(xTaskCreate(record_task,"voice_record",6144,NULL,4,&recorder)!=pdPASS) return;
    if(xTaskCreate(key_task,"voice_keys",4096,NULL,3,NULL)!=pdPASS) return;
    if(xTaskCreate(usb_task,"voice_usb",8192,NULL,3,NULL)!=pdPASS) return;
    ESP_ERROR_CHECK(bsp_button_init(on_key,NULL));hello();
    ESP_LOGI("summon","Voice ready; free heap=%lu",(unsigned long)esp_get_free_heap_size());
}
