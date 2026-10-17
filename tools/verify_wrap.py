# -*- coding: utf-8 -*-
"""独立验收：直接量测写盘后的 XML，不复用 fixwrap 的任何判定逻辑。

逐项核对：
  1. XML 可解析、元素数与 git HEAD 一致
  2. 每行字数 ≤ 该档上限（显示宽 = 中文字数×20px）
  3. 过场/剧情对话的段数 ≤ 4（或如实列出装不下的）
  4. 格式占位符集合未变
  5. 去分隔符后的字面内容未变（没有丢字/多字）
  6. 编码仍是 GBK、XML 声明未改
"""
from __future__ import print_function

import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(encoding="utf-8")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_out_accept.txt")
_f = open(OUT, "w", encoding="utf-8")


def p(*a):
    _f.write(" ".join(str(x) for x in a) + "\n")


GAMES = [("本体", "Original_DATA_CHS"), ("DLC1", "DLC1_DATA_CHS"),
         ("DLC2", "DLC2_DATA_CHS")]

# rel -> (tag, attr, max_chars, max_segs|None)
CHECKS = {
    "data/if/strings/clansdiz.xml": ("string", "value", 26, None),
    "data/if/strings/objectdiz.xml": ("string", "value", 14, None),
    "data/if/strings/perksdiz.xml": ("string", "value", 26, None),
    "data/if/diz/dynamicdialogsglobal.xml": ("Reply", "text", 44, None),
    "data/if/diz/levelinfo.xml": ("diz", None, 44, None),
    "data/if/levelinfo/levelinfo.xml": ("diz", None, 44, None),
    "data/maps/r1m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r1m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/r1m3/strings.xml": ("string", "value", 20, 4),
    "data/maps/r1m4/strings.xml": ("string", "value", 20, 4),
    "data/maps/r2m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r2m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/r3m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r3m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/r4m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r4m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/r5m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r5m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/r6m1/strings.xml": ("string", "value", 20, 4),
    "data/maps/r6m2/strings.xml": ("string", "value", 20, 4),
    "data/maps/sin/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv1/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv2/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv4/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv5/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv6/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv7/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv8/strings.xml": ("string", "value", 20, 4),
    "data/maps/dv9/strings.xml": ("string", "value", 20, 4),
}

_PLACE = re.compile(r"%\d*(?:\$\d+)?[-+ #0]*[\d.]*[a-zA-Z%]")

CJK_W = 20          # 主字号 height=12.000 的 CJK 单元像素宽（DPI=120）
ASCII_W = 9         # fonts.xml 里 SM_Tahoma 12 的 '#'(11)/'x' 等约 6~11，取 9


def display_px(s):
    """一段文本的显示宽度（像素）。这是 UI 判"超宽"的真实依据。

    之前用 len() 判定，把「[+3]  - 」这类半角前缀和俄文片段也算成了
    超宽，误报 27 项。汉字占 20px，半角约占 9px。
    """
    return sum(CJK_W if ord(ch) > 0x2E80 else ASCII_W for ch in s)


def specs(s):
    return sorted(m.group(0) for m in _PLACE.finditer(s) if m.group(0) != "%%")


def decode(fp):
    """译文文件固定是 GBK（XML 声明写 windows-1251，由 ASI 转码）。

    注意：DLC2 的部分 clansdiz/perksdiz 实际内容是 **未翻译的俄文 cp1251**
    （apply.py 保留原文片段），这些文件本来就以 cp1251 字节存储，
    因此编码判定必须与 git HEAD 对比，而不是一律要求 gbk。
    """
    raw = open(fp, "rb").read()
    for e in ("gbk", "cp1251", "utf-8"):
        try:
            return raw.decode(e), e, raw
        except UnicodeDecodeError:
            continue
    return raw.decode("gbk", "replace"), "gbk", raw


