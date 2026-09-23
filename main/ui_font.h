// main/ui_font.h —— 应用字体入口。
//
// 本板默认的 Montserrat 14/20 没有中文字形,直接用它画中文会得到空白或方框。
// 这里统一提供:
//   ui_font_body()  —— 中文字库(16px),覆盖 GB2312 全字集 + ASCII + CJK 标点
//   ui_font_title() —— 标题字体(Montserrat 20),用于品牌与拉丁文标题
// 详见 docs/development/engineering/lvgl-chinese-fonts.md。
#pragma once

#include "lvgl.h"

// 在 LVGL 初始化之后、创建任何控件之前调用一次。
void ui_font_init(void);

// 正文/中文标题字体。未初始化时退化为 Montserrat 14(会被日志提示)。
const lv_font_t *ui_font_body(void);

// 标题字体:语音应用使用同一套正文汉字字体，避免额外加载标题字库。
const lv_font_t *ui_font_title(void);
