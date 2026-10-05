# -*- coding: utf-8 -*-
"""
diag_sampling.py —— 诊断引擎的 UV 采样窗口

在已烘好的字库上做受控实验，一次截图就能看出引擎实际采样的 v 区间：

  * 槽 EC/ED/EE（对应主菜单第一行「新游戏」）
        -> 画 10 条水平白条纹铺满格子。数屏幕上剩几条，就知道采样了格子的几分之几。

  * 槽 EF/F0/F1/F2（对应主菜单第二行「读取存档」）
        -> 同一个「新」字，分别用 上对齐 / 居中 / 下对齐 / 满格 四种方式摆放。
           哪种能完整显示，就说明引擎的采样窗口在哪。

用法：
    python diag_sampling.py [--size 12.000] [--slotmap <dir>]
"""
import os
import re
import sys
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
TTF = os.path.join(HERE, 'SourceHanSansHWSC-VF.ttf')
PROBE = '新'


def decode_dds(p):
    td = tempfile.mkdtemp()
    try:
        subprocess.run([TEXCONV, p, '-f', 'R8G8B8A8_UNORM', '-ft', 'png',
                        '-o', td, '-y', '-nologo'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        png = next(os.path.join(td, f) for f in os.listdir(td) if f.lower().endswith('.png'))
        return Image.open(png).convert('RGBA')
    finally:
        shutil.rmtree(td, ignore_errors=True)


def encode_dds(img, p):
    p = os.path.abspath(p)
    td = tempfile.mkdtemp(dir=os.path.dirname(p))
    try:
        stem = os.path.splitext(os.path.basename(p))[0]
        png = os.path.join(td, stem + '.png')
        img.save(png, 'PNG')
        subprocess.run([TEXCONV, png, '-f', 'BC3_UNORM', '-ft', 'dds',
                        '-o', td, '-y', '-nologo'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.replace(os.path.join(td, stem + '.dds'), p)
    finally:
        shutil.rmtree(td, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=r'G:\Projects\HardTruckApocalypse_CHINESE\update_chs_test')
    ap.add_argument('--size', default='12.000')
    ap.add_argument('--game', default=r'I:\LocalGames\Hard Truck Apocalypse STEAM')
    args = ap.parse_args()

    fontdir = os.path.join(args.dir, 'data', 'if', 'fonts')
    xml = open(os.path.join(fontdir, 'fonts.xml'), 'rb').read().decode('latin-1')
    items = re.findall(r'<Item\b(.*?)</Item>', xml, re.S)
    it = None
    for x in items:
        h = re.search(r'height="([\d.]+)"', x)
        if h and h.group(1) == args.size:
            it = x; break
    if it is None:
        print('找不到字号 %s' % args.size); return 1

    fname = os.path.basename(re.search(r'file="([^"]*)"', it).group(1).replace('\\', '/'))
    dds = os.path.join(fontdir, fname)
    img = decode_dds(dds)
    W, H = img.size
    print('字号 %s  图集 %dx%d  %s' % (args.size, W, H, fname))

    # 收集槽位 -> 格子像素矩形
    cells = {}
    for sb in re.findall(r'<Symbol\b(.*?)/>', it, re.S):
        vm = re.search(r"value=(?:\"(.*?)\"(?=\s|\n|$)|'(.*?)')", sb, re.S)
        tm = re.search(r'tcs="([^"]*)"', sb)
        if not (vm and tm):
            continue
        v = vm.group(1) if vm.group(1) is not None else vm.group(2)
        vb = v.encode('latin-1', 'replace')
        if len(vb) != 1:
            continue
        t = [float(x) for x in tm.group(1).split()]
        if len(t) < 5:
            continue
        cells[vb[0]] = (t[1] * W, t[2] * H, t[3] * W, t[4] * H)

    slots = [0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2]
    missing = [s for s in slots if s not in cells]
    if missing:
        print('缺少槽位: %s' % [hex(s) for s in missing]); return 1

    d = ImageDraw.Draw(img)

    # ---- 前 3 个槽：水平条纹尺子（数条数 = 量采样比例）----
    for s in (0xEC, 0xED, 0xEE):
        x0, y0, x1, y1 = cells[s]
        d.rectangle([int(x0), int(y0), int(x1) - 1, int(y1) - 1], fill=(255, 255, 255, 0))
        h = y1 - y0
        N = 10
        for i in range(N):
            if i % 2 == 0:      # 偶数条填白 -> 共 5 条白纹
                a = y0 + h * i / N
                b = y0 + h * (i + 1) / N
                d.rectangle([int(x0) + 1, int(a), int(x1) - 2, int(b) - 1],
                            fill=(255, 255, 255, 255))

    # ---- 后 4 个槽：同一汉字，四种垂直摆放 ----
    # 判别逻辑：
    #   若「上对齐」完整而「居中」被切下边  -> 采样窗口比格子矮、且贴着上边
    #   若四种都被切同样多                  -> 切在四边形/裁剪层，与格子无关
    #   若「溢出下边」能看到多出来的部分    -> 采样窗口比声明的 v1 还往下
    modes = [('上对齐', 'top'), ('居中', 'center'), ('下对齐', 'bottom'), ('溢出下边', 'over')]
    for s, (name, mode) in zip((0xEF, 0xF0, 0xF1, 0xF2), modes):
        x0, y0, x1, y1 = cells[s]
        gw, gh = x1 - x0, y1 - y0
        d.rectangle([int(x0), int(y0), int(x1) - 1, int(y1) - 1], fill=(255, 255, 255, 0))
        side = int(min(gw, gh))
        box = max(1, int(side * 0.94))
        f = ImageFont.truetype(TTF, box)
        pad = 4
        cv = Image.new('L', (box + pad * 2, box + pad * 2), 0)
        ImageDraw.Draw(cv).text((pad, pad), PROBE, fill=255, font=f)
        bb = cv.getbbox()
        if bb:
            g = cv.crop(bb)
            white = Image.new('RGBA', g.size, (255, 255, 255, 255))
            white.putalpha(g)
            if mode == 'top':
                oy = int(y0)
            elif mode == 'bottom':
                oy = int(y1) - g.size[1]
            elif mode == 'over':
                # 故意让字形探出格子下边 8px，测试采样窗口是否越过 v1
                oy = int(y1) - g.size[1] + 8
            else:
                oy = int(y0) + (int(gh) - g.size[1]) // 2
            ox = int(x0) + (int(gw) - g.size[0]) // 2
            img.paste(white, (ox, oy), white)
            print('  槽 %02X %-8s 格 (%.0f,%.0f)-(%.0f,%.0f)  字 %dx%d  贴到 y=%d..%d'
                  % (s, name, x0, y0, x1, y1, g.size[0], g.size[1], oy, oy + g.size[1]))

    encode_dds(img, dds)
    print('已写回 %s' % dds)
    print()
    print('部署: 复制 %s\\data 到游戏 update\\ 下，启动看主菜单：' % args.dir)
    print('  第一行「新游戏」-> 会显示 3 个条纹尺子；数白条纹条数，5 条=完整采样')
    print('  第二行「读取存档」-> 4 个「新」字：上对齐 / 居中 / 下对齐 / 满格')
    return 0


if __name__ == '__main__':
    sys.exit(main())
