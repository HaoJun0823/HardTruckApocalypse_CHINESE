# -*- coding: utf-8 -*-
"""确认 DLC1 与 DLC2 的原文语言，以及既有译文对应的是哪一条原文。

用途：判断"按 id 复用"是否安全 —— 若 DLC1 原件是英文、DLC2 原件是俄文，
那么两边 id 相同但原文不同，复用等于拿别的文本的译文来用，必须逐条过目。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT
PROBE_IDS = ('collectionbook1', 'collectionbook5', 'r2m1_scout02')


def show(root, rel, label, enc):
    p = os.path.join(ROOT, root, rel)
    if not os.path.exists(p):
        print('%s: 缺失' % label)
        return
    text = xlit.read_bytes(p).decode(enc, errors='replace')
    got = {}
    for el in xlit.find_elements(text, 'Item'):
        k = el.get('id')
        if k in PROBE_IDS:
            got[k] = el.get('value')
    for k in PROBE_IDS:
        print('  %-14s %-9s %s' % (label, k, got.get(k, '<无>')))


def main():
    rel = 'data/if/diz/model_names.xml'
    print('== %s ==' % rel)
    show('DLC1_DATA', rel, 'DLC1原件', 'cp1251')
    show('DLC2_DATA', rel, 'DLC2原件', 'cp1251')
    show('DLC1_DATA_CHS', rel, 'DLC1汉化', 'gbk')


if __name__ == '__main__':
    main()