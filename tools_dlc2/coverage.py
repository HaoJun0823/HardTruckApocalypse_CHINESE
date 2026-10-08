# -*- coding: utf-8 -*-
"""报告 DLC2 各文件里有多少条目能按 id 命中既有译文（复用覆盖率）。

只做统计，不写任何文件。
"""
import json
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT

# DLC2 里需要处理的文件 -> (元素标签, key 属性, 文本属性列表)
FILES = {
    'data/if/strings/setupstrings.xml':     ('string', 'id', ['value']),
    'data/if/strings/msgpatterns.xml':      ('string', 'id', ['value']),
    'data/if/strings/gamestrings.xml':      ('string', 'id', ['value']),
    'data/if/strings/bindnames.xml':        ('string', 'id', ['value']),
    'data/if/strings/fadingmsgs.xml':       ('string', 'id', ['value']),
    'data/if/strings/loadtips.xml':         ('string', 'id', ['value']),
    'data/if/strings/uidescription.xml':    ('string', 'id', ['value']),
    'data/if/strings/uieditorstrings.xml':  ('string', 'id', ['value']),
    'data/if/strings/uieditstrings.xml':    ('string', 'id', ['value']),
    'data/if/strings/help.xml':             ('string', 'id', ['value']),
    'data/if/diz/questinfoglobal.xml':      ('QuestInfo', 'questName', ['briefDiz', 'fullDiz']),
    'data/if/diz/dynamicdialogsglobal.xml': ('string', 'id', ['value']),
    'data/if/diz/model_names.xml':          ('Item', 'name', ['diz']),
    'data/if/levelinfo/levelinfo.xml':      ('LevelInfo', 'name', ['fullName', 'diz0', 'diz1']),
    'data/if/levelinfo/checkpointinfo.xml': ('CheckpointInfo', 'name', ['diz', 'fullDiz']),
}
for n in (1, 2, 4, 5, 6, 7, 8, 9):
    FILES['data/maps/dv%d/strings.xml' % n] = ('string', 'id', ['value'])

ID_ATTRS_FALLBACK = ['id', 'name', 'questName']


def load_pool():
    with open(os.path.join(ROOT, 'tools_dlc2', 'reuse_pool.json'), encoding='utf-8') as f:
        p = json.load(f)
    dlc1 = {tuple(k.split('|', 1)): v for k, v in p['dlc1'].items()}
    orig = {tuple(k.split('|', 1)): v for k, v in p['orig'].items()}
    return dlc1, orig


def pool_for_file(rel):
    """按文件名找参考译文源。参考源的 rel 路径与 DLC2 可能不同，做后缀匹配。"""
    base = os.path.basename(rel)
    cands = [rel]
    return cands


def main():
    dlc1, orig = load_pool()
    # 建索引：basename -> {(key): val}
    idx1 = defaultdict(dict)
    for (rel, key), v in dlc1.items():
        idx1[os.path.basename(rel)][key] = v
    idxo = defaultdict(dict)
    for (rel, key), v in orig.items():
        idxo[os.path.basename(rel)][key] = v

    tot_rows = []
    for rel, (tag, kattr, tattrs) in sorted(FILES.items()):
        doc = xlit.Doc(rel)
        els = [e for _, _, e in doc.elements(tag)]
        b = os.path.basename(rel)
        i1 = idx1.get(b, {})
        io_ = idxo.get(b, {})
        hit = miss = 0
        misses = []
        for el in els:
            key = el.get(kattr)
            if key is None:
                for a in ID_ATTRS_FALLBACK:
                    if el.has(a):
                        key = el.get(a)
                        break
            src = None
            if key in i1:
                src = ('dlc1', i1[key])
            elif key in io_:
                src = ('orig', io_[key])
            if src:
                hit += 1
            else:
                miss += 1
                sample = ''
                for a in tattrs:
                    if el.has(a) and el.get(a):
                        sample = xlit.unesc(el.get(a))[:46]
                        break
                if len(misses) < 3:
                    misses.append('%s :: %s' % (key, sample))
        pct = 100.0 * hit / max(1, hit + miss)
        print('%-34s %4d 条  命中 %3d (%4.1f%%)' % (b, hit + miss, hit, pct))
        for m in misses:
            print('        未命中样例: %s' % m)
        tot_rows.append((rel, hit + miss, hit))

    T = sum(r[1] for r in tot_rows)
    H = sum(r[2] for r in tot_rows)
    print('-' * 60)
    print('合计 %d 条，可复用 %d 条（%.1f%%），需新翻 %d 条'
          % (T, H, 100.0 * H / max(1, T), T - H))


if __name__ == '__main__':
    main()