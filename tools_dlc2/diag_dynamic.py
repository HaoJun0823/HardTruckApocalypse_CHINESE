# -*- coding: utf-8 -*-
"""调查 DLC2 原件本身的 XML 合法性问题。

发现：data/if/diz/dynamicdialogsglobal.xml 原件第 269 行
  scriptCondition="GetPlayerMoney() < ConversationWnd:GetCurrentDynamicQuest():GetReward()"
属性值里有裸 '<'，这在任何 XML 解析器下都是非法的。游戏用的是自研解析器，
可能容忍它 —— 但我们的校验脚本用标准解析器就会报失败。

本脚本确认：这是原版就有的，还是汉化过程引入的。
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT


def check(path, label, enc):
    b = xlit.read_bytes(path)
    txt = b.decode(enc, errors='replace')
    try:
        ET.fromstring(b.decode('latin-1'))
        print('%-34s %s  解析 OK' % (label, os.path.basename(path)))
        return []
    except ET.ParseError as ex:
        line, col = ex.position
        lines = b.decode('latin-1').split('\n')
        ctx = lines[line - 1] if line - 1 < len(lines) else ''
        print('%-34s %s  解析失败 line %d col %d' % (label, os.path.basename(path), line, col))
        print('      该行: %s' % ctx.strip()[:120])
        return [(line, ctx)]


def main():
    rel = 'data/if/diz/dynamicdialogsglobal.xml'
    print('== 原件与三份快照对照 ==')
    check(os.path.join(ROOT, 'DLC2_DATA', *rel.split('/')), 'DLC2_DATA (原版)', 'cp1251')
    check(os.path.join(ROOT, '_DLC2_DATA_CHS_backup', *rel.split('/')), '_DLC2_DATA_CHS_backup', 'cp1251')
    for snap in ('_DLC2_corrupted_snapshot_20261008-182718',
                 '_DLC2_corrupted_snapshot_20261008-191126'):
        p = os.path.join(ROOT, snap, *rel.split('/'))
        if os.path.exists(p):
            check(p, snap, 'gbk')
    p = os.path.join(ROOT, 'DLC2_DATA_CHS', *rel.split('/'))
    if os.path.exists(p):
        check(p, 'DLC2_DATA_CHS (当前)', 'gbk')

    print('\n== 全文里所有含裸 < 或 & 的属性值 ==')
    src = os.path.join(ROOT, 'DLC2_DATA', *rel.split('/'))
    txt = xlit.read_bytes(src).decode('cp1251', errors='replace')
    import re
    for m in re.finditer(r'\s(\w+)="([^"<>]*(?:<[^"]*)?)"', txt):
        if '<' in m.group(2) or '&' in m.group(2):
            print('   %s = %s' % (m.group(1), m.group(2)))


if __name__ == '__main__':
    main()