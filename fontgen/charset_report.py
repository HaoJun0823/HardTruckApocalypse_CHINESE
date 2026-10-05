# -*- coding: utf-8 -*-
"""
charset_report.py —— 字库缺口报告：译文里有哪些字符「现有字体没有」

为什么不是简单统计汉字数：
  游戏的 fonts.xml 已经自带 224 个字形（拉丁 + 西里尔 + 若干标点）。
  其中包含 cp1251 的 '' ' ' " " … 等字符（例如字节 0x92 就是 U+2019 '，
  这是俄文排版的撇号，不是中文标点）。
  早先版本把这些误报成「需要新增槽位的中文标点」，并且因为「按候选编码
  依次尝试解码」而把 cp1251 字节按 GBK 解出假的汉字（如 抣/抯）——
  这是真实的分析错误。

正确做法：
  1. 编码以 XML 声明为准（encoding="windows-1251"/"gbk"/"utf-8"），
     不做「猜编码」的试探解码。
  2. 需要新增字形的字符 = 译文里出现的字符 − 现有 fonts.xml 已覆盖的字符。

用法：
  python charset_report.py
  python charset_report.py --text-dir DIR [--text-dir DIR ...]
  python charset_report.py --fonts <fonts.xml>
  python charset_report.py --write            # 写出 missing.txt
"""
import os
import re
import sys
import glob
import argparse

DECL_RE = re.compile(rb'encoding\s*=\s*["\']([^"\']+)["\']')

# 把 XML 声明的编码名映射到 Python 编码名
ENC_ALIAS = {
    'windows-1251': 'cp1251', 'cp1251': 'cp1251', '1251': 'cp1251',
    'windows-1252': 'cp1252', 'cp1252': 'cp1252',
    'gbk': 'gbk', 'gb2312': 'gbk', 'gb18030': 'gb18030', 'cp936': 'gbk',
    'utf-8': 'utf-8', 'utf8': 'utf-8',
    'koi8-r': 'koi8-r',
}


def read_text_decl_driven(path):
    """按 XML 声明解码；无声明则尝试 UTF-8，最后回退 cp1251。
    返回 (text, encoding)。"""
    with open(path, 'rb') as f:
        raw = f.read()
    m = DECL_RE.search(raw[:256])
    if m:
        decl = m.group(1).decode('ascii', 'ignore').lower().strip()
        enc = ENC_ALIAS.get(decl)
        if enc:
            try:
                return raw.decode(enc), enc
            except UnicodeDecodeError:
                return raw.decode(enc, 'replace'), enc + '(replace)'
    for enc in ('utf-8-sig', 'utf-8', 'cp1251'):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode('latin1', 'replace'), 'latin1'


def parse_font_symbols(fonts_xml):
    """读 fonts.xml，返回已覆盖字符集合（按声明编码解码）。

    只取第一个 <Item>（同为字体族时，各字号覆盖字符集一致）。"""
    if not os.path.isfile(fonts_xml):
        return set(), 'missing'
    with open(fonts_xml, 'rb') as f:
        raw = f.read()
    m = DECL_RE.search(raw[:256])
    enc = 'cp1251'
    if m:
        enc = ENC_ALIAS.get(m.group(1).decode('ascii', 'ignore').lower().strip(), 'cp1251')
    try:
        txt = raw.decode(enc, 'replace')
    except Exception:
        txt = raw.decode('latin1', 'replace')

    items = re.findall(r'<Item\b(.*?)</Item>', txt, re.S)
    scope = items[0] if items else txt
    covered = set()
    for vm in re.finditer(r'<Symbol\b[^>]*?value="((?:[^"\\]|\\.)*)"', scope, re.S):
        v = vm.group(1)
        # XML 实体还原
        v = (v.replace('&amp;', '&').replace('&lt;', '<')
              .replace('&gt;', '>').replace('&quot;', '"').replace('&apos;', "'"))
        for ch in v:
            covered.add(ch)
    return covered, enc


