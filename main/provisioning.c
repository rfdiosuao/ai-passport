// main/provisioning.c —— SoftAP 本地配网实现(固件 UX §1)。
//
// 只依赖 esp_wifi / esp_http_server / nvs_flash,不碰 LVGL。
// 热点名 SUMMON-xxxx、密码 8 位十六进制,均从芯片 MAC 派生,每台设备不同。
#include "provisioning.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

static const char *TAG = "prov";

#define NVS_NS "summon"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_NAME "nameplate"
#define AP_IP "192.168.4.1"
#define CONNECT_TIMEOUT_MS 20000

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_event_ready;
static bool s_wifi_init;
static bool s_ap_up;
static bool s_handlers_ready;
static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_evt;
static esp_event_handler_instance_t s_ip_evt;

static volatile provisioning_state_t s_state = PROV_IDLE;
static char s_ap_ssid[16];
static char s_ap_pass[16];
static char s_nameplate[64];
static char s_ssid[33];
static char s_pass[65];
static int s_volume=65;
static bool s_volume_dirty;

int provisioning_volume(void) { return s_volume; }
void provisioning_set_volume(int value) {
    if(value<0) value=0;
    if(value>100) value=100;
    if(value!=s_volume) {s_volume=value;s_volume_dirty=true;}
}
void provisioning_persist_volume(void) {
    if(!s_volume_dirty) return;
    nvs_handle_t h;
    if(nvs_open(NVS_NS,NVS_READWRITE,&h)!=ESP_OK) return;
    if(nvs_set_u8(h,"volume",(uint8_t)s_volume)==ESP_OK && nvs_commit(h)==ESP_OK) s_volume_dirty=false;
    nvs_close(h);
}

// ---------------------------------------------------------------- NVS 存取
static esp_err_t nvs_open_rw(nvs_handle_t *h)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // 只有空/损坏分区才擦除;正常情况下这是首启。
        ESP_LOGW(TAG, "NVS 需擦除重试: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    return nvs_open(NVS_NS, NVS_READWRITE, h);
}

static void nvs_read_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    if (nvs_open_rw(&h) != ESP_OK) return;
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
    nvs_close(h);
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_rw(&h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------- 凭据派生
static void derive_ap_credentials(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SUMMON-%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------- Wi-Fi 事件
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == PROV_CONNECTING || s_state == PROV_CONNECTED) {
            ESP_LOGW(TAG, "目标网络断开");
            if (s_state == PROV_CONNECTING) s_state = PROV_WIFI_FAILED;
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_state = PROV_CONNECTED;
    }
}

