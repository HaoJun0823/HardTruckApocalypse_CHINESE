# -*- coding: utf-8 -*-
"""导出待人工翻译的条目，供逐条翻译后写回 manual.json。

用法：
  python todo.py --file data/if/strings/loadtips.xml        # 列出该文件待翻条目
  python todo.py --file ... --format json                    # 机器可读
  python todo.py --pending                                   # 汇总所有文件的缺口
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit
from apply import FILES, Applier, fmt_specs, skip_translation


def pending_for(rel, applier):
    cfg = FILES[rel]
    base = os.path.basename(rel)
    doc = xlit.Doc(rel)
    loc = doc.elements(cfg['tag'])
    rows = []
    for (s, e, el) in loc:
        key = el.get(cfg['key'])
        if key is None:
            continue
        for attr in cfg['attrs']:
            if not el.has(attr):
                continue
            cur = xlit.unesc(el.get(attr))
            if not cur.strip():
                continue
            if skip_translation(base, key, cur):
                continue
            cn, why = applier.lookup(base, key, attr, cur)
            if cn:
                continue
            rows.append({'key': key, 'attr': attr, 'ru': cur, 'why': why,
                         'fmt': fmt_specs(cur)})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file')
    ap.add_argument('--pending', action='store_true')
    ap.add_argument('--format', default='text')
    ap.add_argument('--wrap', type=int, default=0, help='每行后按 N 字符换行')
    a = ap.parse_args()

    applier = Applier()
    targets = sorted(FILES) if a.pending else [a.file]

    if a.pending and a.format == 'text':
        print('%-42s %6s %6s' % ('file', '元素', '待翻'))
        T = P = 0
        for rel in targets:
            rows = pending_for(rel, applier)
            n_el = len(xlit.Doc(rel).elements(FILES[rel]['tag']))
            T += n_el
            P += len(rows)
            print('%-42s %6d %6d' % (rel, n_el, len(rows)))
        print('-' * 58)
        print('%-42s %6d %6d' % ('合计', T, P))
        return

    rel = a.file
    rows = pending_for(rel, applier)
    if a.format == 'json':
        print(json.dumps(rows, ensure_ascii=False, indent=1))
        return

    base = os.path.basename(rel)
    print('### %s  待翻 %d 条\n' % (rel, len(rows)))
    for i, r in enumerate(rows, 1):
        fmt = ('  {%s}' % ','.join(r['fmt'])) if r['fmt'] else ''
        print('[%s] %s.%s%s' % (i, base, r['key'], fmt))
        print('    %s' % r['ru'])
        print()


if __name__ == '__main__':
    main()