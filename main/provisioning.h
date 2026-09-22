// main/provisioning.h —— SoftAP 本地配网(固件 UX §1)。
//
// 首次上电或用户主动进入时,设备开热点 + 本地网页,手机填写目标 Wi-Fi 与
// Agent 铭牌。网络配置与铭牌分两个 NVS 键保存,验证成功才原子提交。
// 本模块只管 Wi-Fi/httpd/存储,不碰 LVGL;界面在 demo_provisioning.c。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 配网状态(供界面展示)。
typedef enum {
    PROV_IDLE = 0,        // 未运行
    PROV_AP_READY,        // 热点就绪,等待手机配置
    PROV_CONNECTING,      // 正在连接目标 Wi-Fi
    PROV_CONNECTED,       // Wi-Fi 已连接
    PROV_WIFI_FAILED,     // Wi-Fi 连接失败(旧配置未破坏)
} provisioning_state_t;

// 初始化 NVS 并读取已存配置。应在无线服务前调用一次。
esp_err_t provisioning_init(void);

// 是否已有可用 Wi-Fi 配置(决定是否首启进配网)。
bool provisioning_configured(void);

// 打开 SoftAP + 本地网页。已打开时幂等返回。
esp_err_t provisioning_start(void);

// 关闭 SoftAP 与网页(不丢已存配置)。
esp_err_t provisioning_stop(void);

// 当前状态快照。
provisioning_state_t provisioning_state(void);

// 热点凭据与目标信息,供设备屏显示/日志。
const char *provisioning_ap_ssid(void);
const char *provisioning_ap_password(void);

// 已保存的铭牌(可能为空)。
const char *provisioning_nameplate(void);
int provisioning_volume(void);
void provisioning_set_volume(int value);
void provisioning_persist_volume(void);
