# -*- coding: utf-8 -*-
"""把译文套到 DLC2 上，生成 DLC2_DATA_CHS。

安全设计（针对上一次 AI 弄坏 6 个文件的事故）：
  1. 输出永远从 DLC2_DATA 原文件派生，只替换属性值的字节区间；
     俄文 cp1251 片段原样保留，译文按 GBK 写入，结构零改动。
  2. **原文指纹闸门**：既有译文带 `src`（参考源原文）。仅当 DLC2 当前文本
     与 `src` 完全一致时才允许套用 —— 因为 DLC1 是英文版、DLC2 是俄文版，
     同一 id 未必是同一句话（langcheck.py / verify_reuse.py 已证）。
  3. **格式占位符闸门**：%1s / %2d 等必须与原文完全一致，不一致就拒绝。
  4. 写盘后校验：XML 可解析、元素数一致、非翻译属性未被改动。
"""
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit

ROOT = xlit.ROOT

_FMT = re.compile(r'%(\d+\$)?[-+ #0]*[\d.]*[a-zA-Z%]')
# 需保留的字面量：@%# 是原版脏话遮罩（dv4_t3），% h 是 "|°" 的温度写法（dv4_t1）。
# 先把它们替换成哨兵，抽完占位符再还原。
_LITERAL_PCT = re.compile(r'@%#|%(?=\s)')


def fmt_specs(s):
    """抽取格式占位符集合。

    - `%%` 视作字面量不计。
    - `@%#` 是原版用来遮蔽脏话的写法（questinfoglobal dv4_t3 "this @%# SWAT"），
      不是格式符，必须保留。
    - `%` 后跟空白也不是格式符（dv4_t1 天气预报道数 "85-95%" 被 _FMT 误吃成 "% h"）。
    """
    s2 = _LITERAL_PCT.sub('\x00', s)
    return sorted(m.group(0) for m in _FMT.finditer(s2) if m.group(0) != '%%')


# 无需翻译：剥掉格式占位符后只剩数字/标点（如 "640 x 480"、"0/5"）。
_NO_TEXT = re.compile(r'^[\d\sx×X/:.,#%\-()+<>|=*_~\\]*$')


def no_translate(cur):
    """原文不含字母（分辨率、计数等）时判定为无需翻译。"""
    stripped = _FMT.sub('', cur).strip()
    return bool(stripped) and bool(_NO_TEXT.fullmatch(stripped))


# 键位名在 DLC2 原件里本来就是英文（'Pause'、'Ctrl'、'Правый Ctrl' 混排），
# 任何语言下都保持原样：DLC1_CHS 也同样保留英文，不译。
# 覆盖单字母、F1-F24、Esc/Tab/Enter/Home/End/PgUp/PgDn/Insert/Delete/Ctrl/Alt/
# Shift/Menu/Space/PrintScreen/ScrollLock/NumLock/CapsLock/Backspace/Pause。
# 纯符号（~ \ ( ) , . / : _ 数字）已被上面的 _NO_TEXT 覆盖。
KEYNAME_OK = re.compile(
    r'^(?:[A-Za-z]|F(?:[1-9]|1[0-9]|2[0-4])|'
    r'Esc(?:ape)?|TAB|Enter|Return|Home|End|PgUp|PgDn|PageUp|PageDown|'
    r'Insert|Ins|Delete|Del|Ctrl|Control|Alt|Shift|Menu|Space|Pause|'
    r'PrintScreen|ScrollLock|NumLock|CapsLock|Backspace)$',
    re.I)
# 俄文版键位名（DLC2 原件里俄英混排）：
#   'Правый Ctrl' = 右Ctrl，与 DLC1_CHS 的 '右Ctrl' 同义
_RU_KEYNAME = {
    'Правый Ctrl': '右Ctrl',
    'Левый Ctrl': '左Ctrl',
}


def ru_keyname(cur):
    return _RU_KEYNAME.get(cur.strip())


def skip_translation(base, key, cur):
    """该条目是否不需要人工翻译。返回 True 表示跳过（不算待翻）。"""
    stripped = cur.strip()
    if not stripped:
        return True
    if no_translate(cur):
        return True
    # bindnames.xml 的 KEY_* 是键位名而非文案
    if base == 'bindnames.xml' and key and key.startswith('KEY_'):
        # 俄文键位名（'Правый Ctrl'）要译；纯英文键位名（Q/F1/PgUp/Esc）不译
        if ru_keyname(stripped):
            return False
        return bool(KEYNAME_OK.fullmatch(stripped))
    return False