// ---------------------------------------------------------------- httpd 页面
static const char INDEX_PAGE[] =
"<!doctype html><html lang=zh><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SUMMON 配网</title><style>"
"body{font-family:system-ui,sans-serif;background:#0b0c0a;color:#e7e9e4;margin:0;padding:16px}"
".card{max-width:420px;margin:0 auto;background:#171a1d;border-radius:14px;padding:18px}"
"h1{font-size:18px;margin:0 0 12px}label{display:block;font-size:13px;margin:14px 0 4px;color:#9aa4ad}"
"input,select{box-sizing:border-box;width:100%;padding:10px;border:1px solid #2c3338;border-radius:8px;background:#0f1113;color:#e7e9e4;font-size:15px}"
".row{display:flex;gap:8px}.row input{flex:1}button{background:#1689e8;color:#fff;border:0;border-radius:8px;padding:10px 14px;font-size:14px}"
"#status{margin-top:14px;font-size:14px;min-height:20px}#status.ok{color:#82be2d}#status.err{color:#e43b2f}"
"</style><div class=card><h1>SUMMON 设备配网</h1>"
"<p>语音通过 USB 连接电脑。只设置铭牌时，可不填写 Wi-Fi。</p>"
"<label>目标 Wi-Fi 名称 (SSID，可选)</label>"
"<div class=row><input id=ssid placeholder='手动输入或扫描'><button id=scan type=button onclick='doScan()'>扫描</button></div>"
"<label>密码</label><div class=row><input id=pass type=password placeholder='Wi-Fi 密码'><button type=button onclick='togglePw(this)'>显示</button></div>"
"<label>Agent 铭牌</label><input id=nameplate placeholder='SMN-XXXX-XXXX'>"
"<label>播放音量 <span id=volumeLabel>65</span>%</label><input id=volume type=range min=0 max=100 value=65 oninput='document.getElementById(\"volumeLabel\").textContent=this.value' onchange='setVolume(this.value)'>"
"<button type=button onclick='doSave(this)' style='width:100%;margin-top:18px'>保存并连接</button>"
"<div id=status></div></div>"
"<script>"
"function setStatus(t,ok){var s=document.getElementById('status');s.textContent=t;s.className=ok?'ok':'err'}\n"
"function setVolume(v){fetch('/volume',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'value='+v}).then(function(r){if(!r.ok)throw Error();setStatus('音量已调整',true)}).catch(function(){setStatus('音量调整失败')})}\n"
"fetch('/status').then(function(r){return r.json()}).then(function(j){document.getElementById('nameplate').value=j.nameplate;document.getElementById('volume').value=j.volume;document.getElementById('volumeLabel').textContent=j.volume}).catch(function(){setStatus('状态读取失败')});\n"
"function togglePw(btn){var p=document.getElementById('pass');p.type=(p.type==='password')?'text':'password';btn.textContent=(p.type==='password')?'显示':'隐藏'}\n"
"function doScan(){var b=document.getElementById('scan');b.disabled=true;setStatus('扫描中…');fetch('/scan').then(function(r){return r.json()}).then(function(a){if(!a.length){setStatus('未发现热点');return}var d=document.createElement('select');a.forEach(function(x){var o=document.createElement('option');o.textContent=x.ssid+' ('+x.rssi+'dBm)';o.value=x.ssid;d.appendChild(o)});var s=document.getElementById('ssid');s.parentNode.insertBefore(d,s.nextSibling);d.onchange=function(){s.value=this.value;this.remove()};setStatus('选一个热点')}).catch(function(){setStatus('扫描失败')}).finally(function(){b.disabled=false})}\n"
"function doSave(btn){btn.disabled=true;setStatus('保存中…');var body=new URLSearchParams();body.set('ssid',document.getElementById('ssid').value);body.set('pass',document.getElementById('pass').value);body.set('nameplate',document.getElementById('nameplate').value);fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json()}).then(function(j){setStatus(j.ok?('已连接: '+j.ip):('失败: '+j.message),j.ok)}).catch(function(){setStatus('请求失败')}).finally(function(){btn.disabled=false})}\n"
"</script>";

static esp_err_t http_get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_scan(httpd_req_t *req)
{
    wifi_ap_record_t aps[16];
    uint16_t count = 16;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, aps);

    cJSON *list=cJSON_CreateArray();
    if(err!=ESP_OK) count=0;
    for (uint16_t i = 0; i < count; i++) {
        char ssid[34] = { 0 };
        memcpy(ssid, aps[i].ssid, sizeof(aps[i].ssid) < 33 ? sizeof(aps[i].ssid) : 32);
        cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"ssid",ssid);
        cJSON_AddNumberToObject(item,"rssi",aps[i].rssi);cJSON_AddItemToArray(list,item);
    }
    char *body=cJSON_PrintUnformatted(list);cJSON_Delete(list);
    if(!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    err=httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);free(body);return err;
}

static esp_err_t http_get_status(httpd_req_t *req)
{
    cJSON *j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"configured",provisioning_configured());
    cJSON_AddStringToObject(j,"state",s_state==PROV_CONNECTED ? "connected" : "provisioning");
    cJSON_AddStringToObject(j,"ssid",s_ssid);cJSON_AddStringToObject(j,"nameplate",s_nameplate);
    cJSON_AddNumberToObject(j,"volume",s_volume);
    char *body=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err=httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);free(body);return err;
}

