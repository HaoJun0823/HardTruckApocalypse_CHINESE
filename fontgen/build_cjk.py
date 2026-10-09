# -*- coding: utf-8 -*-
"""
build_cjk.py —— 路径 D 的配套字库生成器

产出（放到 update\\ 下）：
    data/if/fonts/fonts.xml       原版 fonts.xml + 追加的 CJK 图集 Item
    data/if/fonts/cjk_<si>_<pi>.dds   CJK 图集页（每字号若干页）
    hta_chs_cjk.bin                字形包（DLL 读它来填 16 位索引表）

设计要点（与 HardTruckApocalypse_CHINESE_DLL/pathd.cpp 严格对应）：

1. **引擎的字形表被拓宽到 16 位**，索引 = 内存里读到的 16 位小端字：
       ASCII 单字节 c        -> 索引 = c
       GBK 双字节 b1 b2      -> 索引 = b1 | (b2<<8)
   所以包文件里的「码」存 (b1<<8)|b2（便于人读），DLL 负责交换字节得到索引。

2. **图集页不用 DLL 建纹理**，而是做成**额外的 fonts.xml Item**，
   让引擎自己加载。这样完全不用猜 IRenderer 的 createTexture/uploadPixels
   签名和像素格式 —— 风险最低。
   Item 的 height 用 900 编码：  height = 900 + 字号序号*10 + 页序号
   （真实字号是 7.8~18.75，永远不会请求 900+，所以不会抢到这些字体）

3. 页尺寸固定 512x256（引擎已知能加载的尺寸之一）。
   每字号按自己的 em 算格数：cols = 512//cell, rows = 256//cell。

4. `value` 属性沿用原文件的写法（含单引号/实体）——
   重写会产出非法 XML，引擎会拒绝加载整个 fonts.xml。

用法：
    python build_cjk.py --charset <字符集文件> --out <输出目录>
    python build_cjk.py --text-dir <译文目录> --out <输出目录>
"""
import os
import re
import sys
import glob
import shlex
import struct
import argparse
import subprocess
import tempfile
import shutil

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print('需要 Pillow'); sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(HERE, 'texconv.exe')
DEFAULT_TTF = os.path.join(HERE, 'SourceHanSansHWSC-VF.ttf')


def _wine_path(p):
    """wine 模式下，把 Unix 绝对路径转成 wine 的 `Z:` 盘符 + 反斜杠形式。

    ★ 为什么 ★
      texconv.exe 是 Windows 程序，在 wine 下会把**以 `/` 开头的 Unix 绝对路径**
      当成命令行选项开关（如 `Unknown option: home/runner/...`，开头 `/` 被吞），
      而不是当成输入/输出文件。换成 `Z:/abs/path`（wine 把宿主文件系统挂到
      Z: 盘）它才认得是文件。仅对绝对路径做映射；选项/格式名（如
      `-f R8G8B8A8_UNORM`、`-y`、`-nologo`）与相对路径保持原样。
    """
    return 'Z:' + os.path.abspath(p).replace('/', '\\')


def texconv_cmd(*args):
    """返回调用 texconv 的完整命令（list）。

    ★ 为什么要有这个 ★
      本仓库里 `texconv.exe` 是 **Windows 程序**（imports MSVCP140 / VCOMP140），
      本机与 Windows CI 直接执行即可。而 ubuntu-latest 上要跑同一份二进制，
      必须套一层 wine。为此提供环境变量 **`HTA_TEXCONV`**：给出命令前缀
      （例：`wine /abs/path/fontgen/texconv.exe`），本函数把它前置到参数前。

      不设该变量时行为与历史版本**逐字节一致**（就是 [TEXCONV] + args），
      因此本机既有的烘焙产物不受影响。

      ★ 设了该变量（wine 模式）时，参数里的绝对路径会被映射成 wine 的 `Z:`
        形式——否则 wine 下的 Windows 程序会把 Unix 绝对路径当选项开关。
    """
    override = os.environ.get('HTA_TEXCONV', '').strip()
    if override:
        mapped = [_wine_path(a) if os.path.isabs(a) else a for a in args]
        return shlex.split(override) + mapped
    return [TEXCONV] + list(args)

