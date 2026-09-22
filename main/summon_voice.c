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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static QueueHandle_t keys;
typedef struct { unsigned turn; size_t length; unsigned char pcm[1024]; } playback_chunk;
static QueueHandle_t playback;
static volatile bool play_ended;
static unsigned play_sequence;
static SemaphoreHandle_t tx_lock;
static lv_obj_t *status_label, *plate_label, *battery_label, *volume_label;
static volatile bool recording, cancelled, configuring, playing, finish_recording;
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
    cJSON_AddStringToObject(j,"firmware","summon-usb-voice-2");
    cJSON_AddStringToObject(j,"nameplate",provisioning_nameplate());
    cJSON_AddNumberToObject(j,"sample_rate",16000);
    cJSON_AddBoolToObject(j,"audio_ready",audio_ready);
    cJSON_AddNumberToObject(j,"volume",provisioning_volume());
    cJSON_AddNumberToObject(j,"free_heap",esp_get_free_heap_size());
    send_json(j);
}
static void record_task(void *unused) {
    (void)unused;
    int16_t pcm[512]; unsigned char encoded[1400];
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        unsigned id=turn; bool ok=true,heard_voice=false; unsigned count=0,silent_chunks=0;
        cJSON *j=message("record.start",id);
        cJSON_AddStringToObject(j,"nameplate",provisioning_nameplate()); send_json(j);
        status("正在聆听，说完即发送\n再按确定立即结束\n双击确定取消");
        for(unsigned n=0;n<250 && !cancelled && !finish_recording;n++) {
            size_t len=sizeof(pcm), out=0;
            if(bsp_audio_read(pcm,len)!=ESP_OK) {ok=false;break;}
            unsigned energy=0;
            for(unsigned i=0;i<512;i++) energy+=(unsigned)abs((int)pcm[i]);
            if(energy/512>400) {heard_voice=true;silent_chunks=0;}
            else if(heard_voice) silent_chunks++;
            mbedtls_base64_encode(encoded,sizeof(encoded),&out,(unsigned char*)pcm,len);
            encoded[out]=0;
            j=message("record.chunk",id);
            cJSON_AddNumberToObject(j,"seq",n);
            cJSON_AddStringToObject(j,"pcm",(char*)encoded); send_json(j); count+=(unsigned)len;
            if(heard_voice && silent_chunks>=20 && n>=31) break;
        }
        j=message(ok && !cancelled ? "record.end" : "record.cancel",id);
        cJSON_AddNumberToObject(j,"bytes",count); send_json(j);
        recording=false;
        status(cancelled ? "已取消\n确定重新说话" : ok ? "正在识别与请求 Agent…" : "采音失败\n请重新连接设备");
    }
}
static TaskHandle_t recorder;
static void playback_task(void *unused) {
    (void)unused;playback_chunk chunk;
    for(;;) {
        while(!playing) vTaskDelay(pdMS_TO_TICKS(10));
        unsigned id=turn,underruns=0,bytes=0;
        // Prebuffer 384 ms; USB ACK means queued, not yet played.
        while(playing && !cancelled && !play_ended && uxQueueMessagesWaiting(playback)<12) vTaskDelay(pdMS_TO_TICKS(2));
        int64_t started=esp_timer_get_time();
        bool done=false;
        while(playing && !cancelled && id==turn) {
            if(xQueueReceive(playback,&chunk,pdMS_TO_TICKS(100))!=pdTRUE) {
                underruns++;
                if(underruns>=20) {playing=false;send_json(message("play.error",id));break;}
                continue;
            }
            if(chunk.turn!=id) continue;
            if(!chunk.length) {done=true;break;}
            if(bsp_audio_write(chunk.pcm,chunk.length)!=ESP_OK) {send_json(message("play.error",id));break;}
            bytes+=(unsigned)chunk.length;
        }
        playing=false;xQueueReset(playback);
        if(done && !cancelled) {
            cJSON *reply=message("play.done",id);
            cJSON_AddNumberToObject(reply,"bytes",bytes);
            cJSON_AddNumberToObject(reply,"underruns",underruns);
            cJSON_AddNumberToObject(reply,"elapsed_ms",(esp_timer_get_time()-started)/1000);
            send_json(reply);
        }
    }
}
static void on_key(bsp_btn_t key,bsp_btn_ev_t event,void *arg) {
    (void)arg; key_event e={key,event}; (void)xQueueSend(keys,&e,0);
}
static void begin_record(void) {
    if(recording || playing || configuring || !audio_ready) return;
    cancelled=false;finish_recording=false;turn++;recording=true;xTaskNotifyGive(recorder);
}
static void key_task(void *unused) {
    (void)unused; key_event e;int volume=-1;
    for (;;) {
        int next=provisioning_volume();
        if(volume!=next) {
            volume=next;if(audio_ready) bsp_audio_set_volume((uint8_t)volume);
            if(bsp_lvgl_lock(100)) {lv_label_set_text_fmt(volume_label,"音量 %d%%",volume);bsp_lvgl_unlock();}
        }
        if(!playing && !recording) provisioning_persist_volume();
        if(xQueueReceive(keys,&e,pdMS_TO_TICKS(100))!=pdTRUE) continue;
        if(e.key==BSP_BTN_OK && (e.event==BSP_BTN_DOUBLE || e.event==BSP_BTN_LONG)) {
            cancelled=true;playing=false;xQueueReset(playback);send_json(message("cancel",turn));
            if(configuring) {provisioning_stop();configuring=false;}
            status("已返回\n确定说话 · 长按上键配网");
        } else if(e.event==BSP_BTN_CLICK && e.key==BSP_BTN_OK) {
            if(recording) finish_recording=true; else begin_record();
        }
        else if(e.event==BSP_BTN_CLICK && (e.key==BSP_BTN_UP || e.key==BSP_BTN_DOWN)) {
            provisioning_set_volume(provisioning_volume()+(e.key==BSP_BTN_UP ? 10 : -10));
        }
        else if(e.event==BSP_BTN_LONG && e.key==BSP_BTN_UP && !recording && !playing) {
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
            playback_chunk chunk={.turn=turn};size_t len=0;
            if(cJSON_IsString(s) && cJSON_IsNumber(seq) && strlen(s->valuestring)<=1368 &&
                mbedtls_base64_decode(chunk.pcm,sizeof(chunk.pcm),&len,(unsigned char*)s->valuestring,strlen(s->valuestring))==0 && len>0 && len%2==0) {
                if(!playing) {play_sequence=0;play_ended=false;xQueueReset(playback);playing=true;}
                chunk.length=len;
                bool accepted=(unsigned)seq->valuedouble==play_sequence && xQueueSend(playback,&chunk,pdMS_TO_TICKS(500))==pdTRUE;
                if(accepted) play_sequence++;
                cJSON *ack=message(accepted ? "play.ack" : "play.error",turn);
                cJSON_AddNumberToObject(ack,"seq",seq->valuedouble);send_json(ack);
            }
        } else if(strcmp(op->valuestring,"play.end")==0 && playing) {
            playback_chunk end={.turn=turn,.length=0};play_ended=true;
            if(xQueueSend(playback,&end,pdMS_TO_TICKS(500))!=pdTRUE) send_json(message("play.error",turn));
        }
    }
    cJSON_Delete(j);
}
static void usb_task(void *unused) {
    (void)unused; char line[2048];size_t n=0;bool overflow=false;
    for(;;) {
        char block[128]; int got=read(STDIN_FILENO,block,sizeof(block));
        if(got<=0) {vTaskDelay(pdMS_TO_TICKS(1));continue;}
        for(int i=0;i<got;i++) {
        char c=block[i];if(c=='\n') {
            line[n]=0;
            if(!overflow && strncmp(line,"SUMMON1 ",8)==0) receive(line+8);
            n=0;overflow=false;
        } else if(c!='\r') {
            if(n+1<sizeof(line)) line[n++]=c; else overflow=true;
        }
        }
    }
}
void app_main(void) {
    tx_lock=xSemaphoreCreateMutex(); keys=xQueueCreate(8,sizeof(key_event));playback=xQueueCreate(16,sizeof(playback_chunk));
    if(!tx_lock || !keys || !playback) return;
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_display_init()); if(!bsp_lvgl_init()) return;
    ui_font_init();bsp_display_backlight(80);bsp_battery_init();
    ESP_ERROR_CHECK(provisioning_init());
    audio_ready=bsp_audio_init()==ESP_OK && bsp_audio_set_format(16000,16,1)==ESP_OK;
    if(audio_ready) bsp_audio_set_volume((uint8_t)provisioning_volume());
    if(!bsp_lvgl_lock(1000)) return;
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x10182A),0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xE2ECFF),0);
    lv_obj_set_style_text_font(screen,ui_font_body(),0);
    lv_obj_t *title=lv_label_create(screen);lv_label_set_text(title,"SUMMON / 唤名");lv_obj_align(title,LV_ALIGN_TOP_LEFT,14,18);
    battery_label=lv_label_create(screen);lv_obj_align(battery_label,LV_ALIGN_TOP_RIGHT,-12,44);
    volume_label=lv_label_create(screen);lv_obj_align(volume_label,LV_ALIGN_TOP_LEFT,12,44);
    plate_label=lv_label_create(screen);lv_obj_set_width(plate_label,216);lv_obj_align(plate_label,LV_ALIGN_TOP_LEFT,12,75);
    lv_obj_set_style_text_color(plate_label,lv_color_hex(0x6DE9D4),0);
    status_label=lv_label_create(screen);lv_obj_set_width(status_label,212);lv_obj_align(status_label,LV_ALIGN_TOP_LEFT,14,119);
    lv_label_set_long_mode(status_label,LV_LABEL_LONG_WRAP);
    lv_obj_t *footer=lv_label_create(screen);lv_label_set_text(footer,"上下音量 / 长按上键配网\n确定说话 / 双击确定返回");lv_obj_align(footer,LV_ALIGN_BOTTOM_LEFT,12,-15);
    lv_screen_load(screen);bsp_lvgl_unlock();
    status(audio_ready ? "USB 连接电脑网关\n等待你的第一句话" : "音频初始化失败");
    if(xTaskCreate(record_task,"voice_record",6144,NULL,4,&recorder)!=pdPASS) return;
    if(xTaskCreate(playback_task,"voice_play",4096,NULL,5,NULL)!=pdPASS) return;
    if(xTaskCreate(key_task,"voice_keys",4096,NULL,3,NULL)!=pdPASS) return;
    if(xTaskCreate(usb_task,"voice_usb",8192,NULL,3,NULL)!=pdPASS) return;
    ESP_ERROR_CHECK(bsp_button_init(on_key,NULL));hello();
    ESP_LOGI("summon","Voice ready; free heap=%lu",(unsigned long)esp_get_free_heap_size());
}
