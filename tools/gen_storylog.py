# -*- coding: utf-8 -*-
"""生成剧情/任务日志文档。

从游戏数据直接导出，不臆造内容：
  · 关卡清单      data/if/(diz|levelinfo)/levelinfo.xml  的 <LevelInfo name fullName>
  · 主线任务      data/if/diz/questinfoglobal.xml 的 <QuestInfo briefDiz fullDiz isMainQuest>
  · 派系          data/if/strings/clansdiz.xml 的 Belong_* / *_diz
  · 城镇与地点     data/if/strings/objectdiz.xml 的 <城镇>_diz
  · 剧情日志条目   data/if/strings/uihistory.xml

用法：
    python tools/gen_storylog.py            # 写 docs/剧情日志.md
    python tools/gen_storylog.py --preview  # 只打印统计
"""
from __future__ import print_function

import argparse
import os
import re
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(encoding="utf-8")

GAMES = [
    ("本体 · 燃烧飞车：末日浩劫", "Original_DATA_CHS", "hta.exe"),
    ("资料片 1 · 部落崛起", "DLC1_DATA_CHS", "Meridian113.exe"),
    ("资料片 2 · 街机版", "DLC2_DATA_CHS", "emarcade.exe"),
]


def decode(fp):
    raw = open(fp, "rb").read()
    for e in ("gbk", "cp1251", "utf-8"):
        try:
            return raw.decode(e)
        except UnicodeDecodeError:
            continue
    return raw.decode("gbk", "replace")


def strip_nl(s):
    """去掉断行分隔符，还原成连续文本。"""
    return re.sub(r"\|+", "", s or "").strip()


def load(fp):
    if not os.path.exists(fp):
        return None
    return re.sub(r"<!--.*?-->", "", decode(fp), flags=re.S)


# --------------------------------------------------------------------------
def levels(root):
    out = []
    for rel in ("data/if/diz/levelinfo.xml", "data/if/levelinfo/levelinfo.xml"):
        t = load(os.path.join(BASE, root, *rel.split("/")))
        if not t:
            continue
        for m in re.finditer(r"<LevelInfo\b.*?/>", t, re.S):
            b = m.group(0)
            nm = re.search(r'\bname="([^"]*)"', b)
            fn = re.search(r'\bfullName="([^"]*)"', b)
            if nm:
                out.append((nm.group(1), fn.group(1) if fn else ""))
        if out:
            break
    return out


def quests(root):
    t = load(os.path.join(BASE, root, "data", "if", "diz",
                          "questinfoglobal.xml"))
    if not t:
        return [], []
    main, side = [], []
    for m in re.finditer(r"<QuestInfo\b.*?</QuestInfo>", t, re.S):
        b = m.group(0)
        qn = re.search(r'\bquestName="([^"]*)"', b)
        br = re.search(r'\bbriefDiz="([^"]*)"', b)
        fu = re.search(r'\bfullDiz="([^"]*)"', b)
        im = re.search(r'\bisMainQuest="([^"]*)"', b)
        maps = re.findall(r"<Map\b.*?name=\"([^\"]*)\"", b, re.S)
        rec = {
            "name": qn.group(1) if qn else "",
            "brief": strip_nl(br.group(1) if br else ""),
            "full": strip_nl(fu.group(1) if fu else ""),
            "maps": sorted(set(maps)),
        }
        if not rec["brief"] and not rec["full"]:
            continue
        (main if im and im.group(1) == "1" else side).append(rec)
    return main, side


def diz_entries(root, fname, want_suffix="_diz"):
    t = load(os.path.join(BASE, root, "data", "if", "strings", fname))
    if not t:
        return []
    out = []
    for m in re.finditer(r'<string\b[^>]*?value="([^"]*)"', t, re.S):
        idm = re.search(r'\bid="([^"]*)"', m.group(0)[:200])
        if not idm:
            continue
        sid = idm.group(1)
        val = strip_nl(m.group(1))
        if not val:
            continue
        out.append((sid, val))
    return out


