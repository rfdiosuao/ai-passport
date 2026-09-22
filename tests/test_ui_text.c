// tests/test_ui_text.c —— 显示宽度感知的 UTF-8 裁剪(纯逻辑,可在主机上跑)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui_text.h"

int main(void)
{
    char out[64];
    size_t n;

    // 1) 整串放得下:不截断、不加省略号。
    n = ui_text_prefix_cols("abc", 10, out, sizeof(out));
    assert(n == 3 && strcmp(out, "abc") == 0);

    // 2) 半角截断:预算留出省略号的 2 列。
    n = ui_text_prefix_cols("abcdefgh", 5, out, sizeof(out));
    assert(strcmp(out, "abc…") == 0);

    // 3) 中文全角占 2 列:6 列 = 2 个汉字 + 省略号。
    n = ui_text_prefix_cols("你好世界", 6, out, sizeof(out));
    assert(strcmp(out, "你好…") == 0);

    // 4) 中文整串恰好放得下(4 字 = 8 列)。
    n = ui_text_prefix_cols("你好世界", 8, out, sizeof(out));
    assert(strcmp(out, "你好世界") == 0);

    // 5) 中英混排:按列计宽,只在字符边界切。
    n = ui_text_prefix_cols("Mi-家客厅", 8, out, sizeof(out));   // M i - 家 客 厅 = 9 列
    assert(strcmp(out, "Mi-家…") == 0);

    // 6) 非法 UTF-8:立即停止,不猜测、不输出半个字符。
    n = ui_text_prefix_cols("ab\xFFzz", 20, out, sizeof(out));
    assert(strcmp(out, "ab…") == 0);

    // 7) 目标缓冲区太小:不越界、始终以 NUL 结尾。
    n = ui_text_prefix_cols("你好", 8, out, 4);
    assert(strlen(out) <= 3 && out[3] == '\0');

    // 8) 空串与空目标:安全返回。
    assert(ui_text_prefix_cols("", 5, out, sizeof(out)) == 0 && out[0] == '\0');
    assert(ui_text_prefix_cols("abc", 5, NULL, 0) == 0);

    // 9) 单码点宽度判定。
    assert(ui_text_codepoint_cols(0x4E2D) == 2);   // 中
    assert(ui_text_codepoint_cols('A') == 1);
    assert(ui_text_codepoint_cols(0xFF0C) == 2);   // ，(全角逗号)
    assert(ui_text_codepoint_cols('\n') == 0);     // 控制字符

    puts("ui_text UTF-8 width-aware truncation tests: PASS");
    return 0;
}
