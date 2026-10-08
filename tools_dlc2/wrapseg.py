# -*- coding: utf-8 -*-
"""断行器：把长中文段落切成多行（10-16 字，标点优先）。

动态规划求全局最优断点。代价函数组合「长度偏离 12 的平方」与
「是否断在标点后」，对最短行、禁头字符、切断数字/代号重罚。

依据基准：DLC1_DATA_CHS 已部署汉化的实际行宽分布（10-16，中位 12）。
"""
import re

MIN, MAX, TARGET = 10, 16, 12
SENT = '。！？!?'
CLAUSE = '，、；：,;:'
TAIL = '」』）】》”’"'
# 禁止行首的标点。注意 '…' 不在内：中文省略号常用于段首表示引文省略。
NOHEAD = set('。！？；：、）】》」』”’')

W_DEV = 0.6
W_PUNCT_BONUS = -6.0
W_HARD = 6.0
W_SHORT = 60.0
W_HEAD = 80.0
W_TAILONLY = 25.0

_LATIN = set('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:')
# 引擎格式占位符：%1s %2d %2b …。断行时不得从占位符中间切开，
# 且占位符本身不计入行宽判断的核心断点（它只是一个原子）。
_PLACE = re.compile(r'%\d*[a-zA-Z]')


def _ok_head(ch):
    return ch not in NOHEAD


def _atomic(s, j):
    """断点位于 s[j-1] 与 s[j] 之间时，是否允许断开。
    不可切断：数字串（含中文单位）、拉丁词、代号、百分比。"""
    n = len(s)
    if j < 1 or j > n - 1:
        return True
    a, b = s[j - 1], s[j]
    if a in _LATIN and b in _LATIN:
        return False
    if a in _LATIN and not b.isascii() and not b.isspace():
        return False
    if not a.isascii() and not a.isspace() and b in _LATIN:
        return False
    if a.isdigit() and b in '%％':
        return False
    if a in '%％' and b.isdigit():
        return False
    if a == ' ':
        k = j + 1
        while k < n and s[k] in _LATIN:
            k += 1
        if k > j + 1:
            return False
    if b == ' ':
        k = j - 2
        while k >= 0 and s[k] in _LATIN:
            k -= 1
        if k < j - 2:
            return False
    # 占位符 %2b 等：断点不得落在占位符内部，也不得把它与紧邻的
    # 序号数字拆开（%2|b）。占位符整体视为一个不可切原子。
    for mm in _PLACE.finditer(s, max(0, j - 5), min(n, j + 5)):
        if mm.start() < j < mm.end():
            return False
        if mm.start() == j and j > 0 and s[j - 1].isdigit():
            return False
    return True


def wrap_seg(s):
    n = len(s)
    if n <= MAX:
        return [s]

    INF = float('inf')
    cost = [INF] * (n + 1)
    prev = [-1] * (n + 1)
    cost[0] = 0.0

    for i in range(1, n + 1):
        if cost[i - 1] == INF:
            continue
        for L in range(1, min(MAX + 2, n - i + 1) + 1):
            j = i - 1 + L
            if j > n:
                break
            line = s[i - 1:j]
            c = cost[i - 1]
            if L <= MAX:
                c += W_DEV * (L - TARGET) ** 2
            else:
                c += W_DEV * (L - TARGET) ** 2 + 30.0 * (L - MAX)
            if len(line) >= 2 and not _ok_head(line[0]):
                c += W_HEAD
            if not _atomic(s, j):
                c += W_HEAD * 2
            if line[-1] in SENT:
                c += W_PUNCT_BONUS
            elif line[-1] in CLAUSE:
                c += W_PUNCT_BONUS * 0.6
            else:
                c += W_HARD
            if len(line) <= 2 and line[0] in SENT + CLAUSE:
                c += W_TAILONLY
            if j < n:
                if L < MIN:
                    c += W_SHORT * (1 + (MIN - L))
                elif L < 4:
                    c += W_SHORT
            if c < cost[j]:
                cost[j] = c
                prev[j] = i - 1

    if cost[n] == INF:
        return [s[i:i + MAX] for i in range(0, n, MAX)]

    lines = []
    j = n
    while j > 0:
        i = prev[j]
        lines.append(s[i:j])
        j = i
    lines.reverse()
    return lines


def wrap_value(v):
    """对整条属性值断行，保留 | 分段结构。"""
    segs = v.split('|')
    out = []
    for sg in segs:
        if len(sg) <= MAX:
            out.append(sg)
        else:
            out.extend(wrap_seg(sg))
    return '|'.join(out)
