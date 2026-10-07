# -*- coding: utf-8 -*-
"""
diag_missing.py —— 诊断「译文里出现、但字库里没有」的汉字

对比对象：
  A. 译文目录里实际出现的汉字（用 build_cjk.py 同样的解码规则）
  B. hta_chs_cjk.bin 里已烘的字形码表
输出缺失清单（按 GBK 码排序），便于判断是不是 map 翻译新增的字。
"""
import os
import re
import sys
import glob
import struct
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_cjk import decode_text, cjk_count   # noqa: E402

WS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RANGES = ((0x4E00, 0x9FFF), (0x3400, 0x4DBF), (0x3000, 0x303F), (0xFF00, 0xFFEF))


def collect(dirs):
    chars, seen = [], set()
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fp in glob.glob(os.path.join(d, '**', '*.xml'), recursive=True):
            txt = decode_text(open(fp, 'rb').read())
            for ch in txt:
                o = ord(ch)
                if any(a <= o <= b for a, b in RANGES) and ch not in seen:
                    seen.add(ch)
                    chars.append(ch)
    return chars


def load_pkg(path):
    d = open(path, 'rb').read()
    magic, ver, nsz, ngly = struct.unpack_from('<IIII', d, 0)
    if magic != 0x4B4A4348:
        raise RuntimeError('bad magic %08X' % magic)
    codes = set(struct.unpack_from('<%dH' % ngly, d, 48))
    return codes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pkg', default=r'I:\LocalGames\Hard Truck Apocalypse STEAM\update\hta_chs_cjk.bin')
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--out', default=os.path.join(WS, 'fontgen', 'diag_missing.txt'))
    args = ap.parse_args()

    lines = []

    def P(s=''):
        lines.append(s)

    dirs = args.dir or [os.path.join(WS, 'Original_DATA_CHS'),
                        os.path.join(WS, 'DLC1_DATA_CHS')]
    chars = collect(dirs)
    P('扫描目录:')
    for d in dirs:
        P('  ' + d)
    P()
    P('译文字符(全范围) : %d' % len(chars))

    # 只有能编成 GBK 双字节的才可能进包
    usable = {}
    for ch in chars:
        try:
            b = ch.encode('gbk')
        except Exception:
            continue
        if len(b) == 2:
            usable[ch] = (b[0], b[1])
    P('可 GBK 双字节    : %d' % len(usable))

    if os.path.isfile(args.pkg):
        baked = load_pkg(args.pkg)
        P('包内已烘字形     : %d' % len(baked))
        missing = [(ch, usable[ch]) for ch in usable
                   if ((usable[ch][0] << 8) | usable[ch][1]) not in baked]
        missing.sort(key=lambda kv: (kv[1][0], kv[1][1]))
        P()
        P('★ 缺失 %d 个字' % len(missing))
        line = []
        for ch, (b1, b2) in missing:
            line.append('%s(%02X%02X)' % (ch, b1, b2))
        for i in range(0, len(line), 20):
            P('  ' + ' '.join(line[i:i + 20]))
    else:
        P('包文件不存在: %s' % args.pkg)

    ng = [ch for ch in chars if ch not in usable]
    P()
    P('非 GBK 双字节字符 : %d 个 -> %s' % (len(ng), ''.join(ng[:80])))

    with open(args.out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('written: %s' % args.out)


if __name__ == '__main__':
    main()