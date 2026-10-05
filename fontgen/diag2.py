# -*- coding: utf-8 -*-
"""
diag2.py —— 判定「底边采样」的受控实验（比条纹更直接）

条纹测法的缺陷：最底部那条本来就是透明带，量不出真实底边。
本脚本改用「整格填白」：

  槽 EC  整格填白（0 边距）        -> 若屏幕上是一个完整的白方块，说明整格都被采样；
                                      若下边缺一条，说明底边被切。
  槽 ED  整格填白，但底部 2 行透明  -> 对照：底边到底切了几行
  槽 EE  整格填白，但顶部 2 行透明  -> 确认切的是下边还是上边
  槽 EF  「新」铺满整格（不缩放、不居中）
  槽 F0  「新」0.94 居中（当前做法）
  槽 F1  「新」0.94，底边贴齐格子底部
  槽 F2  「新」0.94，整体上移 3px

一次截图即可判定：底边是否被切、切几行、以及字形在格子里该怎么摆。
"""
import os, re, sys, struct, argparse, subprocess, tempfile, shutil
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TEXCONV = os.path.join(HERE, 'texconv.exe')
TTF = os.path.join(HERE, 'SourceHanSansHWSC-VF.ttf')
CH = '新'


def decode_dds(p):
    td = tempfile.mkdtemp()
    try:
        subprocess.run([TEXCONV, p, '-f', 'R8G8B8A8_UNORM', '-ft', 'png', '-o', td,
                        '-y', '-nologo'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        f = next(os.path.join(td, x) for x in os.listdir(td) if x.lower().endswith('.png'))
        return Image.open(f).convert('RGBA')
    finally:
        shutil.rmtree(td, ignore_errors=True)


def encode_dds(img, p):
    p = os.path.abspath(p)
    td = tempfile.mkdtemp(dir=os.path.dirname(p))
    try:
        stem = os.path.splitext(os.path.basename(p))[0]
        png = os.path.join(td, stem + '.png')
        img.save(png, 'PNG')
        subprocess.run([TEXCONV, png, '-f', 'BC3_UNORM', '-ft', 'dds', '-o', td,
                        '-y', '-nologo'], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.replace(os.path.join(td, stem + '.dds'), p)
    finally:
        shutil.rmtree(td, ignore_errors=True)


def glyph_img(box, mode):
    pad = 4
    cv = Image.new('L', (box + pad * 2, box + pad * 2), 0)
    ImageDraw.Draw(cv).text((pad, pad), CH, fill=255, font=ImageFont.truetype(TTF, box))
    bb = cv.getbbox()
    if not bb:
        return None
    g = cv.crop(bb)
    w = Image.new('RGBA', g.size, (255, 255, 255, 255))
    w.putalpha(g)
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=r'G:\Projects\HardTruckApocalypse_CHINESE\update_chs_test')
    ap.add_argument('--size', required=True)
    args = ap.parse_args()

    fontdir = os.path.join(args.dir, 'data', 'if', 'fonts')
    xml = open(os.path.join(fontdir, 'fonts.xml'), 'rb').read().decode('latin-1')
    it = None
    for x in re.findall(r'<Item\b(.*?)</Item>', xml, re.S):
        h = re.search(r'height="([\d.]+)"', x)
        if h and h.group(1) == args.size:
            it = x; break
    if it is None:
        print('找不到字号 %s' % args.size); return 1

    fname = os.path.basename(re.search(r'file="([^"]*)"', it).group(1).replace('\\', '/'))
    dds = os.path.join(fontdir, fname)
    img = decode_dds(dds)
    W, H = img.size

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
        if len(t) >= 5:
            cells[vb[0]] = (t[1] * W, t[2] * H, t[3] * W, t[4] * H)

    need = [0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2]
    miss = [hex(s) for s in need if s not in cells]
    if miss:
        print('缺少槽位 %s' % miss); return 1

    d = ImageDraw.Draw(img)

    def clear(box):
        d.rectangle([int(box[0]), int(box[1]), int(box[2]) - 1, int(box[3]) - 1],
                    fill=(255, 255, 255, 0))

    def white(box):
        d.rectangle([int(box[0]), int(box[1]), int(box[2]) - 1, int(box[3]) - 1],
                    fill=(255, 255, 255, 255))

    # EC: 整格填白（0 边距）—— 判据：屏幕上应是完整白方块
    x0, y0, x1, y1 = cells[0xEC]; clear(cells[0xEC]); white(cells[0xEC])
    # ED: 整格填白，底部 2 行透明
    a = cells[0xED]; clear(a); white((a[0], a[1], a[2], a[3] - 2))
    # EE: 整格填白，顶部 2 行透明
    b = cells[0xEE]; clear(b); white((b[0], b[1] + 2, b[2], b[3]))

    # EF: 字形铺满整格（不缩放留边、不居中）
    c = cells[0xEF]; clear(c)
    side = int(min(c[2] - c[0], c[3] - c[1]))
    g = glyph_img(side, 'fill')
    if g:
        img.paste(g, (int(c[0]), int(c[1])), g)

    # F0: 0.94 居中（当前做法）
    e = cells[0xF0]; clear(e)
    gh = e[3] - e[1]
    g = glyph_img(max(1, int(side * 0.94)), 'c')
    if g:
        img.paste(g, (int(e[0]) + (side - g.size[0]) // 2,
                      int(e[1]) + (int(gh) - g.size[1]) // 2), g)

    # F1: 0.94，底边贴齐格子底部
    f = cells[0xF1]; clear(f)
    gh = f[3] - f[1]
    g = glyph_img(max(1, int(side * 0.94)), 'c')
    if g:
        img.paste(g, (int(f[0]) + (side - g.size[0]) // 2,
                      int(f[3]) - g.size[1]), g)

    # F2: 0.94，整体上移 3px
    k = cells[0xF2]; clear(k)
    gh = k[3] - k[1]
    g = glyph_img(max(1, int(side * 0.94)), 'c')
    if g:
        img.paste(g, (int(k[0]) + (side - g.size[0]) // 2,
                      int(k[1]) + (int(gh) - g.size[1]) // 2 - 3), g)

    for s, name in ((0xEC, '整格填白(0边距)'), (0xED, '填白·底2行透明'),
                    (0xEE, '填白·顶2行透明'), (0xEF, '新·铺满整格'),
                    (0xF0, '新·0.94居中'), (0xF1, '新·底边贴齐'),
                    (0xF2, '新·整体上移3px')):
        x0, y0, x1, y1 = cells[s]
        print('  槽 %02X %-16s 格 (%.0f,%.0f)-(%.0f,%.0f)  %dx%d'
              % (s, name, x0, y0, x1, y1, x1 - x0, y1 - y0))

    encode_dds(img, dds)
    print('已写回 %s' % dds)
    return 0


if __name__ == '__main__':
    sys.exit(main())
