# -*- coding: utf-8 -*-
"""抽取既有译文，并记录每条译文的**原文指纹**。

关键背景（langcheck.py 查证）：
  DLC1_DATA / Original_DATA 原件是**英文**，DLC1_DATA_CHS / Original_DATA_CHS
  是从英文翻的**中文**；DLC2_DATA 原件是**俄文**。
  所以"按 id 复用"必须额外验证 DLC2 的原文与参考源的原文是否同一句话。

本模块输出 tools_dlc2/reuse_pool.json：
  { basename: { key: { attr: {"cn": 译文, "src": 参考源原文} } } }
apply.py 拿 src 与 DLC2 当前文本比对，只有完全一致才允许套用译文。
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT

# 参考译文源：目录, 优先标记
REFS = [
    (os.path.join(ROOT, 'DLC1_DATA'), os.path.join(ROOT, 'DLC1_DATA_CHS'), 'dlc1'),
    (os.path.join(ROOT, 'Original_DATA'), os.path.join(ROOT, 'Original_DATA_CHS'), 'orig'),
]

# 只处理这些目录下的 XML（其余是界面布局/字体/名单）
WANTED = re.compile(r'data/if/(strings|diz|levelinfo)/.*\.xml$')

# (标签, 主键属性) —— 与 apply.py 的 FILES 保持一致
TAG_KEY = [
    ('string', 'id'), ('Item', 'id'), ('QuestInfo', 'questName'),
    ('LevelInfo', 'name'), ('CheckpointInfo', 'id'), ('Reply', 'name'),
]
# 所有可能承载文本的属性
TEXTS = ('value', 'text', 'fullName', 'diz', 'briefDiz', 'fullDiz', 'diz0', 'diz1')


def _read_plain(p):
    """原版目录按 cp1251 读。"""
    return xlit.read_bytes(p).decode('cp1251', errors='replace')


def _read_chs(p):
    """汉化目录按 GBK 读。"""
    return xlit.read_bytes(p).decode('gbk', errors='replace')


def _entries(text):
    """-> [(key, {attr: 原文})]"""
    out = []
    for tag, kattr in TAG_KEY:
        for el in xlit.find_elements(text, tag):
            key = el.get(kattr)
            if not key:
                continue
            attrs = {}
            for a in TEXTS:
                if el.has(a) and el.get(a):
                    attrs[a] = xlit.unesc(el.get(a))
            if attrs:
                out.append((key, attrs))
    return out


def main():
    pool = {}
    for src_dir, chs_dir, label in REFS:
        n = 0
        for dp, _, fn in os.walk(chs_dir):
            for f in sorted(fn):
                if not f.lower().endswith('.xml'):
                    continue
                rel_chs = os.path.relpath(os.path.join(dp, f), chs_dir).replace(os.sep, '/')
                if not WANTED.match(rel_chs):
                    continue
                rel_src = os.path.join(src_dir, *rel_chs.split('/'))
                if not os.path.exists(rel_src):
                    continue
                try:
                    chs_text = _read_chs(os.path.join(dp, f))
                    src_text = _read_plain(rel_src)
                except Exception as e:
                    print('  跳过 %s: %s' % (rel_chs, e))
                    continue
                chs_map = {}
                for key, attrs in _entries(chs_text):
                    if any(xlit.is_cjk(v) for v in attrs.values()):
                        chs_map[key] = attrs
                src_map = dict(_entries(src_text))
                base = os.path.basename(rel_chs)
                slot = pool.setdefault(base, {})
                for key, cattrs in chs_map.items():
                    sattrs = src_map.get(key, {})
                    entry = slot.setdefault(key, {})
                    for attr, cn in cattrs.items():
                        if not xlit.is_cjk(cn):
                            continue
                        srcv = sattrs.get(attr)
                        # 已经在更优先的源里出现过就跳过（DLC1 优先）
                        if attr in entry:
                            continue
                        entry[attr] = {'cn': cn, 'src': srcv, 'from': label}
                        n += 1
        print('  %-6s 收录译文 %d 条' % (label, n))

    out = os.path.join(ROOT, 'tools_dlc2', 'reuse_pool.json')
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(pool, f, ensure_ascii=False, indent=1)
    print('已写 %s（%d 个文件）' % (out, len(pool)))


if __name__ == '__main__':
    main()