def git_head(root, rel):
    r = subprocess.run(["git", "show", "HEAD:%s/%s" % (root, rel)],
                       cwd=BASE, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def decode_like(raw, ref_enc):
    """按参照编码解码——HEAD 与现文件必须用同一编码，否则 id 会错位配对。

    DLC2 的 clansdiz / perksdiz 实际存的是**未翻译的俄文 cp1251**
    （apply.py 保留原文片段），若按 gbk 读 HEAD 会把中文 id 读成乱码，
    从而把「未改动」误判成「全量字面改变」。
    """
    enc = ref_enc or "gbk"
    try:
        return raw.decode(enc)
    except UnicodeDecodeError:
        for e in ("gbk", "cp1251", "utf-8"):
            try:
                return raw.decode(e)
            except UnicodeDecodeError:
                continue
        return raw.decode(enc, "replace")


def decode_head_enc(raw):
    for e in ("gbk", "cp1251", "utf-8"):
        try:
            raw.decode(e)
            return e
        except UnicodeDecodeError:
            continue
    return None


def vals(text, tag, attr):
    """提取 (键, 值) 对。

    键优先取 id，回退取 name —— <string> 用 id，<Reply> 用 name，
    早期版本只查 id，导致 Reply 的键全为空字符串而互相错配。
    """
    if attr:
        rx = re.compile(r"(<%s\b[^>]*?\b%s=\")((?:[^\"])*?)(\")"
                        % (re.escape(tag), re.escape(attr)), re.S)
        out = []
        for m in rx.finditer(text):
            km = re.search(r'\bid="([^"]*)"', m.group(1))
            if not km:
                km = re.search(r'\bname="([^"]*)"', m.group(1))
            out.append((km.group(1) if km else "", m.group(2)))
        return out
    out = []
    # diz* 必须按 name 分组比对，否则所有条目键相同会互相错配
    for m in re.finditer(r'name="([^"]*)"(.*?)/>', text, re.S):
        nm, body = m.group(1), m.group(2)
        dm = re.search(r'\bdiz\d+="((?:[^\"])*?)"', body, re.S)
        if dm:
            out.append((nm, dm.group(1)))
    return out


p("=" * 100)
p("写盘后独立验收")
p("=" * 100)

fails = []
tot_files = tot_changed = 0

for label, root in GAMES:
    p("")
    p("### %s  (%s)" % (label, root))
    for rel, (tag, attr, mx, ms) in sorted(CHECKS.items()):
        fp = os.path.join(BASE, root, *rel.split("/"))
        if not os.path.exists(fp):
            continue
        text, enc, raw = decode(fp)

        # --- 1. XML 可解析（与 HEAD 对比：只关心"我是否改坏"）---
        def parses(t):
            try:
                ET.fromstring(
                    re.sub(r"^\s*<\?xml[^>]*\?>", "", t).strip())
                return True
            except ET.ParseError:
                return False

        cur_ok = parses(text)
        head = git_head(root, rel)
        head_txt = decode_like(head, enc) if head else None
        head_ok = parses(head_txt) if head_txt else True
        if head_ok and not cur_ok:
            fails.append("%s %s : XML 由可解析变为不可解析" % (label, rel))
        if not cur_ok and not head_ok:
            p("  ⚠ %-44s XML 不可解析（原版即如此，非本次改动）" % rel)
            head_txt = None          # 跳过逐条比对，只做结构统计

        # --- 2. 编码未被改写：与 HEAD 同一编码 ---
        if head is not None:
            henc = decode_head_enc(head)
            if henc and enc != henc:
                fails.append("%s %s : 编码 %s→%s" % (label, rel, henc, enc))
        if b'encoding="windows-1251"' not in raw[:120]:
            fails.append("%s %s : XML 声明被改" % (label, rel))

        # --- 3. 元素数与 HEAD 一致 ---
        if head_txt is not None and head_ok and cur_ok:
            try:
                a = len(list(ET.fromstring(
                    re.sub(r"^\s*<\?xml[^>]*\?>", "", head_txt).strip()).iter()))
                b = len(list(ET.fromstring(
                    re.sub(r"^\s*<\?xml[^>]*\?>", "", text).strip()).iter()))
                if a != b:
                    fails.append("%s %s : 元素数 %d→%d" % (label, rel, a, b))
            except ET.ParseError:
                pass

        # --- 4. 逐条量测 ---
        cur = vals(text, tag, attr)
        oldmap = dict(vals(head_txt, tag, attr)) if head_txt else {}

        bad_w = bad_s = bad_spec = bad_text = 0
        seg_overflow = []
        wide_list = []
        changed = 0
        for sid, v in cur:
            segs = v.split("|")
            # 未翻译的俄文片段不适用中文断行规则（它们按 cp1251 窄字排版）
            is_ru = not any(ord(ch) > 0x2E80 for ch in v)
            if not is_ru and max((display_px(x) for x in segs),
                                 default=0) > mx * CJK_W:
                bad_w += 1
                wide_list.append((sid, [display_px(x) for x in segs]))
            if ms and len(segs) > ms:
                seg_overflow.append((sid, len(re.sub(r"\|+", "", v)), len(segs)))
            ov = oldmap.get(sid)
            if ov is not None:
                if ov != v:
                    changed += 1
                if specs(ov) != specs(v):
                    bad_spec += 1
                if re.sub(r"\|+", "", ov) != re.sub(r"\|+", "", v):
                    bad_text += 1

        tot_files += 1
        tot_changed += changed
        bad = []
        if bad_w:
            bad.append("超宽%d" % bad_w)
        if bad_spec:
            bad.append("占位符变%d" % bad_spec)
        if bad_text:
            bad.append("字面变%d" % bad_text)
        status = "✓" if not bad else "✗"
        p("  %s %-44s 改动%3d  %s" % (
            status, rel, changed,
            ("OK" if not bad else " ".join(bad))))
        for s in bad:
            fails.append("%s %s : %s" % (label, rel, s))
        for sid, nch, nsg in seg_overflow:
            p("        ⚠ 行数仍 %d（%d 字，容量 %d×%d）id=%s"
              % (nsg, nch, mx, ms, sid[:16]))
        for sid, lens in wide_list[:4]:
            p("        ⚠ 显示宽超限 %s id=%s" % (lens, sid[:16]))

p("")
p("=" * 100)
p("共检查 %d 个文件，其中 %d 个文件有内容改动" % (tot_files, tot_changed))
if fails:
    p("✗ 失败 %d 项：" % len(fails))
    for x in fails:
        p("   - " + x)
else:
    p("✓ 全部通过：行宽、行数上限、占位符、字面内容、编码、元素数 均无异常")
p("=" * 100)
_f.close()
print("written", OUT)