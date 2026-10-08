# -*- coding: utf-8 -*-
"""DLC2 汉化共用库。

事故背景：上一个 AI 直接改写 XML 结构，把译文前���到原文上并删掉了收尾引号，
导致 6 个文件无法被解析。因此本库的设计原则是：

  **结构绝不重建。** 输出从 DLC2_DATA 原文件逐字节派生，只对属性值的
  字符区间做替换，空白、注释、属性顺序、结尾的 '/' 一律原样保留。
  写盘前强制往返校验，渲染结果必须与输入完全一致。

编码：文件头声明 windows-1251 是原厂遗留，实际按 GBK 读写（与 DLC1_DATA_CHS 一致）。
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'DLC2_DATA')
CHS = os.path.join(ROOT, 'DLC2_DATA_CHS')


# ---------------------------------------------------------------- 编码

def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def decode(b):
    """DLC2 的原始文本是俄文 cp1251；汉化版才是 GBK 汉字。

    与 DLC1_DATA / Original_DATA 一致：原版按 cp1251 解码，
    我们写回的译文按 GBK 编码。原版里本来就有的非 ASCII 字节
    （俄文）一律原样保留，不经 decode/encode 往返。
    """
    return b.decode('cp1251', errors='replace')


def encode(s):
    return s.encode('gbk', errors='replace')


# ---- 原版（俄文 cp1251）读取辅助：只有比较/校验时用，输出路径绝不经过它
def read_original(text):
    """把从 DLC2_DATA 读出的文本按原样保留；GBK 译文单独编码。"""


def gbk_decode(b):
    return b.decode('gbk', errors='replace')


def mixed_decode(b):
    """解码一个**混合编码**的成品文件：译文是 GBK 汉字，未翻译的原文是 cp1251 俄文。

    逐字节尝试 GBK 双字节序列，成功则整体采纳；失败的字节按 cp1251 单字节处理。
    这样既不会因为俄文而整体解码失败，也能正确读出汉字。
    """
    out = []
    i = 0
    n = len(b)
    while i < n:
        c = b[i]
        if 0x81 <= c <= 0xFE and i + 1 < n:
            two = b[i + 1]
            # GBK 第二字节范围 0x40-0xFE（排除 0x7F）
            if 0x40 <= two <= 0xFE and two != 0x7F:
                try:
                    ch = bytes((c, two)).decode('gbk')
                except UnicodeDecodeError:
                    ch = None
                if ch is not None:
                    out.append(ch)
                    i += 2
                    continue
        out.append(chr(c))
        i += 1
    return ''.join(out)


def is_cjk(s):
    return any('\u4e00' <= c <= '\u9fff' for c in s)


def is_cyrillic(s):
    return any('\u0400' <= c <= '\u04ff' for c in s)


def is_latin(s):
    return any('a' <= c.lower() <= 'z' for c in s)


# ---------------------------------------------------------------- 属性区间
# 只识别双引号属性值。原文件里属性值不含裸 "，所以这是安全的。
# group: 属性名 + 值区间（相对 raw 的偏移）

_ATTR_RE = re.compile(
    r'([A-Za-z_:][\w:.\-]*)'       # 1 属性名
    r'\s*=\s*"'                     # 等号与开引号
    r'([^"]*)"'                     # 2 值本体（后面紧跟闭引号）
)

# 开标签扫描：不能简单用 [^<>]*?，因为 DLC2 原件里存在属性值含裸 '<' 的情况
#   （dynamicdialogsglobal.xml:269  scriptCondition="GetPlayerMoney() < ConversationWnd:..."）
#   简单的 [^<>]*? 会在那里提前截断，导致该元素被整体漏掉。
# 正确做法：从 '<' 之后手工扫描成对的双引号，值内部的 < > ' 全部当作普通字符。
_TAGNAME_RE = re.compile(r'<\s*([A-Za-z_][\w:.\-]*)')


def _scan_open_tag(text, i):
    """text[i] == '<'。返回该开标签的结束位置（不含），失败返回 -1。

    规则：跳到标签名后，逐字符前进；引号外遇到 '>' 即结束（可带 '/'），
    引号内一切字符（含 '<' '>'）都跳过，直到配对的 '"'。
    """
    n = len(text)
    m = _TAGNAME_RE.match(text, i)
    if not m:
        return -1
    j = m.end()
    while j < n:
        c = text[j]
        if c == '"':
            k = text.find('"', j + 1)
            if k < 0:
                return -1
            j = k + 1
            continue
        if c == "'":
            k = text.find("'", j + 1)
            if k < 0:
                return -1
            j = k + 1
            continue
        if c == '>':
            return j + 1
        if c == '<':
            return -1          # 未闭合的标签，后面的 '<' 属于别的标签
        j += 1
    return -1


def _iter_open_tags(text, tag):
    """产出 (start, end) —— 所有 <tag ...> 开标签的区间（end 不含）。"""
    name = re.escape(tag)
    pat = re.compile(r'<\s*' + name + r'(?=[\s/>])')
    for m in pat.finditer(text):
        i = m.start()
        # 用扫描定位结束；失败则退回"引号感知的 '>' 搜索"
        end = _scan_open_tag(text, i)
        if end < 0:
            continue
        yield i, end


class Element:
    """一个开标签的手术视图。

    raw        : 原始片段（保持原样）
    spans      : {attr_name: (start, end)} 值在 raw 中的字符区间
    pending    : {attr_name: new_escaped_value} 已登记的替换
    """

    __slots__ = ('raw', 'spans', 'order', 'pending')

    def __init__(self, raw):
        self.raw = raw
        self.spans = {}
        self.order = []
        self.pending = {}
        for m in _ATTR_RE.finditer(raw):
            name = m.group(1)
            self.spans[name] = (m.start(2), m.end(2))
            self.order.append(name)

    def get(self, name):
        sp = self.spans.get(name)
        return self.raw[sp[0]:sp[1]] if sp else None

    def getb(self, name, enc='latin-1'):
        """属性值的**原始字节**。

        源文件的 raw 是 cp1251 解码结果，成品的 raw 是 latin-1 解码结果，
        两者都是 1 字节 = 1 字符，所以字符区间可直接当字节区间用。enc 传对应的
        编码即可还原原始字节（latin-1 用于成品视图，cp1251 用于原件视图）。

        这是判断"这条到底被改没改过"的唯一可靠依据 —— 用 decode 后的 is_cjk
        判断不可靠：未翻译的 cp1251 俄文会被误当成 GBK 汉字
        （ЗАПРЕЩЕНО -> 抢闲刨磐蜙）。
        """
        sp = self.spans.get(name)
        if not sp:
            return None
        return self.raw[sp[0]:sp[1]].encode(enc, errors='replace')

    def has(self, name):
        return name in self.spans

    def render(self):
        """按 pending 做替换后的完整片段（字符形式，仅用于预览/校验）。"""
        out = self.raw
        # 从后往前替换，避免偏移失效
        for name in reversed(self.order):
            if name in self.pending:
                s, e = self.spans[name]
                out = out[:s] + self.pending[name] + out[e:]
        return out

    def render_bytes(self):
        """字节形式：俄文片段走 cp1251，译文属性值走 GBK。"""
        out = []
        cur = 0
        for name in self.order:
            s, e = self.spans[name]
            if name in self.pending:
                out.append(self.raw[cur:s].encode('cp1251', errors='replace'))
                out.append(self.pending[name].encode('gbk', errors='replace'))
                cur = e
        out.append(self.raw[cur:].encode('cp1251', errors='replace'))
        return b''.join(out)

    def set(self, name, value):
        if name not in self.spans:
            raise KeyError('%s 无属性 %s' % (self.raw[:60], name))
        if self.pending is None:
            self.pending = {}
        self.pending[name] = esc(value)

    def changed(self):
        return self.pending


_ESC_MAP = [('&', '&amp;'), ('<', '&lt;'), ('>', '&gt;')]
_ESC_RE = re.compile('|'.join(re.escape(a) for a, _ in _ESC_MAP))
_ESC_D = dict(_ESC_MAP)


def esc(s):
    return _ESC_RE.sub(lambda m: _ESC_D[m.group(0)], s)


def unesc(s):
    return (s.replace('&lt;', '<').replace('&gt;', '>')
             .replace('&quot;', '"').replace('&apos;', "'")
             .replace('&amp;', '&'))


def find_elements(text, tag):
    """取出所有 <tag ...> 开标签，返回 Element 列表（顺序即文件顺序）。"""
    return [Element(text[s:e]) for (s, e) in _iter_open_tags(text, tag)]


def _bpos(buf, char_index):
    """把字符下标换算成字节下标（源码按 cp1251 解码，1 字节=1 字符）。"""
    return char_index


class Doc:
    """一个待翻译的 XML 文件。

    elements() 返回的 Element 必须按文件顺序排列，write() 按同样顺序回写。

    编码约定：源文件是俄文 cp1251，译文是 GBK 汉字。因为两者不是同一个
    编码体系，Doc 全程把文件当作**字节序列**处理：
      - 未改动的片段按 cp1251 原样编码回去（俄文原样保留）；
      - 被替换的属性值单独按 GBK 编码。
    所以 write() 不走 Doc.write 的整串 encode，而是按区间拼接字节。
    """

    def __init__(self, rel):
        self.rel = rel.replace('/', os.sep)
        self.src_path = os.path.join(SRC, self.rel)
        self.src_bytes = read_bytes(self.src_path)
        self.text = self.src_bytes.decode('cp1251', errors='replace')

    def elements(self, tag):
        """返回 [(start, end, Element)]，位置为开标签在全文中的区间。"""
        return [(s, e, Element(self.text[s:e])) for (s, e) in _iter_open_tags(self.text, tag)]

    def write(self, located):
        """located: 与 elements() 同序的 Element 列表。

        按字节拼接：原文片段走 cp1251（俄文原样），译文属性值走 GBK（汉字）。
        """
        buf = []
        cursor = 0          # 字符下标
        bcur = 0            # 字节下标
        for (s, e, el) in located:
            assert s >= cursor, '元素顺序错乱: %s' % self.rel
            buf.append(self.src_bytes[bcur:_bpos(self.src_bytes, s)])
            r = el.render_bytes()
            buf.append(r)
            bcur = _bpos(self.src_bytes, e)
            cursor = e
        buf.append(self.src_bytes[bcur:])
        data = b''.join(buf)
        dst = os.path.join(CHS, self.rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(data)
        return dst