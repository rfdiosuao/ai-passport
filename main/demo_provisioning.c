// main/demo_provisioning.c —— 配网页的设备屏界面。
// 显示热点名/密码/访问地址与连接状态,双击"确定"返回。配网本体在 provisioning.c。
#include "demo.h"
#include "bsp_display.h"
#include "provisioning.h"
#include "ui_font.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdio.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_info;
static lv_timer_t *s_timer;

static void refresh(lv_timer_t *t)
{
    (void)t;
    const char *state;
    switch (provisioning_state()) {
    case PROV_AP_READY:    state = "等待手机连接配置"; break;
    case PROV_CONNECTING:  state = "正在连接目标网络…"; break;
    case PROV_CONNECTED:   state = "已连接"; break;
    case PROV_WIFI_FAILED: state = "连接失败,请重试"; break;
    default:               state = "配网已关闭"; break;
    }
    lv_label_set_text_fmt(s_info,
                          "热点: %s\n密码: %s\n地址: 192.168.4.1\n\n状态: %s",
                          provisioning_ap_ssid(), provisioning_ap_password(), state);
}

void demo_provisioning_enter(void)
{
    s_scr = ui_pixel_screen_create_ex("配网", ui_font_body(), "双击返回");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 14, 58, 212, 180, UI_PAPER);
    s_info = lv_label_create(panel);
    lv_obj_set_style_text_font(s_info, ui_font_body(), 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info, 180);
    lv_obj_align(s_info, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_label_set_text(s_info, "正在启动热点…");
    lv_screen_load(s_scr);
    s_timer = lv_timer_create(refresh, 500, NULL);
    refresh();
}

esp_err_t demo_provisioning_start(void)
{
    return provisioning_start();
}

esp_err_t demo_provisioning_stop(void)
{
    return provisioning_stop();
}

void demo_provisioning_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; s_info = NULL; }
}

void demo_provisioning_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn; (void)ev;   // 本页无需额外按键;双击返回由 main.c 统一拦截。
}
