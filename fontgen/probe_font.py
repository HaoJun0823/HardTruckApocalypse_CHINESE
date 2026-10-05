# -*- coding: utf-8 -*-
"""
probe_font.py — 编码 + 字库探针（Hard Truck Apocalypse 汉化）

目的：验证两件事，再决定最终字库方案。
  1) 编码：引擎接受 UTF-8 还是 GBK 的 fonts.xml / strings.xml 的汉字 value？
  2) 字库：给主菜单用的字号(Idx=0, SM_Tahoma 12px) 追加汉字字形，能否正常显示。

产出（覆盖目录）：
  - update/      -> UTF-8 版（data/if/fonts/... + data/if/strings/uieditstrings.xml）
  - update_gbk/  -> GBK 版（对照测试用）

用法：
  python probe_font.py
  python probe_font.py --game "I:/LocalGames/Hard Truck Apocalypse STEAM"
"""
import os
import sys
import argparse
import re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fontlib as F

# 探针用字：主菜单可见文本所需的最小汉字集合
PROBE_CHARS = list("新游戏读取存档")

FONT_ITEM_INDEX = 0      # 主菜单字号 = fonts.xml 第 0 个 Item（SM_Tahoma 12.000）
FONT_TTF_SIZE = 12       # 与 height="12.000" 对齐
GLYPH_SLOT_W = 24        # 每个汉字占的槽宽(px)
GLYPH_H = 18             # 字形区域高(px)
PLACE_Y = 210            # 贴入图集的起始 y（底部空白区 208~255）
V_ROW_MARKER = 0.078     # 沿用主行 tcs 第5数（实机验证后若不对再调）


def gen_atlas_with_glyphs(src_dds, chars, ttf_path, font_size):
    """解码原图集 -> 在底部空白区画入汉字 -> 返回 (新RGBA图, [(ch, x0,y0,x1,y1), ...], (W,H))"""
    img = F.decode_dds_to_rgba(src_dds)
    W, H = img.size
    font = F.load_font(ttf_path, font_size)
    placements = []
    x = 4
    for ch in chars:
        g, gw = F.render_glyph_rgba(ch, font, pad=1)
        gh = g.size[1]
        scale = min(1.0, (GLYPH_H - 2) / gh) if gh > 0 else 1.0
        if scale < 1.0:
            g = g.resize((max(1, int(g.size[0] * scale)), max(1, int(g.size[1] * scale))), Image.LANCZOS)
        gy = PLACE_Y + (GLYPH_H - g.size[1]) // 2
        img.paste(g, (x, gy), g)
        placements.append((ch, x, gy, x + g.size[0], gy + g.size[1]))
        x += GLYPH_SLOT_W
    return img, placements, (W, H)


def make_strings_xml_bytes(orig_bytes, encoding):
    """把 New Game / Load Game 的 value 改成中文。原文件 cp1251 解码后整体重写。"""
    src = orig_bytes.decode("cp1251", "replace")
    repl = {
        'value="New Game"': 'value="新游戏"',
        'value="Load game"': 'value="读取存档"',
    }
    for old, new in repl.items():
        src = src.replace(old, new)
    src = re.sub(r'encoding\s*=\s*["\']([^"\']+)["\']',
                 'encoding="%s"' % encoding, src, count=1)
    return src.encode(encoding, "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=r"I:\LocalGames\Hard Truck Apocalypse STEAM")
    ap.add_argument("--ws", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    ws = args.ws
    game = args.game
    orig_fonts = os.path.join(ws, "Original_DATA", "data", "if", "fonts", "fonts.xml")
    orig_dds = os.path.join(ws, "Original_DATA", "data", "if", "fonts", "SM_Tahoma_12.000_00.dds")
    orig_strings = os.path.join(ws, "Original_DATA", "data", "if", "strings", "uieditstrings.xml")
    ttf = F.DEFAULT_TTF

    # 1) 图集 + 字形布局
    new_img, placements, (W, H) = gen_atlas_with_glyphs(orig_dds, PROBE_CHARS, ttf, FONT_TTF_SIZE)
    print("[probe] 图集 %dx%d，汉字布局: %s" % (
        W, H, ", ".join("%s@(%d,%d)" % (ch, x0, y0) for ch, x0, y0, x1, y1 in placements)))

    # 2) 读原始字节
    with open(orig_fonts, "rb") as f:
        fonts_bytes = f.read()
    with open(orig_strings, "rb") as f:
        strings_bytes = f.read()

    # 3) 生成两套
    fonts_utf8 = F.make_fonts_xml_bytes(fonts_bytes, FONT_ITEM_INDEX, placements, W, H, "utf-8", V_ROW_MARKER)
    fonts_gbk = F.make_fonts_xml_bytes(fonts_bytes, FONT_ITEM_INDEX, placements, W, H, "gbk", V_ROW_MARKER)
    str_utf8 = make_strings_xml_bytes(strings_bytes, "utf-8")
    str_gbk = make_strings_xml_bytes(strings_bytes, "gbk")

    # 4) 写出
    dest_utf8 = os.path.join(game, "update")
    dest_gbk = os.path.join(game, "update_gbk")

    def dump(root, fb, sb):
        os.makedirs(os.path.join(root, "data", "if", "fonts"), exist_ok=True)
        os.makedirs(os.path.join(root, "data", "if", "strings"), exist_ok=True)
        F.encode_rgba_to_dds(new_img, os.path.join(root, "data", "if", "fonts", "SM_Tahoma_12.000_00.dds"))
        with open(os.path.join(root, "data", "if", "fonts", "fonts.xml"), "wb") as f:
            f.write(fb)
        with open(os.path.join(root, "data", "if", "strings", "uieditstrings.xml"), "wb") as f:
            f.write(sb)

    dump(dest_utf8, fonts_utf8, str_utf8)
    dump(dest_gbk, fonts_gbk, str_gbk)

    print("[probe] 已写出:")
    print("  UTF-8 -> %s\\data\\if\\..." % dest_utf8)
    print("  GBK   -> %s\\data\\if\\..." % dest_gbk)
    print("[probe] 测试: 把 update\\data 复制到游戏 update\\ 启动，看主菜单『新游戏/读取存档』")
    print("         若乱码/不显示，改用 update_gbk\\data 试。告诉我哪种生效。")


if __name__ == "__main__":
    main()
