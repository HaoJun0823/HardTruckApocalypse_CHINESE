# -*- coding: utf-8 -*-
"""按"俄文原文一致"校验复用的正确性。

思路：DLC2 与 DLC1/Original 的原文都是俄文 cp1251。同一 id 若两边俄文 value
完全相同，说明 DLC2 确实在翻同一条，复用可信；若俄文不同（DLC2 改写了文本），
复用译文就可能对不上，需要人工过目或新翻。

输出三档：
  一致   -> 直接复用
  不同   -> 需过目（列出样例）
  无对照 -> DLC1 里没有这条俄文原文（id 是 DLC2 新增）-> 需过目
"""
import json
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT

TAG_KEY = (('string', 'id'), ('Item', 'id'), ('QuestInfo', 'questName'),
           ('LevelInfo', 'name'), ('CheckpointInfo', 'id'), ('Reply', 'name'))
TEXTS = ('value', 'text', 'fullName', 'diz', 'briefDiz', 'fullDiz', 'diz0', 'diz1')


def read_entries(root, rel):
    """读一个原版目录下的文件，返回 {key: 俄文原文}。cp1251。"""
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        return None
    text = xlit.read_bytes(p).decode('cp1251', errors='replace')
    out = {}
    for tag, kattr in TAG_KEY:
        for el in xlit.find_elements(text, tag):
            k = el.get(kattr)
            if not k:
                continue
            for a in TEXTS:
                v = el.get(a)
                if v:
                    out[k] = xlit.unesc(v)
                    break
    return out


FILES = [
    ('data/if/diz/model_names.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/uieditorstrings.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/uidescription.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/uieditstrings.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/gamestrings.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/bindnames.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/fadingmsgs.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/help.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/setupstrings.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/msgpatterns.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/strings/loadtips.xml', ['DLC1_DATA', 'Original_DATA']),
    ('data/if/diz/dynamicdialogsglobal.xml', ['DLC1_DATA', 'Original_DATA']),
]


def main():
    with open(os.path.join(ROOT, 'tools_dlc2', 'reuse_pool.json'), encoding='utf-8') as f:
        pool_raw = json.load(f)
    pool = defaultdict(dict)
    for src in ('dlc1', 'orig'):
        for k, v in pool_raw[src].items():
            rel, key = k.split('|', 1)
            pool[src].setdefault(os.path.basename(rel), {}).setdefault(key, v)

    grand = defaultdict(int)
    rows = []
    for rel, refbases in FILES:
        base = os.path.basename(rel)
        d2 = read_entries(os.path.join(ROOT, 'DLC2_DATA'), rel)
        if d2 is None:
            continue
        stats = defaultdict(int)
        diffs = []
        for key, ru2 in d2.items():
            # 找这个 id 在哪个参考源里有译文
            src = None
            for sb, sdir in (('dlc1', 'DLC1_DATA'), ('orig', 'Original_DATA')):
                if key in pool[sb].get(base, {}):
                    src = sb
                    break
            if src is None:
                stats['无既有译文'] += 1
                continue
            ru1 = read_entries_cache(refbases, src, rel)
            if ru1 is None or key not in ru1:
                stats['无俄文对照'] += 1
                continue
            if ru1[key] == ru2:
                stats['俄文一致'] += 1
            else:
                stats['俄文不同'] += 1
                if len(diffs) < 8:
                    diffs.append((key, ru2, ru1[key]))
        for k, v in stats.items():
            grand[k] += v
        rows.append((base, stats, diffs))

    print('%-28s %6s %6s %6s %6s' % ('file', '一致', '不同', '无对照', '无译文'))
    for base, stats, _ in rows:
        print('%-28s %6d %6d %6d %6d' % (base, stats['俄文一致'], stats['俄文不同'],
                                        stats['无俄文对照'], stats['无既有译文']))
    print('-' * 58)
    print('%-28s %6d %6d %6d %6d' % ('合计', grand['俄文一致'], grand['俄文不同'],
                                    grand['无俄文对照'], grand['无既有译文']))

    print('\n=== 俄文不同（复用有风险，需过目）样例 ===')
    for base, stats, diffs in rows:
        if not diffs:
            continue
        print('== %s (%d 不同)' % (base, stats['俄文不同']))
        for key, ru2, ru1 in diffs[:5]:
            print('   id=%s' % key)
            print('     DLC2 : %s' % ru2[:78])
            print('     DLC1 : %s' % ru1[:78])


_cache = {}


def read_entries_cache(refbases, src, rel):
    sdir = 'DLC1_DATA' if src == 'dlc1' else 'Original_DATA'
    ck = (sdir, rel)
    if ck not in _cache:
        _cache[ck] = read_entries(os.path.join(ROOT, sdir), rel)
    return _cache[ck]


if __name__ == '__main__':
    main()