def main():
    ap = argparse.ArgumentParser()
    ws = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    game = r"I:\LocalGames\Hard Truck Apocalypse STEAM"
    ap.add_argument('--text-dir', action='append', default=None,
                    help='译文目录（可多次指定）')
    ap.add_argument('--fonts', default=None, help='fonts.xml 路径')
    ap.add_argument('--write', action='store_true', help='写出 missing.txt')
    args = ap.parse_args()

    dirs = args.text_dir or [
        os.path.join(ws, 'Original_DATA_CHS'),
        os.path.join(ws, 'DLC1_DATA_CHS'),
    ]
    fonts_xml = args.fonts or os.path.join(game, 'data', 'if', 'fonts', 'fonts.xml')

    print('=' * 64)
    print('字库缺口报告：译文里现有字体没有的字符')
    print('=' * 64)
    print('译文目录:')
    for d in dirs:
        print('   %s%s' % (d, '' if os.path.isdir(d) else '   [不存在]'))
    print('字体定义: %s%s' % (fonts_xml, '' if os.path.isfile(fonts_xml) else '   [不存在]'))
    print()

    covered, fenc = parse_font_symbols(fonts_xml)
    print('现有字体已覆盖字符: %d 个（fonts.xml 声明编码 %s）' % (len(covered), fenc))
    print()

    # 逐文件收集
    used = {}          # 字符 -> 出现文件数
    enc_stat = {}
    nfiles = 0
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fp in glob.glob(os.path.join(d, '**', '*.xml'), recursive=True):
            nfiles += 1
            txt, enc = read_text_decl_driven(fp)
            enc_stat[enc] = enc_stat.get(enc, 0) + 1
            for ch in set(txt):
                if ord(ch) < 0x20:
                    continue
                used[ch] = used.get(ch, 0) + 1

    print('扫描文件: %d 个' % nfiles)
    print('文件编码分布: %s' % ', '.join('%s=%d' % kv for kv in sorted(enc_stat.items())))
    print()

    # 缺口
    missing = sorted(set(used) - covered)
    covered_used = sorted(set(used) & covered)

    # 分类
    def is_han(c):
        o = ord(c)
        return (0x3400 <= o <= 0x4DBF) or (0x4E00 <= o <= 0x9FFF) or (0xF900 <= o <= 0xFAFF)
    han = [c for c in missing if is_han(c)]
    other = [c for c in missing if not is_han(c)]

    print('-' * 64)
    print('结果')
    print('-' * 64)
    print('  译文用到不同字符      : %d' % len(used))
    print('  其中现有字体已覆盖    : %d' % len(covered_used))
    print('  **需要新增字形的字符**: %d' % len(missing))
    print('     其中汉字            : %d' % len(han))
    print('     其他(标点/符号)     : %d' % len(other))
    print()

    if other:
        print('  非汉字缺口: %s' % ''.join(other))
        print('    码位: %s' % ' '.join('U+%04X' % ord(c) for c in other))
        print()

    if len(used) <= 1:
        print('  [!] 译文几乎为空。Original_DATA_CHS / DLC1_DATA_CHS 与原文逐字节相同，')
        print('      即**还没有真正的译文**。字库规模要等有译文后才能确定。')
    elif len(missing) == 0:
        print('  [OK] 无需新增任何字形。')
    else:
        print('  结论: 需为 %d 个字符新增字形。' % len(missing))
        print('        引擎字形表是 256 项单字节索引；现有已占 %d 项，' % len(covered))
        print('        剩余 %d 项。' % (256 - len(covered)))
        if len(missing) <= 256 - len(covered):
            print('        -> 可沿用单字节槽位方案（路径 C）。')
        else:
            print('        -> 超出单字节容量，必须走路径 D（自建表 + 自建图集页）。')

    if args.write:
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'missing.txt')
        with open(out, 'w', encoding='utf-8') as f:
            f.write(''.join(missing))
        print()
        print('已写出: %s (%d 字符)' % (out, len(missing)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
