# -*- coding: utf-8 -*-
"""
fontlib.py — Hard Truck Apocalypse / Ex Machina 中文字库核心库

游戏字体格式（已逆向确认）：
- data/if/fonts/fonts.xml：每个 <Item> 是一个字号，指向一张 DXT5(.dds) 图集。
- 每个字符一条 <Symbol value="字" abc="a b c" tcs="0 u0 0 u1 vrow"/>：
    * tcs 共 5 个数：[0, v_top, 0, v_bottom, v_row_marker]
      - v_top / v_bottom 是字符在图集上的 v 边（归一化 0~1，=像素y / 图集高）
      - 第 1、3 个固定为 0（占位）
      - 第 5 个是行标记（主行 = 0.078，随行递增；本探针沿用主行值）
    * abc = 像素宽度 [左留白, 字形宽, 右留白]
- 图集是 DXT5(BC3) 压缩，白色字形 + alpha 遮罩。
- 引擎按 XML 属性 value 的【解析后字符串】匹配字符，所以编码只要声明与字节自洽即可。

本库依赖：
- Pillow（读/写 PNG、渲染字形）
- texconv.exe（微软 DirectXTex，DDS<->PNG 互转；放 fontgen/ 下）
"""
import os
import re
import struct
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont

# ----------------------------------------------------------------------------
# 路径
# ----------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(HERE, "texconv.exe")
DEFAULT_TTF = os.path.join(HERE, "SourceHanSansHWSC-VF.ttf")


