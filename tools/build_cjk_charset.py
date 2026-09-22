#!/usr/bin/env python3
"""生成中文字库的字符清单(assets/fonts/passport_cjk.chars.txt)。

清单 = ASCII 可打印 + GB/T 2312 全字集(一级 3755 + 二级 3008) + 少量 GB2312 未收的
常用标点。它是字库的"覆盖契约":凡不在清单里的字符,界面会以占位符显示而不是静默消失。

字源:Source Han Sans SC(思源黑体, SIL Open Font License 1.1)。
转换:lv_font_conv(见 package.json / README),命令见本文件末尾的说明。

用法:
  python3 tools/build_cjk_charset.py             # 只重写字符清单
  python3 tools/build_cjk_charset.py --font-out /path/to/dir   # 并下载字源到该目录
"""
import argparse
import os
import sys
import urllib.request

FONT_URL = ("https://raw.githubusercontent.com/adobe-fonts/source-han-sans/"
            "release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf")

# GB2312 未收、但中文设备名/SSID/界面常用的标点与符号。
EXTRA = "·—…“”‘’《》〈〉【】「」『』、。，！？：；（）％℃±×÷→←↑↓№①⑫㎡°"


def build_inventory():
    chars = set()

    def add(cp):
        if cp < 0x20 or cp == 0x7F:
            return
        chars.add(chr(cp))

    for cp in range(0x20, 0x7F):        # ASCII 可打印(用于单位/数字/英文名)
        add(cp)
    for lead in range(0xA1, 0xF8):      # GB/T 2312 双字节区
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes([lead, trail]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            add(ord(ch))
    for ch in EXTRA:
        add(ord(ch))
    return sorted(chars)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font-out", help="下载字源到此目录(可选)")
    args = ap.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    font_dir = os.path.join(repo_root, "assets", "fonts")
    os.makedirs(font_dir, exist_ok=True)

    ordered = build_inventory()
    inventory = "".join(ordered)
    hanzi = [c for c in ordered if "\u4e00" <= c <= "\u9fff"]

    path = os.path.join(font_dir, "passport_cjk.chars.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Character inventory for passport_cjk_16 (one code point per line).\n")
        f.write("# Source font: Source Han Sans SC Regular, SIL OFL 1.1.\n")
        f.write("# Regenerate the .c with lv_font_conv; see tools/build_cjk_charset.py.\n")
        for ch in ordered:
            f.write(ch + "\n")

    print(f"字符清单: {path}")
    print(f"  总码点 {len(ordered)}  汉字 {len(hanzi)}  非汉字 {len(ordered) - len(hanzi)}")
    print()
    print("复现字库(lv_font_conv):")
    print(f"  lv_font_conv --font <SourceHanSansSC-Regular.otf> "
          f"--range 0x20-0x7E --symbols \"{inventory if False else '<字符清单内容>'}\" "
          f"--size 16 --bpp 4 --format lvgl --no-compress "
          f"--lv-font-name passport_cjk_16 --lv-include lvgl.h "
          f"--output assets/fonts/passport_cjk_16.c")

    if args.font_out:
        os.makedirs(args.font_out, exist_ok=True)
        dst = os.path.join(args.font_out, "SourceHanSansSC-Regular.otf")
        print(f"\n下载字源 -> {dst}")
        req = urllib.request.Request(FONT_URL, headers={"User-Agent": "SUMMON-Adopter/0.1"})
        with urllib.request.urlopen(req, timeout=300) as r, open(dst, "wb") as out:
            total = 0
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                out.write(chunk)
                total += len(chunk)
        print(f"  已保存 {total:,} 字节")


if __name__ == "__main__":
    sys.exit(main())
