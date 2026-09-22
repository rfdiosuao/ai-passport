// main/main.c —— FoloToy AI Passport BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  双击   演示页中=返回菜单
//   确定  长按   演示页中=返回菜单(与双击等效,留一条更好按的备用路径)
// 界面文案为简体中文,由 passport_cjk_16 绘制(见 ui_font.c)。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "demo_navigation.h"
#include "provisioning.h"
#include "ui_font.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { .name = "显示", .enter = demo_display_enter, .exit = demo_display_exit,
      .key = demo_display_key },
    { .name = "按键", .enter = demo_button_enter, .exit = demo_button_exit,
      .key = demo_button_key },
    { .name = "音频", .enter = demo_audio_enter, .exit = demo_audio_exit,
      .key = demo_audio_key, .start = demo_audio_start, .stop = demo_audio_stop },
    { .name = "电池", .enter = demo_battery_enter, .exit = demo_battery_exit,
      .key = demo_battery_key },
    { .name = "无线", .enter = demo_wifi_enter, .exit = demo_wifi_exit,
      .key = demo_wifi_key, .start = demo_wifi_start, .stop = demo_wifi_stop },
    { .name = "蓝牙", .enter = demo_ble_enter, .exit = demo_ble_exit,
      .key = demo_ble_key, .start = demo_ble_start, .stop = demo_ble_stop },
    { .name = "低功耗", .enter = demo_low_power_enter, .exit = demo_low_power_exit,
      .key = demo_low_power_key, .start = demo_low_power_start, .stop = demo_low_power_stop },
    { .name = "配网", .enter = demo_provisioning_enter, .exit = demo_provisioning_exit,
      .key = demo_provisioning_key, .start = demo_provisioning_start, .stop = demo_provisioning_stop },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))
#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static demo_navigation_t s_navigation;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : " 失败");
        ui_pixel_set_selected(s_cards[i], i == s_navigation.selected, s_ok[i]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    // 品牌名保留拉丁文用标题字体;其余中文文案由 ui_font_body() 绘制。
    s_menu_scr = ui_pixel_screen_create_ex("FoloToy", ui_font_title(), "上下选择 确定进入");

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x = 11 + (int)(i % 2) * 112;
        int y = 52 + (int)(i / 2) * 47;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, 102, 40, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], ui_font_body(), 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 242);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

static void enter_menu(void) {
    menu_build();
}

static demo_nav_input_t navigation_input(bsp_btn_t btn, bsp_btn_ev_t event) {
    // 只把"确定"的双击/长按翻译成返回;上/下的双击仍是页面自定义事件。
    if (btn == BSP_BTN_OK && event == BSP_BTN_DOUBLE) return DEMO_NAV_INPUT_OK_DOUBLE;
    if (btn == BSP_BTN_OK && event == BSP_BTN_LONG) return DEMO_NAV_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK) return DEMO_NAV_INPUT_OTHER;
    if (btn == BSP_BTN_UP) return DEMO_NAV_INPUT_UP_CLICK;
    if (btn == BSP_BTN_DOWN) return DEMO_NAV_INPUT_DOWN_CLICK;
    if (btn == BSP_BTN_OK) return DEMO_NAV_INPUT_OK_CLICK;
    return DEMO_NAV_INPUT_OTHER;
}

static void process_input(const input_event_t *input) {
    demo_nav_input_t nav_input = navigation_input(input->btn, input->event);

    if (s_navigation.active >= 0) {
        demo_nav_result_t result = demo_navigation_handle(&s_navigation, nav_input, true);
        const demo_entry_t *demo = &DEMOS[result.index];
        if (result.action == DEMO_NAV_ACTION_EXIT) {
            esp_err_t e = demo->stop ? demo->stop() : ESP_OK;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "%s 页面停止失败: %s", demo->name, esp_err_to_name(e));
                return;
            }
            if (!bsp_lvgl_lock(500)) return;
            demo->exit();
            demo_navigation_complete_exit(&s_navigation);
            enter_menu();
            bsp_lvgl_unlock();
        } else if (result.action == DEMO_NAV_ACTION_FORWARD) {
            demo->key(input->btn, input->event);
        }
        return;
    }

    // 菜单里"确定"双击/长按没有语义,直接忽略,避免落到页面事件里。
    if (nav_input == DEMO_NAV_INPUT_OTHER || nav_input == DEMO_NAV_INPUT_OK_LONG ||
        nav_input == DEMO_NAV_INPUT_OK_DOUBLE) return;
    if (!bsp_lvgl_lock(500)) return;
    demo_nav_result_t result = demo_navigation_handle(
        &s_navigation, nav_input, s_ok[s_navigation.selected]);
    if (result.action == DEMO_NAV_ACTION_REFRESH) {
        menu_refresh();
        ui_pixel_mascot_jump(s_mascot);
    } else if (result.action == DEMO_NAV_ACTION_ENTER) {
        const demo_entry_t *demo = &DEMOS[result.index];
        ui_pixel_mascot_jump(s_mascot);
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
        s_mascot = NULL;
        demo->enter();
        bsp_lvgl_unlock();

        esp_err_t e = demo->start ? demo->start() : ESP_OK;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s 页面启动失败: %s", demo->name, esp_err_to_name(e));
        }
        return;
    }
    bsp_lvgl_unlock();
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "demo_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport BSP demo 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本 demo 的 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(那会让本文件复杂一倍,违背参考示例的初衷)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    // 必须在创建任何控件之前初始化字体,否则控件会绑定到没有中文字形的默认字体。
    ui_font_init();

    demo_navigation_init(&s_navigation, DEMO_COUNT);
    // 读取已存 Wi-Fi/铭牌配置(决定首启是否提示配网)。失败不阻塞其它功能。
    if (provisioning_init() != ESP_OK) {
        ESP_LOGW(TAG, "配网初始化失败(可能无 NVS 分区)");
    }

    // 其余外设单项失败不阻塞:菜单里标 [FAIL],其他项照常可测。
    s_ok[0] = true;                                   // Display 已确认可用
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    s_ok[1] = input_err == ESP_OK && button_err == ESP_OK;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
    }
    s_ok[2] = (bsp_audio_init() == ESP_OK);
    s_ok[3] = (bsp_battery_init() == ESP_OK);
    s_ok[4] = true;                                    // 页面内按需初始化并显示错误
    s_ok[5] = true;
    s_ok[6] = true;
    s_ok[7] = true;                                    // 配网:SoftAP 按需启动

    if (bsp_lvgl_lock(1000)) {
        enter_menu();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3]);
}
