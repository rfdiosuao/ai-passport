// main/ui_text.c —— 显示宽度感知的 UTF-8 裁剪(无 LVGL 依赖,可在主机上单测)。
#include "ui_text.h"

#include <stdbool.h>
#include <string.h>

#define ELLIPSIS "\xE2\x80\xA6"   // U+2026 …
#define ELLIPSIS_BYTES 3
#define ELLIPSIS_COLS  2

int ui_text_codepoint_cols(unsigned long cp)
{
    // 控制字符不占位;C0 之外的控制码位也按 0 处理,避免把光标类字符算进宽度。
    if (cp < 0x20 || cp == 0x7F) return 0;
    if (cp < 0x7F) return 1;

    // 全角/宽字符区间。按 wcwidth 的常见实现裁剪,只保留本板可能出现的区段。
    if (cp >= 0x1100 && (
            cp <= 0x115F ||                      // 谚文字母
            cp == 0x2329 || cp == 0x232A ||
            (cp >= 0x2E80 && cp <= 0x303E) ||    // CJK 部首、CJK 标点
            (cp >= 0x3041 && cp <= 0x33FF) ||    // 假名、注音、CJK 兼容
            (cp >= 0x3400 && cp <= 0x4DBF) ||    // CJK 扩展 A
            (cp >= 0x4E00 && cp <= 0x9FFF) ||    // CJK 基本区
            (cp >= 0xA000 && cp <= 0xA4CF) ||    // 彝文
            (cp >= 0xAC00 && cp <= 0xD7A3) ||    // 谚文音节
            (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 兼容表意文字
            (cp >= 0xFE10 && cp <= 0xFE19) ||    // 竖排标点
            (cp >= 0xFE30 && cp <= 0xFE6F) ||    // CJK 兼容形式
            (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角 ASCII 变体
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x1F300 && cp <= 0x1F64F) ||  // 常用 emoji
            (cp >= 0x20000 && cp <= 0x3FFFD)))   // CJK 扩展 B 及以后
        return 2;
    return 1;
}

// 解一个 UTF-8 码点。返回字节数;非法或截断返回 -1,不猜测内容。
static int utf8_next(const char *s, size_t remaining, unsigned long *out)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char b0 = u[0];
    size_t need;
    unsigned long value;

    if (b0 < 0x80) {
        *out = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        need = 1; value = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2; value = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3; value = b0 & 0x07;
    } else {
        return -1;
    }

    if (remaining < need + 1) return -1;
    for (size_t i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) return -1;
        value = (value << 6) | (unsigned long)(u[i] & 0x3F);
    }

    // 过长编码、代理区、超出 Unicode 范围一律视为非法。
    if ((need == 1 && value < 0x80) || (need == 2 && value < 0x800) ||
        (need == 3 && value < 0x10000) || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
        return -1;

    *out = value;
    return (int)(need + 1);
}

// 拷贝不超过 limit_cols 列的前缀。*stopped_early 表示后面还有内容没放下。
static size_t copy_prefix(const char *src, size_t limit_cols, char *dst,
                          size_t dst_size, bool *stopped_early)
{
    size_t out = 0, cols = 0;
    const char *p = src;
    size_t remaining = strlen(src);

    *stopped_early = false;
    while (remaining > 0) {
        unsigned long cp = 0;
        int len = utf8_next(p, remaining, &cp);
        if (len < 0) {                    // 非法序列:就此停止,并按"有剩余"处理
            *stopped_early = true;
            break;
        }
        int w = ui_text_codepoint_cols(cp);
        if (cols + (size_t)w > limit_cols || out + (size_t)len + 1 > dst_size) {
            *stopped_early = true;
            break;
        }
        memcpy(dst + out, p, (size_t)len);
        out += (size_t)len;
        cols += (size_t)w;
        p += (size_t)len;
        remaining -= (size_t)len;
    }
    dst[out] = '\0';
    return out;
}

size_t ui_text_prefix_cols(const char *src, size_t max_cols, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;

    bool stopped = false;
    size_t out = copy_prefix(src, max_cols, dst, dst_size, &stopped);
    if (!stopped) return out;                       // 整串都放得下

    // 放不下:给省略号留出 2 列和 3 字节,再补 "…",保证总宽度不超预算。
    if (dst_size < ELLIPSIS_BYTES + 1) return out;  // 连省略号都放不下,保留已拷贝部分
    size_t out2 = copy_prefix(src, max_cols >= ELLIPSIS_COLS ? max_cols - ELLIPSIS_COLS : 0,
                              dst, dst_size - ELLIPSIS_BYTES, &stopped);
    memcpy(dst + out2, ELLIPSIS, ELLIPSIS_BYTES);
    out2 += ELLIPSIS_BYTES;
    dst[out2] = '\0';
    return out2;
}
