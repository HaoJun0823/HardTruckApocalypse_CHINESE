# -*- coding: utf-8 -*-
"""
build_atlas.py —— 生成带汉字字形的字体图集 + fonts.xml + 槽位映射表

═══════════════════════════════════════════════════════════════════════════
逆向确认的前提（详见 HardTruckApocalypse_CHINESE_DLL/README.md）
═══════════════════════════════════════════════════════════════════════════

1. 引擎字形表是 **256 项单字节索引**（Font+0x40，索引 = 零扩展单字节 ×4）。
   不修改 exe 就**不能扩表**，所以汉字必须映射到 0x80..0xFF 的槽位。

2. 槽位字节禁区（绘制 sub_685CA0 / 度量 sub_685990 会劫持）：
       0x00..0x1F  静默跳过（零宽、不查字形）
       0x23 '#'    切状态位
       0x24 '$'    状态位未置位时跳过
       0x26 '&'    同上
       0x40 '@'    **8 字节 hex 颜色转义**（sscanf %x 后 add esi,8）——会吃字节
       0x7C '|'    度量里有状态切换
   所以可用槽位 = 0x80..0xFF 减去 {0x40} = 127 个。

3. tcs = [page, u0, v0, u1, v1]，全部归一化到 [0,1]
   abc = [abcA, width, abcC]，单位是「120 dpi 下的像素」
   字形屏幕尺寸 = (u1-u0)*atlasW*scale, (v1-v0)*atlasH*scale
   推进量 = abcA + width + abcC

4. fonts.xml 的 height 是**点**（1/72 英寸）；实际点阵高 = height/72*120。
   例：height=12.000 → em=20px；height=10.000 → em≈16.7px。

5. 汉字应当**等宽推进**（advance = em）才能在网格里对齐。

═══════════════════════════════════════════════════════════════════════════
关键实现决策
═══════════════════════════════════════════════════════════════════════════

现有图集几乎被填满（256×256 里已用到 y=252），放不下 127 个汉字。
因此本工具**把图集高度扩为 2 倍**（256×256 → 256×512，512×256 → 512×512），
上半部**原样保留**原有字形像素，下半部放汉字。

由于图集高度变了，**所有已有 Symbol 的 v 坐标必须按 H/newH 重新缩放**，
否则原有字形会采样错位。

⚠ 编码陷阱：槽位是 0x80..0xFF 的**裸单字节**，但 GBK 无法把 0x80..0x9F 编成
单字节（它们是 GBK 前导字节），按 GBK 写出会变成 '?'。所以全程用 **latin-1**
作为「字节空间」拼装 XML：latin-1 是 0x00..0xFF 与码位的 1:1 映射，既能原样
保留原文件的 cp1251 西里尔字节，又能让 chr(slot) 精确落成字节 slot。

用法：
  python build_atlas.py --text-dir <译文目录> [--text-dir ...]
  python build_atlas.py --charset <字符集文件> --out <输出目录>
"""
import os
import re
import sys
import glob
import struct
import argparse
import subprocess
import tempfile
import shutil

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print('需要 Pillow:  pip install pillow')
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(HERE, 'texconv.exe')
DEFAULT_TTF = os.path.join(HERE, 'SourceHanSansHWSC-VF.ttf')

# 绘制路径劫持的字节（不可用作槽位）
FORBIDDEN = set(range(0x00, 0x20)) | {0x23, 0x24, 0x26, 0x40, 0x7C}

DPI = 120.0          # height(点) -> 像素
CJK_FILL = 0.94      # 汉字占 em 的比例
CELL_PAD = 1         # 单元间距（px）