# ----------------------------------------------------------------------------
# DDS 互转（经 texconv）
# ----------------------------------------------------------------------------
def decode_dds_to_rgba(dds_path):
    """DXT5 DDS -> Pillow RGBA Image（用 texconv 转 PNG 再读）。"""
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(
            [TEXCONV, dds_path, "-f", "R8G8B8A8_UNORM", "-ft", "png", "-o", td, "-y"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        png = next((os.path.join(td, f) for f in os.listdir(td) if f.lower().endswith(".png")))
        return Image.open(png).convert("RGBA")


def encode_rgba_to_dds(image, dds_path):
    """Pillow RGBA Image -> DXT5 DDS（先写 PNG，再 texconv 转 BC3）。"""
    dds_path = os.path.abspath(dds_path)
    os.makedirs(os.path.dirname(dds_path), exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        png = os.path.join(td, "atlas.png")
        image.save(png, "PNG")
        subprocess.run(
            [TEXCONV, png, "-f", "BC3_UNORM", "-ft", "dds", "-o", os.path.dirname(dds_path),
             "-y", "-nologo"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        out = os.path.join(os.path.dirname(dds_path), "atlas.dds")
        if not os.path.exists(out):
            raise RuntimeError("texconv 未生成 atlas.dds")
        # texconv 输出文件名基于输入名，移动/改名到目标
        if os.path.abspath(out) != dds_path:
            if os.path.exists(dds_path):
                os.remove(dds_path)
            os.replace(out, dds_path)


# ----------------------------------------------------------------------------
# 字体 / 字形渲染
# ----------------------------------------------------------------------------
def load_font(ttf_path, pixel_size):
    return ImageFont.truetype(ttf_path, pixel_size)


def render_glyph_rgba(ch, font, pad=1):
    """把单个字符渲染成 RGBA 图：白色字形 + alpha 遮罩，背景透明。
    返回 (Image, glyph_w_px)。图尺寸 = 字形包围盒 + pad。"""
    # 先用临时图测尺寸
    tmp = Image.new("L", (font.size * 2, font.size * 2), 0)
    d = ImageDraw.Draw(tmp)
    d.text((pad, pad), ch, fill=255, font=font)
    bbox = tmp.getbbox()
    if bbox is None:
        # 空格等无墨字符：给 1px 占位
        bbox = (pad, pad, pad + 1, pad + 1)
    x0, y0, x1, y1 = bbox
    w = x1 - x0
    h = y1 - y0
    img = Image.new("RGBA", (w + pad * 2, h + pad * 2), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    d.text((pad - x0, pad - y0), ch, fill=(255, 255, 255, 255), font=font)
    return img, w


# ----------------------------------------------------------------------------
# fonts.xml 读写
# ----------------------------------------------------------------------------
def detect_xml_encoding(raw):
    """从 XML 声明推断编码名（仅用于日志/默认，不做强制解码）。"""
    m = re.search(rb'encoding\s*=\s*["\']([^"\']+)["\']', raw)
    if m:
        decl = m.group(1).decode("ascii", "ignore").lower()
        if "1251" in decl:
            return "cp1251"
        elif "gbk" in decl or "gb2312" in decl:
            return "gbk"
        elif "utf-8" in decl or "utf8" in decl:
            return "utf-8"
    return "utf-8"


def iter_items(xml_bytes):
    """在字节流上定位 <Item ...>...</Item> 块，返回 [(block_bytes, start, end)]。
    不解码，避免 1251 不可映射字节丢失。"""
    items = []
    for m in re.finditer(rb"<Item\b", xml_bytes):
        s = m.start()
        e = xml_bytes.find(b"</Item>", s)
        if e == -1:
            e2 = xml_bytes.find(b"/>", s)
            e = e2 + 2
        else:
            e = e + len(b"</Item>")
        items.append((xml_bytes[s:e], s, e))
    return items


def item_file_and_symbols(item_block):
    """从 Item 块里取 file 属性与所有 <Symbol .../> 字符串列表。"""
    file_m = re.search(r'file\s*=\s*"([^"]+)"', item_block, re.IGNORECASE)
    file = file_m.group(1) if file_m else None
    symbols = re.findall(r"<Symbol\b[^>]*?/>", item_block, re.DOTALL)
    return file, symbols


def build_symbol(value, abc_a, abc_b, abc_c, u_left, u_right, v_bottom, v_row=0.078):
    """生成一条 <Symbol> 文本。
    tcs 五元组已逆向确认 = [0, u_left, 0, u_right, v_bottom]：
      第2/4 = 字符水平 UV（x0/H, x1/H）；第5 = 字符行底 V（y1/H，引擎据行间堆叠推 v_top）。
    value 由调用方以正确编码给出。"""
    return (
        f'\t\t<Symbol\n'
        f'\t\t\tvalue="{value}"\n'
        f'\t\t\tabc="{abc_a:.3f} {abc_b:.3f} {abc_c:.3f}"\n'
        f'\t\t\ttcs="0.000 {u_left:.3f} 0.000 {u_right:.3f} {v_bottom:.3f}" />'
    )


def make_fonts_xml_bytes(orig_xml_bytes, item_index, placements, W, H, encoding, v_row=0.078):
    """在指定 Item 的 </Item> 前插入汉字 Symbol。
    原文件按 cp1251 解码（保留原西里尔/拉丁/特殊符号），整体以目标编码重写。
    返回 new_bytes（encoding 声明已更新为目标编码）。"""
    src_text = orig_xml_bytes.decode("cp1251", "replace")
    items = iter_items(orig_xml_bytes)
    if item_index >= len(items):
        raise RuntimeError(f"Item 索引 {item_index} 超出范围（共 {len(items)} 个）")
    block, s, e = items[item_index]
    appends = []
    for ch, x0, y0, x1, y1 in placements:
        u_left = x0 / W
        u_right = x1 / W
        v_bottom = y1 / H
        abc_b = (x1 - x0)
        sym = build_symbol(ch, 0.0, float(abc_b), 0.0, u_left, u_right, v_bottom, v_row)
        appends.append(sym)
    insert_text = "\n".join(appends) + "\n"
    # 在 Item 块的 </Item> 前插入（用字符串定位，因已解码）
    block_str = block.decode("cp1251", "replace")
    rel = block_str.rfind("</Item>")
    if rel == -1:
        rel = len(block_str)
    new_block_str = block_str[:rel] + insert_text + block_str[rel:]
    new_text = src_text[:s] + new_block_str + src_text[e:]
    new_text = re.sub(r'encoding\s*=\s*["\']([^"\']+)["\']',
                      'encoding="%s"' % encoding, new_text, count=1)
    return new_text.encode(encoding, "replace")


# ----------------------------------------------------------------------------
# 汉字提取
# ----------------------------------------------------------------------------
CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uF900-\uFAFF]")


def extract_cjk(text):
    """从文本里提取去重汉字集合（保持顺序）。"""
    seen = set()
    out = []
    for ch in text:
        if CJK_RE.match(ch) and ch not in seen:
            seen.add(ch)
            out.append(ch)
    return out


def scan_xml_dirs_for_cjk(dirs, enc_hint="cp1251"):
    """扫描若干目录下的 xml，汇总出现过的汉字。"""
    import glob
    chars = []
    seen = set()
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fp in glob.glob(os.path.join(d, "**", "*.xml"), recursive=True):
            try:
                with open(fp, "rb") as f:
                    raw = f.read()
            except Exception:
                continue
            enc = "utf-8"
            m = re.search(rb'encoding\s*=\s*["\']([^"\']+)["\']', raw)
            if m:
                decl = m.group(1).decode("ascii", "ignore").lower()
                if "1251" in decl:
                    enc = "cp1251"
                elif "gbk" in decl or "gb2312" in decl:
                    enc = "gbk"
            try:
                txt = raw.decode(enc, "ignore")
            except Exception:
                txt = raw.decode("utf-8", "ignore")
            for ch in extract_cjk(txt):
                if ch not in seen:
                    seen.add(ch)
                    chars.append(ch)
    return chars
