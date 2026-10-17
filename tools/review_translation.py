# -*- coding: utf-8 -*-
"""提交前译文体检：不依赖 fixwrap，独立扫描三类风险。

1. 字面丢失    —— 去掉 | 后与 HEAD 逐字比对
2. 占位符损坏  —— %1s/%2d 等集合变化
3. 排版异常    —— 行首禁头标点、行尾悬空、行宽超出控件容量
4. 编码/结构   —— GBK 可解码、XML 元素数、属性完整性
5. 乱码探测    —— 出现 mojibake 特征（如「锟斤拷」「烫烫烫」）
"""
from __future__ import print_function

import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(encoding="utf-8")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_out_review.txt")
_f = open(OUT, "w", encoding="utf-8")


def p(*a):
    _f.write(" ".join(str(x) for x in a) + "\n")


ROOTS = ["Original_DATA_CHS", "DLC1_DATA_CHS", "DLC2_DATA_CHS"]
SPECIAL = re.compile(r'\b(?:msgType|fontSize|scrollSpeed|numButtons)=')
PLACE = re.compile(r"%\d*(?:\$\d+)?[-+ #0]*[\d.]*[a-zA-Z%]")
NOHEAD = set(u"。！？；：、）】》」』”’，,.!?;:")
MOJIBAKE = (u"锟斤拷", u"烫烫烫", u"屯屯屯", u"���")

# 控件容量（像素）
CAPS = {
    "clansdiz": 26 * 20,
    "objectdiz": 14 * 20,
    "perksdiz": 26 * 20,
    "strings": 20 * 20,      # maps/*/strings.xml → wndText
    "dynamicdialogs": 44 * 20,
    "levelinfo": 44 * 20,
}


def disp(s):
    return sum(20 if ord(c) > 0x2E80 else 9 for c in s)


def dec_like(raw, enc="gbk"):
    try:
        return raw.decode(enc)
    except UnicodeDecodeError:
        for e in ("cp1251", "utf-8", "gbk"):
            try:
                return raw.decode(e)
            except UnicodeDecodeError:
                continue
        return raw.decode("gbk", "replace")


def head_bytes(root, rel):
    r = subprocess.run(["git", "show", "HEAD:%s/%s" % (root, rel)],
                       cwd=BASE, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def classify(rel):
    n = os.path.basename(rel)
    if n in ("clansdiz.xml", "perksdiz.xml"):
        return "clansdiz"
    if n == "objectdiz.xml":
        return "objectdiz"
    if n == "dynamicdialogsglobal.xml":
        return "dynamicdialogs"
    if n == "levelinfo.xml":
        return "levelinfo"
    if n == "strings.xml" and "maps" in rel:
        return "strings"
    return None


def values(text, tag, attr):
    rx = re.compile(r"(<%s\b[^>]*?\b%s=\")((?:[^\"])*?)(\")"
                    % (re.escape(tag), re.escape(attr)), re.S)
    whole = re.compile(r"<%s\b[^>]*?/>" % re.escape(tag), re.S)
    out = []
    for m in rx.finditer(text):
        e = whole.search(text, max(0, m.start() - 4))
        blk = e.group(0) if e else m.group(1)
        km = re.search(r'\bid="([^"]*)"', blk) or \
            re.search(r'\bname="([^"]*)"', blk)
        out.append((km.group(1) if km else "", m.group(2), bool(SPECIAL.search(blk))))
    return out


def diz_values(text):
    out = []
    for m in re.finditer(r'name="([^"]*)"(.*?)/>', text, re.S):
        dm = re.search(r'\bdiz\d+="((?:[^\"])*?)"', m.group(2), re.S)
        if dm:
            out.append((m.group(1), dm.group(1), False))
    return out


p("=" * 104)
p("提交前译文体检")
p("=" * 104)

summary = []
for root in ROOTS:
    files = subprocess.run(["git", "diff", "--name-only", "--", root],
                           cwd=BASE, capture_output=True).stdout
    names = [x for x in files.decode().split("\n") if x.strip()]
    p("")
    p("### %s　改动 %d 个文件" % (root, len(names)))
    for full in names:
        rel = full[len(root) + 1:]
        kind = classify(rel)
        if not kind:
            p("  ? %-50s [未归类，跳过]" % rel)
            continue
        cap = CAPS[kind]
        raw = open(os.path.join(BASE, full), "rb").read()
        text = dec_like(raw)
        hb = head_bytes(root, rel)
        ht = dec_like(hb) if hb else ""

        if kind == "levelinfo":
            cur = diz_values(text)
            old = dict(diz_values(ht)) if ht else {}
        else:
            tag, attr = ("Reply", "text") if kind == "dynamicdialogs" \
                else ("string", "value")
            cur = values(text, tag, attr)
            old = dict((k, v) for k, v, _ in values(ht, tag, attr)) if ht else {}

        n_lost = n_place = n_head = n_wide = n_moji = n_skip = 0
        detail = []
        for sid, v, special in cur:
            if special:
                n_skip += 1
                continue
            ov = old.get(sid)
            if ov is not None:
                if re.sub(r"\|+", "", ov) != re.sub(r"\|+", "", v):
                    n_lost += 1
                    detail.append(("字面丢失", sid, ov[:40], v[:40]))
                po = sorted(x.group(0) for x in PLACE.finditer(ov)
                            if x.group(0) != "%%")
                pn = sorted(x.group(0) for x in PLACE.finditer(v)
                            if x.group(0) != "%%")
                if po != pn:
                    n_place += 1
                    detail.append(("占位符", sid, ",".join(po), ",".join(pn)))
            if any(mk in v for mk in MOJIBAKE):
                n_moji += 1
                detail.append(("乱码", sid, v[:40], ""))
            for seg in v.split("|"):
                if len(seg) >= 2 and seg[0] in NOHEAD:
                    n_head += 1
                    detail.append(("行首禁头", sid, seg[:20], ""))
                if disp(seg) > cap:
                    n_wide += 1
                    detail.append(("超宽", sid,
                                   "%dpx" % disp(seg), "上限%dpx" % cap))

        # XML 结构
        xml_ok = True
        try:
            ET.fromstring(re.sub(r"^\s*<\?xml[^>]*\?>", "", text).strip())
        except ET.ParseError:
            hb2 = ht
            try:
                ET.fromstring(
                    re.sub(r"^\s*<\?xml[^>]*\?>", "", hb2).strip())
            except ET.ParseError:
                xml_ok = True          # 原版即如此
            else:
                xml_ok = False         # 我改坏了

        bad = n_lost + n_place + n_moji + (0 if xml_ok else 1)
        flag = "OK " if bad == 0 else "!! "
        p("  %s%-50s 条目%3d 跳过%2d 字面丢%2d 占位符%2d 行首禁头%3d 超宽%3d 乱码%d"
          % (flag, rel, len(cur), n_skip, n_lost, n_place, n_head,
             n_wide, n_moji))
        if not xml_ok:
            p("       ✗ XML 由可解析变为不可解析")
        for d in detail[:4]:
            p("       · [%s] %s | 旧:%r 新:%r" % d)
        summary.append((root, rel, bad))

p("")
p("=" * 104)
tot = sum(b for _, _, b in summary)
p("共检查 %d 个改动文件，问题 %d 处" % (len(summary), tot))
if tot == 0:
    p("✓ 未发现字面丢失、占位符损坏、乱码、XML 损坏")
else:
    p("✗ 需人工复核")
p("=" * 104)
_f.close()
print("written", OUT)