# ---------------------------------------------------------------------------
# DDS <-> PNG
# ---------------------------------------------------------------------------
def decode_dds(dds_path):
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([TEXCONV, dds_path, '-f', 'R8G8B8A8_UNORM', '-ft', 'png',
                        '-o', td, '-y', '-nologo'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        png = next(os.path.join(td, f) for f in os.listdir(td) if f.lower().endswith('.png'))
        return Image.open(png).convert('RGBA')


def encode_dds(img, dds_path):
    dds_path = os.path.abspath(dds_path)
    os.makedirs(os.path.dirname(dds_path), exist_ok=True)
    stem = os.path.splitext(os.path.basename(dds_path))[0]
    # texconv 输出目录必须与目标同盘，否则 os.replace 跨盘失败
    td = tempfile.mkdtemp(dir=os.path.dirname(dds_path))
    try:
        png = os.path.join(td, stem + '.png')
        img.save(png, 'PNG')
        subprocess.run([TEXCONV, png, '-f', 'BC3_UNORM', '-ft', 'dds',
                        '-o', td, '-y', '-nologo'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = os.path.join(td, stem + '.dds')
        if not os.path.isfile(out):
            raise RuntimeError('texconv 未生成 ' + out)
        os.replace(out, dds_path)
    finally:
        shutil.rmtree(td, ignore_errors=True)


# ---------------------------------------------------------------------------
# fonts.xml 解析（latin-1 字节空间）
# ---------------------------------------------------------------------------
DECL_RE = re.compile(rb'encoding\s*=\s*["\']([^"\']+)["\']')
ENC_ALIAS = {'windows-1251': 'cp1251', 'cp1251': 'cp1251',
             'windows-1252': 'cp1252', 'gbk': 'gbk', 'gb2312': 'gbk',
             'utf-8': 'utf-8', 'utf8': 'utf-8'}


def pick_encoding(raw):
    m = DECL_RE.search(raw[:256])
    if m:
        return ENC_ALIAS.get(m.group(1).decode('ascii', 'ignore').lower(), 'cp1251')
    return 'cp1251'


def parse_items(latin1_text):
    """在 latin-1 字节空间解析。返回 [dict(name,height,file,symbols,header)]"""
    items = []
    for blk in re.findall(r'<Item\b(.*?)</Item>', latin1_text, re.S):
        he = blk.find('>')
        hdr, body = blk[:he], blk[he:]
        nm = re.search(r'name="([^"]*)"', hdr)
        ht = re.search(r'height="([^"]*)"', hdr)
        fl = re.search(r'file="([^"]*)"', hdr)
        syms = []
        for sb in re.findall(r'<Symbol\b(.*?)/>', body, re.S):
            vm = re.search(r'value=(?:"(.*?)"(?=\s|\n|$)|\'(.*?)\')', sb, re.S)
            am = re.search(r'abc="([^"]*)"', sb)
            tm = re.search(r'tcs="([^"]*)"', sb)
            if not (vm and am and tm):
                continue
            v = vm.group(1) if vm.group(1) is not None else vm.group(2)
            # raw_value：value 属性的**原文**（含其引号形式与实体）。
            # 原文件用 value='"'（单引号）表示双引号字符，还用 &amp; 表示 &。
            # 回写时必须原样保留，否则产出非法 XML，引擎会直接拒绝加载
            # （实测：报 "ReadXmlFile: Cannot parse file" 并回退到原字体）。
            raw_value = vm.group(0)
            syms.append((v, am.group(1), tm.group(1), raw_value))
        items.append({
            'name': nm.group(1) if nm else 'SM_Tahoma',
            'height': float(ht.group(1)) if ht else 12.0,
            'file': fl.group(1) if fl else '',
            'symbols': syms,
            'header': hdr,
        })
    return items


# ---------------------------------------------------------------------------
# 渲染一个汉字到指定边长，返回紧致字形图
# ---------------------------------------------------------------------------
def render_cjk(ch, ttf_path, em, box):
    """把 ch 渲染成不超过 box×box 的紧致 RGBA 图（白色 + alpha）。

    ⚠ 画布必须按**字体真实度量**给足高度，否则 PIL 会静默裁掉字形底部。
      实测 Source Han Sans 在 size=N 时 ascent+descent ≈ 1.47N：
          size=17 -> (20, 5) -> 共 25px
      若画布只有 N+8=25，加上 pad=4 就需要 29px —— 底部 4 行直接没了，
      表现为「汉字下半截显示不出来」（实机踩到过）。
      这里一律用 asc+desc+2*pad 作为画布高，彻底杜绝裁切。

    em  : 建字体用的 em 尺寸（点阵高）
    box : 目标 em 边长（通常 = em * CJK_FILL）

    ⚠ 缩放必须用**统一系数** k = box/em，不能按各自墨迹缩放到填满 box。
      否则笔画多的字（如「藏」）和笔画少的字（如「一」「口」）会被拉到同一
      外接尺寸 —— 字库里看是"都填满了"，实机上就是「高矮不一样」。
      实测踩到过：新/游/戏/读 各自填满后，视觉大小明显不齐。
    """
    font = ImageFont.truetype(ttf_path, em)
    asc, desc = font.getmetrics()
    pad = 6
    W = em + pad * 2
    H = asc + desc + pad * 2          # 按真实度量，绝不裁切
    cv = Image.new('L', (W, H), 0)
    ImageDraw.Draw(cv).text((pad, pad), ch, fill=255, font=font)
    bbox = cv.getbbox()
    if not bbox:
        return None
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return None
    # ★ 统一系数：只与 em 有关，与具体字形的墨迹大小无关
    k = box / float(em)
    nw = max(1, int(round(w * k)))
    nh = max(1, int(round(h * k)))
    g = cv.crop(bbox).resize((nw, nh), Image.LANCZOS)
    white = Image.new('RGBA', (nw, nh), (255, 255, 255, 255))
    white.putalpha(g)
    return white


# ---------------------------------------------------------------------------
# 槽位分配
# ---------------------------------------------------------------------------
def transcode_usable(b):
    """该字节能否安全用作字形槽位（与插件 transcode::IsUsableSlot 保持一致）。

    这些字节会被引擎的度量/绘制循环劫持：
        0x00..0x1F 静默跳过 / 0x23 '#' / 0x24 '$' / 0x26 '&'
        0x40 '@' 触发 8 字节 hex 颜色转义 / 0x7C '|' 状态切换
    """
    if b < 0x20 or b == 0x23 or b == 0x24 or b == 0x26 or b == 0x40 or b == 0x7C:
        return False
    return True


def assign_slots(chars):
    """分配槽位。**从高到低**取（0xFF..0x80），与烘图时「底部优先牺牲」的顺序一致，
    这样「被牺牲的槽位」与「汉字占用的槽位」是同一批，不会出现两套不一致的集合。
    """
    slots = [b for b in range(0xFF, 0x7F, -1) if transcode_usable(b)]
    mapping, unassigned = {}, []
    for ch in chars:
        if not slots:
            unassigned.append(ch); continue
        mapping[ch] = slots.pop(0)
    return mapping, unassigned


def collect_rows(item, W, H):
    """把某字号里所有可征用槽位按「像素行」分组。

    返回 [(y0, y1, [(x0, x1, slot), ...]), ...]，按 y **从底部往上**排序，
    行内按 x 从左到右。
    """
    rm = {}
    for v, abc, tcs, _raw in item['symbols']:
        vb = v.encode('latin-1', 'replace')
        if len(vb) != 1 or vb[0] < 0x80 or not transcode_usable(vb[0]):
            continue                      # 只征用 >=0x80 且安全的槽位（不动 ASCII）
        t = [float(x) for x in tcs.split()]
        if len(t) < 5:
            continue
        key = round(t[2] * H, 1)          # 用 v0 的像素 y 作为行标识
        e = rm.setdefault(key, {'y1': t[4] * H, 'items': []})
        e['items'].append((t[1] * W, t[3] * W, vb[0]))
        e['y1'] = max(e['y1'], t[4] * H)
    return [(y0, rm[y0]['y1'], sorted(rm[y0]['items']))
            for y0 in sorted(rm, reverse=True)]


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ws = os.path.dirname(HERE)
    ap.add_argument('--game', default=r'I:\LocalGames\Hard Truck Apocalypse STEAM')
    ap.add_argument('--src-fonts', default=None)
    ap.add_argument('--text-dir', action='append', default=None)
    ap.add_argument('--charset', default=None)
    ap.add_argument('--ttf', default=DEFAULT_TTF)
    ap.add_argument('--out', default=None)
    ap.add_argument('--only-size', default=None)
    ap.add_argument('--max-chars', type=int, default=0,
                    help='只取前 N 个汉字（调试用）')
    args = ap.parse_args()

    src_fonts = args.src_fonts or os.path.join(args.game, 'data', 'if', 'fonts', 'fonts.xml')
    out_root = args.out or os.path.join(ws, 'update_chs')
    fonts_dir = os.path.normpath(os.path.join(src_fonts, '..'))

    if not os.path.isfile(src_fonts):
        print('[错误] 找不到 %s' % src_fonts); return 1
    if not os.path.isfile(args.ttf):
        print('[错误] 找不到 %s' % args.ttf); return 1

    # ---- 收集字符 ----
    chars, seen = [], set()
    if args.charset:
        for ch in open(args.charset, encoding='utf-8').read():
            if ch.strip() and ch not in seen:
                seen.add(ch); chars.append(ch)
    else:
        dirs = args.text_dir or [os.path.join(ws, 'Original_DATA_CHS'),
                                 os.path.join(ws, 'DLC1_DATA_CHS')]
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for fp in glob.glob(os.path.join(d, '**', '*.xml'), recursive=True):
                rb = open(fp, 'rb').read()
                try:
                    txt = rb.decode(pick_encoding(rb), 'replace')
                except Exception:
                    continue
                for ch in txt:
                    o = ord(ch)
                    if (0x4E00 <= o <= 0x9FFF) or (0x3400 <= o <= 0x4DBF) \
                       or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF):
                        if ch not in seen:
                            seen.add(ch); chars.append(ch)
    if args.max_chars:
        chars = chars[:args.max_chars]

    print('=' * 66)
    print('字库构建')
    print('=' * 66)
    print('源 fonts.xml : %s' % src_fonts)
    print('输出目录     : %s' % out_root)
    print('需要字形字符 : %d' % len(chars))
    if not chars:
        print()
        print('[!] 没收集到汉字 —— 译文目录仍是原文，或未指定 --charset。')
        return 1

    mapping, unassigned = assign_slots(chars)
    print('已分配槽位   : %d 个 (0x80..0xFF 减禁区)' % len(mapping))
    if unassigned:
        print('[!] 超出单字号容量: %d 个未分配，显示为 ?' % len(unassigned))
    print()

    raw = open(src_fonts, 'rb').read()
    latin1 = raw.decode('latin-1')
    items = parse_items(latin1)
    print('字号数: %d' % len(items))

    os.makedirs(os.path.join(out_root, 'data', 'if', 'fonts'), exist_ok=True)
    processed = {}
    # ═══════════════════════════════════════════════════════════════════════
    # 第一步：确定**全局统一**的槽位分配（必须在所有字号上一致）
    #
    # 为什么必须全局统一（实测踩坑，勿改回去）：
    #   转码发生在 sub_406F50（String3d 赋值）的那一刻，那时**还不知道字号**，
    #   所以一个 GBK 汉字只能映射到唯一一个槽号。
    #   如果让各字号各自「从底部往上征用」，不同字号的行结构不同 -> 征用到的
    #   槽号集合也不同。而 slotmap 只能写一份，结果就是：只有其中一个字号对，
    #   其余字号拿这个槽号去查**另一个格子**，采到错误的像素区域
    #   —— 表现为字形被截断/错位（实机踩到过：下面约 3/8 被吞）。
    #
    # 做法：拿参考字号（第一个有图集的）算出「从底部往上、行内从左到右」的
    # 槽位顺序，作为全局分配顺序；所有字号都征用**这批同样的槽号**。
    # ═══════════════════════════════════════════════════════════════════════
    ref = next((x for x in items if x['file']), None)
    if ref is None:
        print('[错误] 没有任何带图集的字号'); return 1
    _ref_dds = os.path.join(fonts_dir,
                            os.path.basename(ref['file'].replace('\\', '/')))
    _t = decode_dds(_ref_dds)
    refW, refH = _t.size
    del _t

    slot_order = []
    for _y0, _y1, _cells in collect_rows(ref, refW, refH):
        for _x0, _x1, _s in _cells:
            slot_order.append(_s)

    mapping_final = {}
    for i, ch in enumerate(chars):
        if i >= len(slot_order):
            break
        mapping_final[ch] = slot_order[i]
    slot2ch = {sl: c for c, sl in mapping_final.items()}
    if len(mapping_final) < len(chars):
        print('[!] 全局可用槽位 %d 个 < 需要 %d 个汉字'
              % (len(mapping_final), len(chars)))
    print('全局槽位分配（%d 个汉字，所有字号共用）:' % len(mapping_final))
    print('  ' + ' '.join('%s=%02X' % (c, mapping_final[c])
                          for c in chars if c in mapping_final))
    print()

    for it in items:
        height = it['height']
        if args.only_size and ('%.3f' % height) != args.only_size:
            continue
        if not it['file']:
            continue
        fname = os.path.basename(it['file'].replace('\\', '/'))
        src_dds = os.path.join(fonts_dir, fname)
        if not os.path.isfile(src_dds):
            print('  [跳过] 缺图集 %s' % src_dds)
            continue

        em = height / 72.0 * DPI                 # 点阵高（em）
        cell = int(em + 0.5)                     # 汉字单元边长
        img = decode_dds(src_dds)
        W, H = img.size

        # ═══════════════════════════════════════════════════════════════════
        # 按「整行」征用，并把整行宽度按 cell 重新划分给**该行里已分配的槽位**。
        #
        # 为什么不能逐个槽位替换（实测踩坑）：
        #   原图集的西里尔槽位是按**字母墨迹宽度**紧凑排布的，单格只有 9~12px 宽、
        #   20px 高（宽高比约 0.45）。汉字是方块字，塞进 9px 宽的格子里只能缩成
        #   9x9 贴在格子中间 —— 画面上表现为「字小得只剩一点点、贴在最上边」。
        #   必须让汉字拿到 cell x cell 的**方正格子**。
        #
        # 注意：槽号来自全局分配 mapping_final，本字号只负责把这些槽号所在的
        # 像素行清空并重排成方块，**不重新分配槽号**。
        # ═══════════════════════════════════════════════════════════════════
        canvas = img.copy()
        d0 = ImageDraw.Draw(canvas)
        font = ImageFont.truetype(args.ttf, cell)
        rects = {}                       # ch -> 像素方块
        slot_of = {}                     # ch -> 槽位字节（= 全局分配）
        doomed = set()                   # 被牺牲的槽位（需从 fonts.xml 移除）

        for y0, y1, cells in collect_rows(it, W, H):
            in_row = [(x0, x1, s) for (x0, x1, s) in cells if s in slot2ch]
            if not in_row:
                continue
            # 该行整行作废（像素将被方块覆盖）
            for _x0, _x1, s in cells:
                doomed.add(s)
            gh = y1 - y0
            side = int(min(cell, gh))
            if side < 4:
                continue                 # 行太矮，放不下方块
            step = side + 1
            ncol = int(W // step)
            for j, (_sx0, _sx1, s) in enumerate(in_row):
                if j >= ncol:
                    break                # 该行放不下更多方块
                ch = slot2ch[s]
                x0 = j * step
                x1 = x0 + side
                d0.rectangle([x0, int(y0), x1 - 1, int(y1) - 1],
                             fill=(255, 255, 255, 0))
                g = render_cjk(ch, args.ttf, cell, max(1, int(side * CJK_FILL)))
                if g is None:
                    continue
                ox = x0 + (side - g.size[0]) // 2
                oy = int(y0) + (int(gh) - g.size[1]) // 2
                canvas.paste(g, (ox, oy), g)
                rects[ch] = (float(x0), float(y0), float(x1), float(y1))
                slot_of[ch] = s

        if len(slot_of) < len(chars):
            print('  [!] 字号 %.3f: 只放下 %d 个汉字（需要 %d）'
                  % (height, len(slot_of), len(chars)))

        out_dds = os.path.join(out_root, 'data', 'if', 'fonts', fname)
        encode_dds(canvas, out_dds)
        processed[('%.3f' % height)] = {'rects': rects, 'W': W, 'H': H,
                                        'oldH': H, 'cell': cell,
                                        'doomed': doomed, 'slot_of': slot_of}
        print('  字号 %-8.3f em=%5.1fpx 图集 %dx%d(不变)  烘入 %d 个汉字  %s'
              % (height, em, W, H, len(rects), fname))

    # ---- 生成 fonts.xml（latin-1 字节空间）----
    out = ['<?xml version="1.0" encoding="windows-1251" standalone="yes" ?>', '<Fonts>']
    for it in items:
        key = '%.3f' % it['height']
        info = processed.get(key)
        out.append('\t<Item')
        # header 原文以 '\n\t\t...' 开头、以 file 属性结尾（不含 '>'）。
        # 直接接上即可，末尾补 '>' 收尾。
        out.append(it['header'])
        out.append('>')

        if info is None:
            # 未处理的字号：原样输出（value 用原文，保留单引号/实体写法）
            for _v, abc, tcs, raw in it['symbols']:
                out.append('\t\t<Symbol'); out.append('\t\t\t%s' % raw)
                out.append('\t\t\tabc="%s"' % abc); out.append('\t\t\ttcs="%s" />' % tcs)
        else:
            W, H, oldH = info['W'], info['H'], info['oldH']
            rects = info['rects']
            slot_of = info['slot_of']
            doomed = info['doomed']       # 被牺牲的槽位
            assert H == oldH, '图集尺寸必须保持不变（见上文「为什么不能加高」）'
            # 图集尺寸不变 => 原有字形原样保留，不做任何缩放
            for v, abc, tcs, raw in it['symbols']:
                vb = v.encode('latin-1', 'replace')
                if len(vb) == 1 and vb[0] in doomed:
                    continue              # 该槽位让给汉字
                # ★ 必须回写 value 的**原文**：原文件用 value='"'（单引号）
                #   表示双引号字符，还用 &amp; 表示 &。重新用双引号包裹会
                #   产出非法 XML，引擎直接拒绝加载整个 fonts.xml。
                out.append('\t\t<Symbol'); out.append('\t\t\t%s' % raw)
                out.append('\t\t\tabc="%s"' % abc); out.append('\t\t\ttcs="%s" />' % tcs)
            # 汉字字形：等宽推进 = cell（UV 直接按原图集尺寸归一化）
            for ch, (x0, y0, x1, y1) in rects.items():
                slot = slot_of[ch]
                tcs = '0.000 %.4f %.4f %.4f %.4f' % (x0 / W, y0 / H, x1 / W, y1 / H)
                abc = '0.000 %.3f 0.000' % (x1 - x0)
                out.append('\t\t<Symbol'); out.append('\t\t\tvalue="%s"' % chr(slot))
                out.append('\t\t\tabc="%s"' % abc); out.append('\t\t\ttcs="%s" />' % tcs)
        out.append('\t</Item>')
    out.append('</Fonts>')

    out_fonts = os.path.join(out_root, 'data', 'if', 'fonts', 'fonts.xml')
    with open(out_fonts, 'wb') as f:
        f.write('\n'.join(out).encode('latin-1', 'replace'))
    print()
    print('已写出 fonts.xml: %s' % out_fonts)

    # ---- 槽位映射 ----
    map_path = os.path.join(out_root, 'hta_chs_slotmap.txt')
    n = 0
    with open(map_path, 'w', encoding='ascii') as f:
        f.write('# hta_chs_slotmap 1\n# GBK-hex slot-hex\n')
        for ch, slot in sorted(mapping_final.items(), key=lambda kv: kv[1]):
            gbk = ch.encode('gbk', 'replace')
            if len(gbk) != 2:
                continue
            f.write('%02X%02X %02X\n' % (gbk[0], gbk[1], slot))
            n += 1
    print('已写出槽位映射: %s (%d 条)' % (map_path, n))
    print()
    print('=' * 66)
    print('部署:')
    print('  1) %s\\data 覆盖到游戏 update/ 下' % out_root)
    print('  2) hta_chs_slotmap.txt 与 hta_chs.asi 放同一目录（游戏根目录）')
    print('=' * 66)
    return 0


if __name__ == '__main__':
    sys.exit(main())