# ---------------------------------------------------------------- 文件定义
FILES = {
    'data/if/strings/setupstrings.xml':     {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/msgpatterns.xml':      {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/gamestrings.xml':      {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/bindnames.xml':        {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/fadingmsgs.xml':       {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/loadtips.xml':         {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/uidescription.xml':    {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/uieditorstrings.xml':  {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/uieditstrings.xml':    {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/strings/help.xml':             {'tag': 'string', 'key': 'id', 'attrs': ['value']},
    'data/if/diz/questinfoglobal.xml':      {'tag': 'QuestInfo', 'key': 'questName', 'attrs': ['briefDiz', 'fullDiz']},
    'data/if/diz/dynamicdialogsglobal.xml': {'tag': 'Reply', 'key': 'name', 'attrs': ['text']},
    'data/if/diz/model_names.xml':          {'tag': 'Item', 'key': 'id', 'attrs': ['value']},
    'data/if/levelinfo/levelinfo.xml':      {'tag': 'LevelInfo', 'key': 'name', 'attrs': ['fullName', 'diz0', 'diz1']},
    'data/if/levelinfo/checkpointinfo.xml': {'tag': 'CheckpointInfo', 'key': 'id', 'attrs': ['fullName', 'diz']},
}
for _n in (1, 2, 4, 5, 6, 7, 8, 9):
    FILES['data/maps/dv%d/strings.xml' % _n] = {'tag': 'string', 'key': 'id', 'attrs': ['value']}


def load_pool():
    p = os.path.join(ROOT, 'tools_dlc2', 'reuse_pool.json')
    if not os.path.exists(p):
        return {}
    with open(p, encoding='utf-8') as f:
        return json.load(f)


def load_manual():
    p = os.path.join(ROOT, 'tools_dlc2', 'manual.json')
    if not os.path.exists(p):
        return {}
    with open(p, encoding='utf-8') as f:
        return json.load(f)


class Applier:
    def __init__(self, manual=None, pool=None, strict_src=True):
        self.pool = pool if pool is not None else load_pool()
        self.manual = manual if manual is not None else load_manual()
        self.strict_src = strict_src

    def lookup(self, base, key, attr, cur):
        """返回 (译文, 来源标记) 或 (None, 原因)。来源标记：manual/reuse/... """
        m = self.manual.get(base, {}).get(key)
        if m and m.get(attr):
            return m[attr], 'manual'
        e = self.pool.get(base, {}).get(key)
        if not e or attr not in e:
            return None, '无既有译文'
        rec = e[attr]
        cn, src = rec['cn'], rec.get('src')
        if self.strict_src and src is not None and src != cur:
            return None, '原文不一致'
        if self.strict_src and src is None:
            return None, '参考源无原文'
        return cn, rec.get('from', 'reuse')

    def run_file(self, rel, dry=False, allow_partial=True):
        cfg = FILES[rel]
        base = os.path.basename(rel)
        doc = xlit.Doc(rel)
        loc = doc.elements(cfg['tag'])
        stat = {'applied': 0, 'manual': 0, 'reuse': 0, 'fmt_reject': 0}
        skipped = []
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
                cn, why = self.lookup(base, key, attr, cur)
                if not cn:
                    skipped.append((key, attr, cur, why))
                    continue
                a, b = fmt_specs(cur), fmt_specs(cn)
                if a != b:
                    stat['fmt_reject'] += 1
                    skipped.append((key, attr, cur, '占位符不符:%s!=%s' % (a, b)))
                    continue
                el.set(attr, cn)
                stat['applied'] += 1
                stat[why if why in ('manual', 'reuse') else 'reuse'] += 1
        path = None if dry else doc.write(loc)
        errs = [] if dry else self.check(rel, path, cfg)
        return {'rel': rel, 'n_el': len(loc), 'path': path, 'errs': errs,
                'skipped': skipped, **stat}

    def check(self, rel, path, cfg):
        """XML 可解析 / 元素数一致 / 非翻译属性未被改动。"""
        errs = []
        raw = xlit.read_bytes(path)
        # 混合编码：汉字段是 GBK，俄文段是 cp1251。用 latin-1 做结构解析不影响标签结构。
        # 判据是"不比原件更差"：原件 dynamicdialogsglobal.xml 第 269 行
        # scriptCondition 属性里含裸 '<'，标准解析器本来就不接受。
        def perr(data):
            try:
                ET.fromstring(data)
                return None
            except ET.ParseError as ex:
                return (ex.position[0], ex.position[1])

        our = perr(raw.decode('latin-1'))
        base_err = perr(xlit.read_bytes(os.path.join(xlit.SRC, *rel.split('/'))).decode('latin-1'))
        if our and not base_err:
            errs.append('XML 解析失败(原件合法): line %d col %d' % our)
        elif our and base_err and our != base_err:
            errs.append('XML 解析错误位置变化 %s -> %s' % (base_err, our))
        src_n = len(xlit.Doc(rel).elements(cfg['tag']))
        out_n = len(xlit.find_elements(raw.decode('latin-1'), cfg['tag']))
        if src_n != out_n:
            errs.append('元素数不一致 %d -> %d' % (src_n, out_n))
        a = xlit.find_elements(xlit.Doc(rel).text, cfg['tag'])
        b = xlit.find_elements(raw.decode('latin-1'), cfg['tag'])
        for x, y in zip(a, b):
            if x.spans.keys() != y.spans.keys():
                errs.append('属性集变化: %s' % x.raw[:50])
                break
            for attr in x.spans:
                if attr in cfg['attrs']:
                    continue
                if x.get(attr) != y.get(attr):
                    errs.append('属性 %s 被改动' % attr)
                    break
        return errs


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--file')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--loose', dest='strict_src', action='store_false', default=True,
                    help='放松原文指纹闸门（不推荐）')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--show-skip', type=int, default=0)
    a = ap.parse_args()

    ap_obj = Applier(strict_src=a.strict_src)
    targets = sorted(FILES) if a.all else [a.file]
    T = S = R = 0
    for rel in targets:
        r = ap_obj.run_file(rel, dry=a.dry)
        T += r['n_el']
        S += r['applied']
        R += r['fmt_reject']
        print('%s %-26s %4d 元素 / 译 %4d (手工 %3d, 复用 %3d) / 跳过 %4d'
              % ('OK ' if not r['errs'] else 'ERR', rel.split('/')[-1],
                 r['n_el'], r['applied'], r['manual'], r['reuse'], len(r['skipped'])))
        for e in r['errs'][:4]:
            print('      !! %s' % e)
        for k, at, cur, why in r['skipped'][:a.show_skip]:
            print('      跳过 %s/%s [%s] %s' % (k, at, why, cur[:50]))
    print('-' * 70)
    print('合计 %d 元素，已译 %d，需人工 %d，占位符拒绝 %d'
          % (T, S, T - S - R, R))


if __name__ == '__main__':
    main()