PAGE_W, PAGE_H = 512, 256      # 引擎已知能加载的图集尺寸
# height 编码：height = CJK_HEIGHT_BASE + sizeIndex*CJK_HEIGHT_STEP + pageIndex
#
# ★ 步长必须 >= 单字号最大页数，否则不同字号会撞车 ★
#   2330 字时 18.750 号要 2330/128 = 19 页，曾经的步长 10 会让
#   si=0/pi=10 与 si=1/pi=0 都编码成 910 —— 两个字号抢同一张图集。
#   步长取 50 > 19 有余量；基准 500 保证 si=9/pi=19 -> 969 < 1000，
#   仍落在实测可正常加载的区间内（避开 >=1000 是否被引擎接受的未知风险）。
CJK_HEIGHT_BASE = 500          # height 编码基准
CJK_HEIGHT_STEP = 50           # 每个字号的编码步长（> 最大页数）
DPI = 120.0
CJK_FILL = 0.94

DECL_RE = re.compile(rb'encoding\s*=\s*["\']([^"\']+)["\']')
ENC_ALIAS = {'windows-1251': 'cp1251', 'cp1251': 'cp1251',
             'windows-1252': 'cp1252', 'gbk': 'gbk', 'gb2312': 'gbk',
             'utf-8': 'utf-8', 'utf8': 'utf-8'}

CJK_LO, CJK_HI = 0x4E00, 0x9FFF


def cjk_count(text):
    return sum(1 for ch in text if CJK_LO <= ord(ch) <= CJK_HI)


def pick_encoding(raw):
    m = DECL_RE.search(raw[:256])
    if m:
        return ENC_ALIAS.get(m.group(1).decode('ascii', 'ignore').lower(), 'cp1251')
    return 'cp1251'


def decode_text(raw):
    """解码译文 XML —— **声明的编码不可信**。

    ★ 这是当初只烘出 300 个汉字、菜单全空白的真正原因 ★
      汉化译文文件的 XML 声明原样保留 `encoding="windows-1251"`，
      但字节实际是 **GBK**（汉化补丁的通行做法：只换字节不改声明）。
      按声明解成 cp1251 -> 一片西里尔字母 -> 一个汉字都提不出来，
      于是只能靠 --charset test300.txt 凑 300 字，
      而「新游戏 / 设置 / 退出」全不在那 300 字里 -> 查表 NULL -> 按钮空白。

    判定规则：按声明解；若解不出汉字，再试 gbk / utf-8，取汉字最多者。
      - 真中文文件：声明解出 0 -> gbk 解出上千 -> 选 gbk   ✅
      - 真俄文残留：三者都接近 0 -> 取声明（cp1251）     ✅ 不误伤
    """
    decl = pick_encoding(raw)
    try:
        t = raw.decode(decl, 'replace')
    except Exception:
        t = ''
    if cjk_count(t) > 0:
        return t
    best, bestN = t, cjk_count(t)
    for e in ('gbk', 'utf-8'):
        if e == decl:
            continue
        try:
            c = raw.decode(e, 'replace')
        except Exception:
            continue
        n = cjk_count(c)
        if n > bestN:
            best, bestN = c, n
    return best