def history(root):
    t = load(os.path.join(BASE, root, "data", "if", "strings",
                          "uihistory.xml"))
    if not t:
        return []
    out = []
    for m in re.finditer(r'<string\b[^>]*?value="([^"]*)"', t, re.S):
        idm = re.search(r'\bid="([^"]*)"', m.group(0)[:200])
        val = strip_nl(m.group(1))
        if val:
            out.append(((idm.group(1) if idm else "?"), val))
    return out


# --------------------------------------------------------------------------
def build():
    lines = []
    A = lines.append

    A("# 剧情与任务日志")
    A("")
    A("> 本文件由 `tools/gen_storylog.py` 从游戏数据直接导出，")
    A("> 内容以游戏内实际文本为准，未做任何补写或润色。")
    A("> 重新生成：`python tools/gen_storylog.py`")
    A("")
    A("**数据来源**")
    A("")
    A("| 内容 | 数据文件 |")
    A("|---|---|")
    A("| 关卡清单 | `data/if/diz/levelinfo.xml`、`data/if/levelinfo/levelinfo.xml` |")
    A("| 任务日志 | `data/if/diz/questinfoglobal.xml` |")
    A("| 派系 | `data/if/strings/clansdiz.xml` |")
    A("| 城镇与地点 | `data/if/strings/objectdiz.xml` |")
    A("| 剧情日志条目 | `data/if/strings/uihistory.xml` |")
    A("")

    summary = []
    for title, root, exe in GAMES:
        lv = levels(root)
        mn, sd = quests(root)
        cl = diz_entries(root, "clansdiz.xml")
        ob = diz_entries(root, "objectdiz.xml")
        hi = history(root)
        summary.append((title, root, exe, lv, mn, sd, cl, ob, hi))

    A("## 总览")
    A("")
    A("| 游戏 | 主程序 | 关卡 | 主线任务 | 支线任务 | 派系条目 | 地点条目 | 剧情日志 |")
    A("|---|---|---|---|---|---|---|---|")
    for title, root, exe, lv, mn, sd, cl, ob, hi in summary:
        A("| %s | `%s` | %d | %d | %d | %d | %d | %d |"
          % (title, exe, len(lv), len(mn), len(sd), len(cl), len(ob), len(hi)))
    A("")

    for title, root, exe, lv, mn, sd, cl, ob, hi in summary:
        A("---")
        A("")
        A("## %s" % title)
        A("")
        A("主程序：`%s`　译文目录：`%s`" % (exe, root))
        A("")

        if lv:
            A("### 关卡")
            A("")
            A("| 内部名 | 中文名 |")
            A("|---|---|")
            for nm, fn in lv:
                A("| `%s` | %s |" % (nm, fn or "—"))
            A("")

        if cl:
            A("### 派系")
            A("")
            for sid, val in cl:
                if sid.endswith("_abb") or "_abb" in sid:
                    continue
                A("- **%s**：%s" % (sid, val))
            A("")

        if ob:
            A("### 城镇与地点")
            A("")
            for sid, val in ob:
                A("- `%s`：%s" % (sid, val))
            A("")

        if mn:
            A("### 主线任务")
            A("")
            for q in mn:
                loc = "、".join("`%s`" % m for m in q["maps"]) or "—"
                A("#### %s" % (q["brief"] or q["name"]))
                A("")
                A("- 内部名：`%s`" % q["name"])
                A("- 地点：%s" % loc)
                if q["full"] and q["full"] != q["brief"]:
                    A("")
                    A("> %s" % q["full"])
                A("")

        if sd:
            A("### 支线任务")
            A("")
            A("共 %d 条。" % len(sd))
            A("")
            for q in sd:
                loc = "、".join("`%s`" % m for m in q["maps"]) or "—"
                A("- **%s** — `%s`（%s）" % (q["brief"] or "—", q["name"], loc))
            A("")

        if hi:
            A("### 剧情日志条目")
            A("")
            for sid, val in hi:
                A("#### `%s`" % sid)
                A("")
                A("> %s" % val)
                A("")

    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--out", default=os.path.join(BASE, "docs", "剧情日志.md"))
    args = ap.parse_args()

    md = build()
    if args.preview:
        sys.stdout.write(md)
        return 0

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(md)
    print("已写出 %s（%d 字符）" % (args.out, len(md)))
    return 0


if __name__ == "__main__":
    sys.exit(main())