static esp_err_t http_post_volume(httpd_req_t *req) {
    char body[20]={0};
    if(req->content_len<7 || req->content_len>=sizeof(body)) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"invalid volume");
    int got=httpd_req_recv(req,body,req->content_len);
    if(got!=req->content_len || strncmp(body,"value=",6)) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"invalid volume");
    char *end;long value=strtol(body+6,&end,10);
    if(*end || value<0 || value>100) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"invalid volume");
    provisioning_set_volume((int)value);
    httpd_resp_set_type(req,"application/json");return httpd_resp_send(req,"{\"ok\":true}",HTTPD_RESP_USE_STRLEN);
}

static esp_err_t connect_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;

    s_state = PROV_CONNECTING;
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;

    // 轮询等待结果(配网页是短生命周期操作,阻塞可接受)。
    for (int i = 0; i < CONNECT_TIMEOUT_MS / 100; i++) {
        if (s_state == PROV_CONNECTED) return ESP_OK;
        if (s_state == PROV_WIFI_FAILED) return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

// 把 URL 编码表单值解码进 dst(+ -> 空格,%XX -> 字节)。
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dst_size; p++) {
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            unsigned v = 0;
            for (int k = 1; k <= 2; k++) {
                char c = p[k];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else { v = 0; break; }
            }
            dst[o++] = (char)(v & 0xFF);
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    char buf[400] = { 0 };
    if(req->content_len<=0 || req->content_len>=sizeof(buf)) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"invalid body length");
    int len=0;
    while(len<req->content_len) {
        int n=httpd_req_recv(req,buf+len,req->content_len-len);
        if(n<=0) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"incomplete body");
        len+=n;
    }
    buf[len] = '\0';

    char raw_ssid[128] = { 0 }, raw_pass[196] = { 0 }, raw_name[128] = { 0 };
    char ssid[33] = { 0 }, pass[65] = { 0 }, nameplate[64] = { 0 };
    char *save = NULL;
    for (char *tok = strtok_r(buf, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        if (strcmp(tok, "ssid") == 0) snprintf(raw_ssid, sizeof(raw_ssid), "%s", val);
        else if (strcmp(tok, "pass") == 0) snprintf(raw_pass, sizeof(raw_pass), "%s", val);
        else if (strcmp(tok, "nameplate") == 0) snprintf(raw_name, sizeof(raw_name), "%s", val);
    }
    url_decode(raw_ssid, ssid, sizeof(ssid));
    url_decode(raw_pass, pass, sizeof(pass));
    url_decode(raw_name, nameplate, sizeof(nameplate));
    if (ssid[0] == '\0' && nameplate[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"message\":\"请填写 Wi-Fi 或 Agent 铭牌\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (nameplate[0]) {
        bool valid = strlen(nameplate) == 13 && strncmp(nameplate,"SMN-",4)==0 && nameplate[8]=='-';
        for(size_t i=4; valid && i<13; i++) {
            if(i==8) continue;
            if(!strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ",nameplate[i])) valid=false;
        }
        if(!valid) {
            httpd_resp_set_type(req,"application/json");
            return httpd_resp_send(req,"{\"ok\":false,\"message\":\"铭牌格式为 SMN-XXXX-XXXX\"}",HTTPD_RESP_USE_STRLEN);
        }
    }
    if (ssid[0] == '\0') {
        esp_err_t saved=nvs_write_str(NVS_KEY_NAME,nameplate);
        if(saved==ESP_OK) snprintf(s_nameplate,sizeof(s_nameplate),"%s",nameplate);
        httpd_resp_set_type(req,"application/json");
        return httpd_resp_send(req,saved==ESP_OK ? "{\"ok\":true,\"ip\":\"铭牌已保存，双击设备确定键返回\"}" : "{\"ok\":false,\"message\":\"保存失败\"}",HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = connect_sta(ssid, pass);
    char body[320];
    if (err == ESP_OK) {
        // 验证成功才原子提交,坏配置不覆盖旧值。
        nvs_write_str(NVS_KEY_SSID, ssid);
        nvs_write_str(NVS_KEY_PASS, pass);
        if (nameplate[0]) nvs_write_str(NVS_KEY_NAME, nameplate);
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
        strncpy(s_pass, pass, sizeof(s_pass) - 1);
        strncpy(s_nameplate, nameplate, sizeof(s_nameplate) - 1);
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_sta_netif, &ip);
        char ipstr[16];
        esp_ip4addr_ntoa(&ip.ip, ipstr, sizeof(ipstr));
        snprintf(body, sizeof(body), "{\"ok\":true,\"ip\":\"%s\"}", ipstr);
    } else {
        snprintf(body, sizeof(body), "{\"ok\":false,\"message\":\"%s\"}",
                 err == ESP_ERR_TIMEOUT ? "连接超时,请检查密码" : "连接失败");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) return ESP_FAIL;

    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.uri = "/"; u.method = HTTP_GET; u.handler = http_get_index;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/scan"; u.handler = http_get_scan;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/status"; u.handler = http_get_status;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/save"; u.method = HTTP_POST; u.handler = http_post_save;
    httpd_register_uri_handler(s_server, &u);
    u.uri = "/volume"; u.handler = http_post_volume;
    httpd_register_uri_handler(s_server, &u);
    return ESP_OK;
}

// ---------------------------------------------------------------- 公共接口
esp_err_t provisioning_init(void)
{
    derive_ap_credentials();
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 擦除后重试");
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    nvs_read_str(NVS_KEY_SSID, s_ssid, sizeof(s_ssid));
    nvs_read_str(NVS_KEY_PASS, s_pass, sizeof(s_pass));
    nvs_read_str(NVS_KEY_NAME, s_nameplate, sizeof(s_nameplate));
    nvs_handle_t h;
    if(nvs_open(NVS_NS,NVS_READONLY,&h)==ESP_OK) {
        uint8_t volume;
        if(nvs_get_u8(h,"volume",&volume)==ESP_OK && volume<=100) s_volume=volume;
        nvs_close(h);
    }
    return ESP_OK;
}

bool provisioning_configured(void)
{
    return s_ssid[0] != '\0';
}

esp_err_t provisioning_start(void)
{
    if (s_ap_up) return ESP_OK;
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;  // 已初始化算成功
        s_netif_ready = true;
    }
    if (!s_event_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_event_ready = true;
    }
    if (!s_wifi_init) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) return ESP_FAIL;
        s_wifi_init = true;
    }
    if(!s_handlers_ready) {
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, &s_wifi_evt);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        ip_event_handler, NULL, &s_ip_evt);
    s_handlers_ready=true;
    }

    if(!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if(!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password) - 1);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.channel = 1;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    ip.gw.addr = esp_ip4addr_aton(AP_IP);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);

    if (esp_wifi_start() != ESP_OK) return ESP_FAIL;
    s_ap_up = true;
    s_state = PROV_AP_READY;
    start_httpd();
    ESP_LOGI(TAG, "配网热点已就绪，凭据仅在设备屏幕显示");
    return ESP_OK;
}

esp_err_t provisioning_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    if (s_wifi_init) { esp_wifi_stop(); }
    s_ap_up = false;
    s_state = PROV_IDLE;
    return ESP_OK;
}

provisioning_state_t provisioning_state(void) { return s_state; }
const char *provisioning_ap_ssid(void) { return s_ap_ssid; }
const char *provisioning_ap_password(void) { return s_ap_pass; }
const char *provisioning_nameplate(void) { return s_nameplate; }
