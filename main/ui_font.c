// main/ui_font.c —— 中文字体描述符与回退链。
#include "ui_font.h"

#include "esp_log.h"

LV_FONT_DECLARE(passport_cjk_16);

static const char *TAG = "ui_font";

// 可写的浅拷贝:主字体是中文字库,回退到 Montserrat 只为补它没有的字形
// (主要是 LVGL 图标所在的私用区)。静态位图字体才允许这样拷贝描述符,
// 动态字体引擎不能照搬。见 lvgl-chinese-fonts.md 第 5 节。
static lv_font_t s_body;
static bool s_ready;

void ui_font_init(void)
{
    s_body = passport_cjk_16;
    s_body.fallback = &lv_font_montserrat_14;
    s_ready = true;
    ESP_LOGI(TAG, "中文字体就绪:passport_cjk_16 (回退 montserrat_14)");
}

const lv_font_t *ui_font_body(void)
{
    if (!s_ready) {
        // 调用顺序错误是可诊断的:给日志、也回退到一个能画的字体,避免整屏空白。
        ESP_LOGW(TAG, "ui_font_init() 未调用,暂时回退 Montserrat 14(中文会缺字形)");
        return &lv_font_montserrat_14;
    }
    return &s_body;
}

const lv_font_t *ui_font_title(void)
{
    return ui_font_body();
}