def decode_dds(p):
    # ★ 不用 tempfile.mkdtemp()：它内部走 os.mkdir(path, 0o700)，而 Windows 上的
    #   Python 会因此写入一个**显式限制性 DACL**，本机 DSH 沙箱用户不在其中 ——
    #   目录能建出来，但往里写文件必然 PermissionError(13)。
    #   改用普通子目录（os.makedirs 用默认 mode，不设 DACL），语义等价：同处一地、用完即删。
    td = os.path.join(tempfile.gettempdir(), 'hta_cjk_decode_tmp')
    shutil.rmtree(td, ignore_errors=True)
    os.makedirs(td, exist_ok=True)
    try:
        subprocess.run(texconv_cmd(p, '-f', 'R8G8B8A8_UNORM', '-ft', 'png', '-o', td,
                                   '-y', '-nologo'), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        f = next(os.path.join(td, x) for x in os.listdir(td) if x.lower().endswith('.png'))
        return Image.open(f).convert('RGBA')
    finally:
        shutil.rmtree(td, ignore_errors=True)


def encode_dds(img, p):
    p = os.path.abspath(p)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    # ★ 同上：mkdtemp 的 0o700 -> 显式 DACL -> 沙箱下不可写。
    td = os.path.join(os.path.dirname(p), '_tmp_dds_encode')
    shutil.rmtree(td, ignore_errors=True)
    os.makedirs(td, exist_ok=True)
    try:
        stem = os.path.splitext(os.path.basename(p))[0]
        # ★★ 输入用 Pillow 写的**未压缩 RGBA8 DDS**，而不是 PNG ★★
        #   texconv 读 PNG 走 Windows 自带的 WIC 编解码器；在 wine 下 WIC 的
        #   可用性随发行版/版本而变（历史上多次出现「能跑但读不了 PNG」），
        #   是 CI 上最脆的一环。未压缩 DDS 由 texconv 自己解析，不依赖 WIC。
        #
        #   等价性已实测（本机 10 张真实 cjk 图集页）：
        #     PNG  -> BC3  与  DDS(RGBA8) -> BC3   产物**逐字节相同**。
        #   所以这不改变任何像素结果，只是把输入容器换成更稳的一种。
        raw = os.path.join(td, stem + '_raw.dds')
        img.save(raw, format='DDS')
        subprocess.run(texconv_cmd(raw, '-f', 'BC3_UNORM', '-ft', 'dds', '-o', td,
                                   '-y', '-nologo'), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.replace(os.path.join(td, stem + '_raw.dds'), p)
    finally:
        shutil.rmtree(td, ignore_errors=True)


def render_cjk(ch, ttf, em, box):
    """按**统一系数** k=box/em 缩放（保持字与字之间的相对大小）。

    ⚠ 画布必须按字体真实度量给足高度，否则 PIL 会静默裁掉字形底部
      （Source Han Sans 在 size=N 时 ascent+descent ≈ 1.47N）。
    """
    em = int(round(em))
    if em < 4:
        return None
    font = ImageFont.truetype(ttf, em)
    asc, desc = font.getmetrics()
    pad = 6
    W, H = em + pad * 2, asc + desc + pad * 2
    cv = Image.new('L', (W, H), 0)
    ImageDraw.Draw(cv).text((pad, pad), ch, fill=255, font=font)
    bb = cv.getbbox()
    if not bb:
        return None
    x0, y0, x1, y1 = bb
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return None
    k = box / float(em)                      # ★ 统一系数
    nw, nh = max(1, int(round(w * k))), max(1, int(round(h * k)))
    g = cv.crop(bb).resize((nw, nh), Image.LANCZOS)
    white = Image.new('RGBA', (nw, nh), (255, 255, 255, 255))
    white.putalpha(g)
    return white


def parse_items(latin1):
    """在 latin-1 字节空间解析 fonts.xml，保留 value 原文。"""
    items = []
    for blk in re.findall(r'<Item\b(.*?)</Item>', latin1, re.S):
        he = blk.find('>')
        hdr, body = blk[:he], blk[he:]
        ht = re.search(r'height="([^"]*)"', hdr)
        fl = re.search(r'file="([^"]*)"', hdr)
        syms = []
        for sb in re.findall(r'<Symbol\b(.*?)/>', body, re.S):
            vm = re.search(r'value=(?:"(.*?)"(?=\s|\n|$)|\'(.*?)\')', sb, re.S)
            am = re.search(r'abc="([^"]*)"', sb)
            tm = re.search(r'tcs="([^"]*)"', sb)
            if not (vm and am and tm):
                continue
            syms.append((vm.group(0), am.group(1), tm.group(1)))
        items.append({'height': float(ht.group(1)) if ht else 0.0,
                      'file': fl.group(1) if fl else '',
                      'header': hdr, 'symbols': syms})
    return items


def normalize_eol(raw):
    """把源 fonts.xml 统一成 **CRLF** 行尾后返回。

    ★ 为什么必须做这一步（CI 可复现性的关键）★
      本函数的下游（parse_items → main 的 '\n'.join）是按**二进制**读源、
      再按行拼写产物的：`it['header']` 里保留着源文件里的 `\\r\\n`，所以
      **产物的换行分布继承自源文件**。

      而 git 里同一个 fonts.xml 在不同工作区可能是不同字节：
        · 本机 core.autocrlf=true      → 工作区 **CRLF**（221821 B）
        · ubuntu-latest 的 checkout
          （actions/checkout 默认 autocrlf=false）→ **LF**（210559 B）
      实测两者烘出的产物：
        CRLF 源 → 1980514 B；LF 源 → 1980134 B（差 380 个 CRLF）。
      即「CI 产物 ≠ 本机验证过的产物」，这正是要避免的。

      统一到 CRLF 之后，烘焙结果**与输入行尾无关**：
        · 本机 CRLF 源   → 规范化不变 → 与历史产物逐字节相同（已实测）
        · CI 的 LF 源    → 规范化补回 CRLF → 同样得到那份产物
      这样就不必依赖 .gitattributes/core.autocrlf 的配置，也不会因为
      谁在哪个平台上 checkout 而改变发布包字节。

    ★ 为什么选 CRLF 而不是 LF ★
      发布包里的参考产物是 CRLF 源烘出来的；选 CRLF 才能与已实机验证、
      已发布的那一版**逐字节一致**（DLC1/DLC2 实测 100% 相同）。
    """
    # 先全部折成 LF（同时干掉可能存在的孤立 CR），再无差别地展开成 CRLF
    lf = raw.replace(b'\r\n', b'\n').replace(b'\r', b'\n')
    return lf.replace(b'\n', b'\r\n')


def main():
    ap = argparse.ArgumentParser()
    ws = os.path.dirname(HERE)
    ap.add_argument('--game', default=r'I:\LocalGames\Hard Truck Apocalypse STEAM')
    ap.add_argument('--src-fonts', default=None)
    ap.add_argument('--charset', default=None)
    ap.add_argument('--text-dir', action='append', default=None)
    ap.add_argument('--ttf', default=DEFAULT_TTF)
    ap.add_argument('--out', default=None)
    ap.add_argument('--max-chars', type=int, default=0)
    args = ap.parse_args()

    src = args.src_fonts or os.path.join(args.game, 'data', 'if', 'fonts', 'fonts.xml')
    out = args.out or os.path.join(ws, 'update_pathd')

    if not os.path.isfile(src):
        print('[错误] 找不到 %s' % src); return 1
    if not os.path.isfile(args.ttf):
        print('[错误] 找不到字体 %s' % args.ttf); return 1

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
                txt = decode_text(rb)     # ★ 声明不可信，见 decode_text 注释
                for ch in txt:
                    o = ord(ch)
                    if (0x4E00 <= o <= 0x9FFF) or (0x3400 <= o <= 0x4DBF) \
                       or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF):
                        if ch not in seen:
                            seen.add(ch); chars.append(ch)
    if args.max_chars:
        if len(chars) > args.max_chars:
            print('[!] --max-chars=%d 会截断 %d 个汉字（调试用，正式构建不要加）'
                  % (args.max_chars, len(chars) - args.max_chars))
        chars = chars[:args.max_chars]

    # 只保留能被 GBK 编码成双字节的
    gbk_of = {}
    for ch in chars:
        try:
            b = ch.encode('gbk')
        except Exception:
            continue
        if len(b) == 2:
            gbk_of[ch] = (b[0], b[1])
    chars = [c for c in chars if c in gbk_of]

    print('=' * 66)
    print('路径 D 字库构建')
    print('=' * 66)
    print('源 fonts.xml : %s' % src)
    print('输出目录     : %s' % out)
    print('汉字数       : %d' % len(chars))
    if not chars:
        print('[!] 没有汉字可烘 —— 请用 --charset 指定，或先准备译文')
        return 1

    raw = normalize_eol(open(src, 'rb').read())
    latin1 = raw.decode('latin-1')
    items = parse_items(latin1)
    # ★ 幂等性：剔掉源文件里**上一次生成时自己追加的** CJK Item ★
    #   否则重复运行会把 height=900/910/... 这些页字体当真实字号去烘，
    #   白白跳过一堆，还会把过时的 cjk_N_M.dds 引用原样复制进输出，
    #   而 .dds 文件名已按新页数改名 -> 引擎加载到不存在的图集。
    stale = [it for it in items if it['height'] >= CJK_HEIGHT_BASE]
    if stale:
        items = [it for it in items if it['height'] < CJK_HEIGHT_BASE]
        print('已剔除源文件里 %d 个过时的 CJK Item（幂等处理）' % len(stale))
    print('原字号数     : %d' % len(items))
    if not items:
        print('[错误] 源 fonts.xml 里没有真实字号，是不是把产出当输入了？')
        return 1
    print()

    fontdir = os.path.join(out, 'data', 'if', 'fonts')
    os.makedirs(fontdir, exist_ok=True)

    # 追加 CJK Item 到 fonts.xml；同时记录每个字号的页/格位
    cjk_items_xml = []
    size_records = []          # 每个字号：dict(height, pageCount, cols, cellsPerPage, cellW, cellH, cell[])
    for si, it in enumerate(items):
        height = it['height']
        em = height / 72.0 * DPI
        cell = int(em + 0.5)
        if cell < 4:
            print('  [跳过] 字号 %.3f 的 em=%.1f 太小' % (height, em))
            continue
        cols = PAGE_W // cell
        rows = PAGE_H // cell
        per_page = cols * rows
        if per_page <= 0:
            print('  [跳过] 字号 %.3f 单元 %d 放不进 %dx%d' % (height, cell, PAGE_W, PAGE_H))
            continue
        n_pages = (len(chars) + per_page - 1) // per_page
        if n_pages > CJK_HEIGHT_STEP:
            print('  [跳过] 字号 %.3f 需要 %d 页 > 步长 %d，编码会撞车'
                  % (height, n_pages, CJK_HEIGHT_STEP))
            continue
        sidx = len(size_records)     # ★ 用「已记录数」当序号，避免跳过时错位

        cells = []
        page_imgs = [Image.new('RGBA', (PAGE_W, PAGE_H), (0, 0, 0, 0)) for _ in range(n_pages)]
        for i, ch in enumerate(chars):
            pi = i // per_page
            pos = i % per_page
            col, row = pos % cols, pos // cols
            cx, cy = col * cell, row * cell
            g = render_cjk(ch, args.ttf, em, max(1, int(cell * CJK_FILL)))
            if g is not None:
                ox = cx + (cell - g.size[0]) // 2
                oy = cy + (cell - g.size[1]) // 2
                page_imgs[pi].paste(g, (ox, oy), g)
            cells.append(i)          # 格位就是顺序下标

        # 写页 + 生成对应 Item
        for pi in range(n_pages):
            name = 'cjk_%d_%d.dds' % (sidx, pi)
            encode_dds(page_imgs[pi], os.path.join(fontdir, name))
            enc_h = CJK_HEIGHT_BASE + sidx * CJK_HEIGHT_STEP + pi
            # Item 头：沿用原 Item 的 file 属性风格，只换名字和 height
            hdr = it['header']
            hdr = re.sub(r'height="[^"]*"', 'height="%d.000"' % enc_h, hdr)
            hdr = re.sub(r'heightVirtual="[^"]*"', 'heightVirtual="%d.000"' % enc_h, hdr)
            hdr = re.sub(r'file="[^"]*"', 'file="data\\\\if\\\\fonts\\\\%s"' % name, hdr)
            cjk_items_xml.append((hdr, it['symbols']))
        size_records.append({'height': height, 'pageCount': n_pages, 'cols': cols,
                             'cellsPerPage': per_page, 'cellW': cell, 'cellH': cell,
                             'cell': cells})
        print('  字号 %-8.3f em=%5.1fpx 单元 %2d  每页 %4d 格 (%dx%d)  %d 页  %d 字'
              % (height, em, cell, per_page, cols, rows, n_pages, len(chars)))

    # ---- 写 fonts.xml（原内容 + 追加 CJK Item）----
    out_lines = ['<?xml version="1.0" encoding="windows-1251" standalone="yes" ?>', '<Fonts>']
    for it in items:
        out_lines.append('\t<Item')
        out_lines.append(it['header'])
        out_lines.append('>')
        for raw_v, abc, tcs in it['symbols']:
            out_lines.append('\t\t<Symbol'); out_lines.append('\t\t\t%s' % raw_v)
            out_lines.append('\t\t\tabc="%s"' % abc); out_lines.append('\t\t\ttcs="%s" />' % tcs)
        out_lines.append('\t</Item>')
    for hdr, syms in cjk_items_xml:
        out_lines.append('\t<Item')
        out_lines.append(hdr)
        out_lines.append('>')
        # CJK Item 的 Symbol 只是为了让解析器满意；UV 不会被用到
        for raw_v, abc, tcs in syms:
            out_lines.append('\t\t<Symbol'); out_lines.append('\t\t\t%s' % raw_v)
            out_lines.append('\t\t\tabc="%s"' % abc); out_lines.append('\t\t\ttcs="%s" />' % tcs)
        out_lines.append('\t</Item>')
    out_lines.append('</Fonts>')
    fonts_out = os.path.join(fontdir, 'fonts.xml')
    with open(fonts_out, 'wb') as f:
        f.write('\n'.join(out_lines).encode('latin-1', 'replace'))
    print()
    print('已写出 fonts.xml (%d 个 Item，其中 CJK %d 个): %s'
          % (len(items) + len(cjk_items_xml), len(cjk_items_xml), fonts_out))

    # ---- 写包文件 ----
    pkg = os.path.join(out, 'hta_chs_cjk.bin')
    codes = [(gbk_of[c][0] << 8) | gbk_of[c][1] for c in chars]   # 存 (b1<<8)|b2
    with open(pkg, 'wb') as f:
        f.write(struct.pack('<IIII', 0x4B4A4348, 1, len(size_records), len(chars)))
        f.write(struct.pack('<II', PAGE_W, PAGE_H))
        f.write(b'\0' * 24)                                       # reserved[6]  (头共 48 字节)
        f.write(struct.pack('<%dH' % len(codes), *codes))
        for sr in size_records:
            f.write(struct.pack('<fIIIIIII', sr['height'], sr['pageCount'], sr['cols'],
                                sr['cellsPerPage'], sr['cellW'], sr['cellH'], 0, 0))
            f.write(struct.pack('<%dI' % len(sr['cell']), *sr['cell']))
    print('已写出包文件: %s (%d 字节)' % (pkg, os.path.getsize(pkg)))
    print()
    print('=' * 66)
    print('部署：')
    print('  1) %s\\data 覆盖到游戏 update\\ 下' % out)
    print('  2) %s\\hta_chs_cjk.bin 放到游戏 update\\ 下' % out)
    print('  3) hta_chs.asi 放游戏 update\\ 下（asi 在 update 里视同根目录）')
    print('=' * 66)
    return 0


if __name__ == '__main__':
    sys.exit(main())
