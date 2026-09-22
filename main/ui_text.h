// main/ui_text.h —— 显示宽度感知的 UTF-8 文本裁剪。
//
// 为什么需要它:LVGL 的 snprintf("%.18s") 按字节截断,中文一个字 3 字节,
// 既会少显示字,又可能切在字符中间产生非法 UTF-8 —— 字体再全也画不出来。
// 这里按"显示列数"截断,全角算 2 列、半角算 1 列,且只在字符边界切割。
#pragma once

#include <stddef.h>

// 取 src 的前 max_cols 显示列写入 dst(总是以 NUL 结尾)。
// 全角(CJK/假名/谚文/全角标点等)计 2 列,其余计 1 列。
// 发生截断时,末尾写 "…"(本身占 2 列,已计入预算内)。
// 非法 UTF-8 序列立即停止(不猜、不补),返回已写入的字节数(不含结尾 NUL)。
size_t ui_text_prefix_cols(const char *src, size_t max_cols, char *dst, size_t dst_size);

// 单个码点的显示列数:2 = 全角,1 = 半角。
int ui_text_codepoint_cols(unsigned long codepoint);
