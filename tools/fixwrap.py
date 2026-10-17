# -*- coding: utf-8 -*-
"""统一断行器：把长中文段落切成引擎能显示的行。

背景（全部经实测，见 docs/2026-10-17-断行修复.md）
------------------------------------------------------------------
CJK 是等宽推进：adv = cellW 像素，cellW = round(height/72×120)。
主字号 height=12.000 ⇒ cellW=20px。于是每个控件的行容量是确定的：

| 界面 / 控件                          | 宽度 | cellW | 每行字数 |
|--------------------------------------|------|-------|---------|
| encyclopaediawnd:wndDiz（百科描述）    | 557  | 20    | 27      |
| encyclopaediawnd:subjectList（条目名） | 286  | 20    | 14      |
| localmapwnd:txtObjectDiz（地图城镇）   | 304  | 20    | 15      |
| cinemapanel:wndText（过场字幕）        | 564  | 20    | 28      |
| cinemapanel:wndScrollText（过场滚动） | 896  | 20    | 44      |
| historywnd:txtFrame（剧情日志）        | 904  | 20    | 45      |

过场字幕 wndText 只有 100px 高，行高 20px ⇒ **只能显示 5 行**。
用户要求剧情文本切在 **4 行以内**、每行 **20 字以内**（最低分辨率实机验证）。

数据格式：引擎用 `|` 作为强制换行分隔符，且 `||` 表示空行
（uidescription.xml 里已大量使用，保留原样）。

用法
------------------------------------------------------------------
    python tools/fixwrap.py --dry-run          # 只报告，不写盘
    python tools/fixwrap.py --apply            # 实际写入 *_DATA_CHS

安全设计
------------------------------------------------------------------
1. 输出永远从对应 *_DATA_CHS 现有文件派生，只替换属性值的字节区间，
   标签结构、属性顺序、其他语言片段零改动。
2. **占位符闸门**：%1s / %2d 等格式符在断行前后必须完全一致，
   且断点不得落在占位符内部。
3. **不跨越既有 |**：已经手工分好段的文本，其段内不再重排（除非该段仍超宽）。
4. **中文才断行**：全是 ASCII 的片段（数字、代号、按键名）原样保留。
5. **幂等**：已满足约束的文件二次运行不产生任何改动。
6. 写盘前逐文件做 XML 可解析 + 元素数一致校验，不通过则拒绝写入。
"""
from __future__ import print_function

import argparse
import codecs
import os
import re
import sys
import xml.etree.ElementTree as ET

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------
# 行宽档位：每行字数（含中文标点），按实测控件宽度向下取整
# --------------------------------------------------------------------------
GAMES = [
    ("本体", "Original_DATA_CHS"),
    ("DLC1", "DLC1_DATA_CHS"),
    ("DLC2", "DLC2_DATA_CHS"),
]

CELL_W = 20          # 主字号 height=12.000 的 CJK 单元宽（DPI=120）

# 断行档：名字 -> 每行最大字数
PROFILE = {
    # ---- 问题1：百科/地图描述 ----
    #   encyclopaediawnd:wndDiz  557px → 27；留 1 字余量避免贴边
    "pedia": 26,
    #   localmapwnd:txtObjectDiz 304px → 15
    "town": 14,
    # ---- 问题2：过场 / 剧情对话（cinemapanel:wndText）----
    #   wndText 宽 564px；用户实机验证 20 字安全
    "cine": 20,
    # ---- 可滚动的长文本窗（cinemapanel:wndScrollText / historywnd:txtFrame）----
    #   896px 宽、512px 高，带滚动条，不受行数限制，只限行宽
    "scroll": 44,
}

MAX_SEGMENTS = {
    # 过场字幕 wndText 只有 100px 高 = 5 行，用户要求切在 4 行以内
    "cine": 4,
    # 其余控件可滚动，不限段数
    "pedia": 0,
    "town": 0,
    "scroll": 0,
}

# --------------------------------------------------------------------------
# 作用域白名单：(相对路径, 标签, 属性, 档位)
# 相对路径以 *_DATA_CHS 为根，正斜杠分隔
# --------------------------------------------------------------------------
SCOPE = [
    # ===== 问题1-A：百科中的派系描述 =====
    ("data/if/strings/clansdiz.xml",   "string", "value", "pedia"),
    # ===== 问题1-B：地图中的城镇/地点描述 =====
    ("data/if/strings/objectdiz.xml",  "string", "value", "town"),
    # 城镇描述同时出现在百科条目里，用较宽的档位再跑一遍
    ("data/if/strings/objectdiz.xml",  "string", "value", "pedia"),
    # ===== 顺带：称号描述（实测最大 70px，DLC2 有 7 条溢出）=====
    ("data/if/strings/perksdiz.xml",   "string", "value", "pedia"),

    # ===== 问题2：过场 / 剧情对话流 =====
    ("data/maps/r1m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r1m2/strings.xml", "string", "value", "cine"),
    ("data/maps/r1m3/strings.xml", "string", "value", "cine"),
    ("data/maps/r1m4/strings.xml", "string", "value", "cine"),
    ("data/maps/r2m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r2m2/strings.xml", "string", "value", "cine"),
    ("data/maps/r3m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r3m2/strings.xml", "string", "value", "cine"),
    ("data/maps/r4m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r4m2/strings.xml", "string", "value", "cine"),
    ("data/maps/r5m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r5m2/strings.xml", "string", "value", "cine"),
    ("data/maps/r6m1/strings.xml", "string", "value", "cine"),
    ("data/maps/r6m2/strings.xml", "string", "value", "cine"),
    ("data/maps/sin/strings.xml",  "string", "value", "cine"),
    ("data/maps/dv1/strings.xml", "string", "value", "cine"),
    ("data/maps/dv2/strings.xml", "string", "value", "cine"),
    ("data/maps/dv4/strings.xml", "string", "value", "cine"),
    ("data/maps/dv5/strings.xml", "string", "value", "cine"),
    ("data/maps/dv6/strings.xml", "string", "value", "cine"),
    ("data/maps/dv7/strings.xml", "string", "value", "cine"),
    ("data/maps/dv8/strings.xml", "string", "value", "cine"),
    ("data/maps/dv9/strings.xml", "string", "value", "cine"),

    # ===== 顺带：动态对话（可滚动窗，不受行数限制）=====
    ("data/if/diz/dynamicdialogsglobal.xml", "Reply", "text", "scroll"),
]

# 关卡介绍 diz0..diz11（标签是 LevelInfo，属性名按 diz%d 匹配）
LEVELINFO_DIZ = ["diz%d" % i for i in range(0, 12)]

# --------------------------------------------------------------------------
# 断行核心
# --------------------------------------------------------------------------
SENT = u"。！？!?"
CLAUSE = u"，、；：,;:"
TAIL = u"」』）】》”’\""
# 禁止出现在行首的字符
NOHEAD = set(u"。！？；：、）】》」』”’，,.!?;:")

W_DEV = 0.6           # 长度偏离目标的惩罚
W_PUNCT_BONUS = -6.0  # 断在句末标点后
W_HARD = 6.0          # 不得不硬断
W_SHORT = 8.0         # 行尾过短的惩罚（过大会把每行都压短，实测会退化成 14/14/14）
W_HEAD = 400.0       # 禁头标点（本行行首或下一行行首）—— 近似硬禁
W_TAILONLY = 25.0     # 只剩标点的行
W_OVERMAX = 40.0      # 超过每行上限（保留兼容，硬闸门在 wrap_seg 里已先拦）

_LATIN = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "0123456789-_./:")
_PLACE = re.compile(r"%\d*(?:\$\d+)?[-+ #0]*[\d.]*[a-zA-Z%]")

# --------------------------------------------------------------------------
# 最小中文词典：禁止在词内断行
#
# 实测问题（town 档 14 字时出现过）：
#   「这间加油站以合理的价格提 | 供非常有用的服务。」  ← "提供" 被劈开
#   「靠盗匪活动、赌博和与邻近 | 村庄的贸易维生…」    ← "邻近" 被劈开
#   「除了那个难念的名字，这座城 | 市再没什么…」       ← "城市" 被劈开
# 只收录在本项目译文里高频出现的双字词，避免误伤正常搭配。
# --------------------------------------------------------------------------
_MIN2 = (
    "提供 供应 出现 发生 属于 位于 来自 带来 带走 成为 进行 参加 "
    "看到 听到 想到 感到 觉得 认为 希望 需要 应该 可以 能够 "
    "必须 可能 也许 或许 显然 当然 于是 然后 因此 所以 但是 可是 "
    "虽然 尽管 如果 假如 除非 除了 关于 对于 至于 由于 为了 "
    "通过 经过 根据 按照 随着 只要 只有 无论 不管 "
    "得到 获得 失去 剩下 出来 起来 下去 上来 回去 过来 "
    "开始 结束 继续 停止 离开 到达 回来 出去 进来 "
    "知道 认识 了解 明白 理解 发现 寻找 找到 "
    "看清 听完 说完 讲完 读完 写完 做完 "
    "城市 乡村 居民 敌人 友人 车辆 卡车 城镇 村庄 "
    "邻居 邻近 附近 遥远 靠近 接近 远离 "
    "任务 目标 计划 行动 决定 建议 "
    "世界 国家 民族 人民 人们 人类 生命 生活 "
    "武器 火力 装甲 结构 性能 型号 系统 设备 装置 "
    "货物 商品 金钱 货币 交易 买卖 价格 价值 "
    "游戏 战斗 战争 和平 秩序 规则 法律 力量 "
    "道路 公路 桥梁 建筑 房屋 房间 地区 区域 "
    "朋友 伙伴 同伴 对手 首领 领袖 队长 战士 "
    "医生 技师 工人 农民 商人 司机 学者 专家 "
    "地方 位置 中心 边缘 角落 表面 里面 外面 "
    "机会 问题 情况 条件 环境 空气 地面 水面 "
    "时间 时刻 期间 时期 时代 年代 "
    "喜欢 讨厌 害怕 担心 放心 安心 关心 用心 "
    "重要 主要 次要 必要 需求 供给 "
    "效率 效果 影响 作用 意义 意思 意见 "
    "发展 变化 改变 改进 改善 提高 增加 "
    "生产 制造 加工 修理 建设 建造 破坏 摧毁 "
    "保护 防守 进攻 攻击 撤退 逃跑 追赶 跟随 "
    "统治 管理 领导 组织 分配 安排 处理 解决 "
    "帮助 支持 反对 拒绝 接受 同意 承诺 宣布 "
    "打开 关闭 启动 安装 拆卸 维护 "
    "运输 搬运 携带 装载 卸载 堆积 存储 保存 "
    "农业 商业 工业 军事 科技 经济 政治 "
    "父亲 母亲 儿子 女儿 哥哥 弟弟 姐姐 妹妹 "
    "朋友 同事 部下 上司 队伍 组织 团体 "
    "读书 写字 说话 听话 看报 休息 "
    "汽车 火车 飞机 轮船 道路 桥梁 "
    "东西 南北 上下 左右 前后 里外 内外 "
    "许多 很多 少量 大量 一些 一点 全部 所有 "
    "非常 十分 特别 相当 比较 稍微 略微 "
    "正在 已经 曾经 将要 马上 立刻 终于 仍然 依然 "
)

_WORDS = set(w for w in _MIN2.split() if len(w) == 2)

W_WORD = 120.0       # 双字词被劈开


def splits_word(s, j):
    """断点 j 是否切开了一个双字词。

    j 位于 s[j-1] 与 s[j] 之间。检查 s[j-2:j] 与 s[j:j+2]：
      · 「提|供」——s[j-2:j]=='提' 不是词，但 s[j-1:j+1]=='提供' 是词 ⇒ 切开
      · 「这座城|市」——s[j-1:j+1]=='城市' 是词             ⇒ 切开
      · 「以|农」——s[j-1:j+1]=='以农' 非词               ⇒ 不切开
    """
    if j < 1 or j >= len(s):
        return False
    pair = s[j - 1:j + 1]          # 跨断点的那个双字组合
    return pair in _WORDS


def is_cjk(s):
    """含中日韩字符才值得断行。"""
    for ch in s:
        if ord(ch) > 0x2E80:
            return True
    return False


# 主字号 height=12.000 ⇒ CJK 单元 20px；SM_Tahoma 12 的半角字符约 6~11px。
# 断行器内部一律用「显示像素宽」判断是否超限，而不是字符数——
# 否则 "[+3]  - 在城市修理厂…" 这类半角前缀会被误判成超宽而切错位置。
ASCII_W = 9.0


def disp(s):
    """一段文本的显示宽度（像素）。"""
    return sum(float(CELL_W) if ord(ch) > 0x2E80 else ASCII_W for ch in s)


def fmt_specs(s):
    """抽取格式占位符集合（%% 不算）。"""
    return sorted(m.group(0) for m in _PLACE.finditer(s) if m.group(0) != "%%")


def place_spans(s):
    """返回占位符的 (start, end) 区间列表。"""
    return [(m.start(), m.end()) for m in _PLACE.finditer(s)]


def can_break(s, j):
    """断点位于 s[j-1] 与 s[j] 之间时是否允许断开。"""
    n = len(s)
    if j < 1 or j > n - 1:
        return True
    a, b = s[j - 1], s[j]
    # 不可切断：拉丁词、数字串、百分比
    if a in _LATIN and b in _LATIN:
        return False
    if a in _LATIN and not b.isspace() and ord(b) > 0x2E80:
        return False
    if ord(a) > 0x2E80 and b in _LATIN:
        return False
    if a == " " or b == " ":
        # 空格两侧的拉丁词整体不可拆
        if b == " ":
            k = j - 2
            while k >= 0 and s[k] in _LATIN:
                k -= 1
            if k < j - 2:
                return False
        if a == " ":
            k = j + 1
            while k < n and s[k] in _LATIN:
                k += 1
            if k > j + 1:
                return False
    if a.isdigit() and b in u"%％":
        return False
    if a in u"%％" and b.isdigit():
        return False
    # 占位符整体是不可切原子
    for (st, en) in place_spans(s):
        if st < j < en:
            return False
        if st == j and j > 0 and s[j - 1].isdigit():
            return False
    return True


def wrap_seg(s, max_chars, target=None):
    """把单个片段切成显示宽 ≤max_chars×CELL_W 的行，DP 求全局最优断点。

    max_chars 是「汉字当量」：一行能放几个全角字。
    半角字符只占 ASCII_W，所以 "[+3]  - 在城市修理厂…" 这类含半角前缀的
    文本不会被误切。
    """
    n = len(s)
    max_px = max_chars * float(CELL_W)
    if disp(s) <= max_px:
        return [s]
    target = target or max(4, max_chars - 2)
    target_px = float(target * CELL_W)

    INF = float("inf")
    cost = [INF] * (n + 1)
    prev = [-1] * (n + 1)
    cost[0] = 0.0

    for i in range(1, n + 1):
        if cost[i - 1] == INF:
            continue
        # 最多再吃 max_chars 个全角字；半角更窄，用 +2 留余量后
        # 由下面的 wpx <= max_px 硬闸门把超宽方案直接排除。
        for L in range(1, min(max_chars + 2, n - i + 1) + 1):
            j = i - 1 + L
            if j > n:
                break
            line = s[i - 1:j]
            wpx = disp(line)
            if wpx > max_px:
                # 硬约束：单行显示宽不得超过上限，超了就跳过该转移。
                # （早前版本只加惩罚项，DP 仍会选超限行 → 出现 21 字/420px 的行）
                continue
            c = cost[i - 1] + W_DEV * (wpx - target_px) ** 2
            if len(line) >= 2 and line[0] in NOHEAD:
                c += W_HEAD
            # 断点正后方的字符 = 下一行的行首。该字符若是收尾标点，
            # 就是中文排版忌讳的「行首禁头」（。！，』）。
            # 早前版本只惩罚 line[0]（本行行首），漏掉了这一侧，
            # 于是 DP 会为了凑满行宽而把句号顶到下一行开头。
            if j < n and s[j] in NOHEAD:
                c += W_HEAD
            if not can_break(s, j):
                c += W_HEAD * 2
            if splits_word(s, j):
                c += W_WORD
            last = line[-1]
            if last in SENT:
                c += W_PUNCT_BONUS
            elif last in CLAUSE:
                c += W_PUNCT_BONUS * 0.6
            else:
                c += W_HARD
            if len(line) <= 2 and line[0] in SENT + CLAUSE:
                c += W_TAILONLY
            if j < n and wpx < target_px:
                c += W_SHORT
            if c < cost[j]:
                cost[j] = c
                prev[j] = i - 1

    if cost[n] == INF:
        # 无可行解（整段由不可切的原子构成，如长 URL / 长数字串）：
        # 退化为按显示宽贪心切，保证单行仍不超宽。
        out = []
        cur = []
        for ch in s:
            trial = "".join(cur) + ch
            if cur and disp(trial) > max_px:
                out.append("".join(cur))
                cur = [ch]
            else:
                cur.append(ch)
        if cur:
            out.append("".join(cur))
        return out

    lines = []
    j = n
    while j > 0:
        i = prev[j]
        lines.append(s[i:j])
        j = i
    lines.reverse()
    return lines


def is_speaker_line(s):
    """判断一段是否是「角色名」而不是台词正文。

    依据（实测 r2m1/r1m3/r5m1/dv1 的 strings.xml）：
      · 角色名单独成段，通常 ≤12 字，不以句末标点结尾
      · 台词正文以句读收尾，或明显更长
    """
    t = s.strip()
    if not t:
        return False
    if len(t) > 12:
        return False
    return t[-1] not in SENT + CLAUSE + u"!?,;:"


def reflow_preserving_head(v, max_chars, max_segs):
    """重新流动，但保留开头的「角色名」段。

    r2m1 id=4 实测：
        旧(5行): 陌生人 ⏎ 这么说你不是派来煽动的？ ⏎ 该死，…
        若直接 flatten，角色名会与首句粘连成「陌生人这么说你…」，
    说话人标签就此丢失。正确做法是把第一段固定为独立行。
    """
    segs = v.split("|")
    if len(segs) < 2 or not is_speaker_line(segs[0]):
        return None
    head = segs[0].strip()
    body = "|".join(segs[1:])

    if any(not s for s in segs[1:]):        # 正文含空段，不能重排
        return None

    flat = body.replace("|", "")
    if not is_cjk(flat) or not flat:
        return None

    lines = wrap_seg(flat, max_chars)
    return "|".join([head] + lines)


def wrap_value(v, max_chars, max_segs):
    """对整条属性值断行。返回 (新值, 是否行数仍超限)。

    最小侵入原则：**只在违反约束时才动手**，且优先保留原作者的断句。

    1. 含空段（`||`，uidescription.xml 的分节标记）
       空段是刻意分节，一律保留；只在每个非空段内部按 max_chars 切分。
    2. 不含空段
       · 全部行都 ≤max_chars 且行数 ≤max_segs  → 原样返回（绝大多数条目）
       · 有行 >max_chars                          → 该行内部切分
       · 行数 >max_segs                            → 重新流动重排
         若首段是角色名，则保留为独立行，绝不与台词粘连。
    硬约束：任何一行都不得长于 max_chars。
    软约束：行数不超过 max_segs；总字数装不下时保留更多行（宁可被 UI
    裁掉，也不行内超宽导致横向溢出），并如实报告。
    """
    segs = v.split("|")

    # ---- 情况 1：含空段 → 保留分节，只在段内切 ----
    if any(not s for s in segs):
        out = []
        for sg in segs:
            if not sg:
                out.append(sg)
            elif disp(sg) <= max_chars * CELL_W or not is_cjk(sg):
                out.append(sg)
            else:
                out.extend(wrap_seg(sg, max_chars))
        return "|".join(out), bool(max_segs) and len(out) > max_segs

    # ---- 情况 2：无空段 ----
    # 带 msgType（SCROLL 等）或 fontSize 的条目走的是不同控件/字号，
    # 行容量与 wndText 无关，交由 --skip-msgtype 排除，不在此处理。
    wide = [i for i, s in enumerate(segs)
            if disp(s) > max_chars * CELL_W]
    too_many = bool(max_segs) and len(segs) > max_segs

    if not wide and not too_many:
        return v, False                       # 已合规，不动

    if wide and not too_many:
        # 只需修超宽行：就地切分，保留其余断句
        out = []
        for sg in segs:
            if disp(sg) > max_chars * CELL_W and is_cjk(sg):
                out.extend(wrap_seg(sg, max_chars))
            else:
                out.append(sg)
        return "|".join(out), False

    # 行数超限（或同时有超宽行）：重新流动。
    # 先尝试保住角色名首段。
    kept = reflow_preserving_head(v, max_chars, max_segs)
    if kept is not None:
        n = len(kept.split("|"))
        return kept, n > max_segs

    flat = v.replace("|", "")
    if not is_cjk(flat):
        return v, too_many

    # 全部按「显示像素宽」估算：全角计 CELL_W，半角计 ASCII_W
    n_px = disp(flat)
    cap_px = max_chars * float(CELL_W) * max_segs if max_segs else None
    if cap_px and n_px > cap_px:
        # 装不下：按 max_chars 硬切，如实报告行数超限
        lines = wrap_seg(flat, max_chars)
        return "|".join(lines), True

    # 装得下：求一个能把总宽压进 max_segs 行的每行宽度
    min_w = int(n_px / (CELL_W * float(max_segs))) + 1
    best_w = max_chars
    for w in range(max_chars, max(4, min_w) - 1, -1):
        lines = wrap_seg(flat, w)
        if len(lines) <= max_segs:
            best_w = w
            break
    lines = wrap_seg(flat, best_w)
    return "|".join(lines), len(lines) > max_segs


# --------------------------------------------------------------------------
# XML 属性值的安全替换（只动 value/text 的字节区间）
# --------------------------------------------------------------------------
_ATTR_RE = None


def attr_re(tag, attr):
    """匹配 <tag ... attr="VALUE" ...> 的自闭合元素。"""
    return re.compile(
        r"(<%s\b[^>]*?\b%s=\")((?:[^\"]|\"\s)*?)(\")" % (re.escape(tag),
                                                       re.escape(attr)),
        re.S)


# 标记「走别的控件/字号」的条目。实测 maps/*/strings.xml 里 msgType="SCROLL"
# 仅 3 条（本体 r1m1/r1m3/r2m1 的 id=77771），它们 fontSize=18（cellW=31）
# 且由引擎按 SCROLL 模式渲染，与 wndText 的 20 字/4 行容量无关。
_SPECIAL_ATTR = re.compile(r'\b(?:msgType|fontSize|scrollSpeed|numButtons)=')


def decode(fp):
    with open(fp, "rb") as f:
        raw = f.read()
    for enc in ("gbk", "cp1251", "utf-8"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("gbk", "replace"), "gbk"


def encode(text, enc):
    return text.encode(enc, "replace")


def process_value(text, max_chars, max_segs, report):
    """返回 (新值, 是否改动, 是否行数超限)。"""
    before_segs = text.split("|")
    mx = max_chars * CELL_W
    before = (max((disp(x) for x in before_segs) if before_segs else (0, 0)),
              len(before_segs))
    if before[0] <= mx and (not max_segs or before[1] <= max_segs):
        return text, False, False            # 已合规
    if not is_cjk(text):
        return text, False, False
    new, overflow = wrap_value(text, max_chars, max_segs)
    if fmt_specs(new) != fmt_specs(text):
        report.append("    [拒绝] 占位符集合变化：%r" % text[:60])
        return text, False, False
    if re.sub(r"\|+", "", new) != re.sub(r"\|+", "", text):
        report.append("    [拒绝] 字面内容变化：%r" % text[:60])
        return text, False, False
    return new, new != text, overflow


def wrap_file(fp, tag, attr, max_chars, max_segs, apply, report):
    """对一个文件做断行。返回 (改动条目数, 行数仍超限条目数)。"""
    if not os.path.exists(fp):
        return 0, 0
    text, enc = decode(fp)
    original = text
    rx = attr_re(tag, attr)
    whole = re.compile(r"<%s\b[^>]*?/>" % re.escape(tag), re.S)

    changed = 0
    overflow_ids = []
    skipped = []

    def repl(m):
        nonlocal changed
        val = m.group(2)
        idm = re.search(r'\bid="([^"]*)"', m.group(1))
        # 排除带 msgType / fontSize / scrollSpeed / numButtons 的条目：
        # 它们由引擎按自定义字号与 msgType 走**别的控件**（如 SCROLL 型
        # 走 wndScrollText、fontSize=18 ⇒ cellW=31；带 numButtons 的走
        # 对话选项窗），行容量与 wndText 的 20 字/4 行无关。
        # 原版已按该字号/控件排好，不应按 wndText 档位重排。
        # 注意：这些属性常排在 value= 之后，必须检查**整个元素**而非 m.group(1)。
        elem = whole.search(text, max(0, m.start() - 4))
        if elem and _SPECIAL_ATTR.search(elem.group(0)):
            skipped.append(idm.group(1) if idm else "?")
            return m.group(0)
        new, did, ovf = process_value(val, max_chars, max_segs, report)
        if did:
            changed += 1
            if ovf:
                overflow_ids.append(
                    (idm.group(1) if idm else "?",
                     len(re.sub(r"\|+", "", new)),
                     len(new.split("|"))))
            return m.group(1) + new + m.group(3)
        return m.group(0)

    out = rx.sub(repl, text)

    if not changed:
        for sid in skipped:
            report.append("    [跳过] id=%-22s 带 msgType/fontSize，走其他控件"
                          % sid[:22])
        return 0, 0

    for sid, nchar, nseg in overflow_ids:
        report.append("    [行数仍超限] id=%-22s %d 字 → %d 行（容量 %d 字 × %d 行）"
                      % (sid[:22], nchar, nseg, max_chars, max_segs))

    for sid in skipped:
        report.append("    [跳过] id=%-22s 带 msgType/fontSize，走其他控件"
                      % sid[:22])

    # ---- 安全闸门：XML 可解析 + 元素数一致 ----
    try:
        a = ET.fromstring(re.sub(r"^\s*<\?xml[^>]*\?>", "", original).strip())
        b = ET.fromstring(re.sub(r"^\s*<\?xml[^>]*\?>", "", out).strip())
    except ET.ParseError as e:
        report.append("    [拒绝] 改后 XML 无法解析：%s" % e)
        return 0, 0
    if len(list(a.iter())) != len(list(b.iter())):
        report.append("    [拒绝] 元素数不一致：%d vs %d"
                      % (len(list(a.iter())), len(list(b.iter()))))
        return 0, 0

    if apply:
        with open(fp, "wb") as f:
            f.write(encode(out, enc))
    return changed, len(overflow_ids)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="写盘（默认干跑）")
    ap.add_argument("--only", default="", help="只处理某个游戏，如 本体/DLC1/DLC2")
    args = ap.parse_args()
    apply_ = args.apply

    print("模式：%s" % ("写盘" if apply_ else "干跑（不写盘）"))
    print("CJK cellW=%dpx" % CELL_W)
    print()

    total_files = 0
    total_items = 0
    for label, root in GAMES:
        if args.only and args.only != label:
            continue
        print("=" * 78)
        print("### %s  (%s)" % (label, root))
        print("=" * 78)
        gchanged = 0
        gfiles = 0
        goverflow = 0
        seen = set()
        for rel, tag, attr, prof in SCOPE:
            if (rel, tag, attr) in seen:
                continue
            seen.add((rel, tag, attr))
            fp = os.path.join(BASE, root, *rel.split("/"))
            report = []
            n, ovf = wrap_file(fp, tag, attr, PROFILE[prof],
                               MAX_SEGMENTS[prof], apply_, report)
            if n:
                gfiles += 1
                gchanged += n
                goverflow += ovf
                print("  %-46s [%s] 改动 %3d 条" % (rel, prof, n))
                for r in report:
                    print(r)

        # 关卡介绍（标签 LevelInfo，属性 diz0..diz11）
        for rel in ("data/if/diz/levelinfo.xml", "data/if/levelinfo/levelinfo.xml"):
            fp = os.path.join(BASE, root, *rel.split("/"))
            if not os.path.exists(fp):
                continue
            report = []
            # LevelInfo 用 diz* 属性，单独处理
            text, enc = decode(fp)
            out = text
            cnt = 0
            overflow_ids = []
            for attr in LEVELINFO_DIZ:
                arx = re.compile(r"(\b%s=\")((?:[^\"])*?)(\")" % attr, re.S)
                _cnt = [0]

                def _repl(m, _a=attr):
                    val = m.group(2)
                    if not val.strip():
                        return m.group(0)
                    new, did, ovf = process_value(
                        val, PROFILE["scroll"], MAX_SEGMENTS["scroll"],
                        report)
                    if not did:
                        return m.group(0)
                    _cnt[0] += 1
                    if ovf:
                        overflow_ids.append(
                            (_a, len(re.sub(r"\|+", "", new)),
                             len(new.split("|"))))
                    return m.group(1) + new + m.group(3)

                out = arx.sub(_repl, out)
                cnt += _cnt[0]

            if out != text:
                for aid, nchar, nseg in overflow_ids:
                    report.append(
                        "    [行数仍超限] %-18s %d 字 → %d 行（容量 %d 字 × %d 行）"
                        % (aid, nchar, nseg, PROFILE["scroll"],
                           MAX_SEGMENTS["scroll"] or 0))
                try:
                    a = ET.fromstring(re.sub(r"^\s*<\?xml[^>]*\?>", "", text).strip())
                    b = ET.fromstring(re.sub(r"^\s*<\?xml[^>]*\?>", "", out).strip())
                    if len(list(a.iter())) != len(list(b.iter())):
                        report.append("    [拒绝] levelinfo 元素数不一致")
                        cnt = 0
                    elif apply_:
                        with open(fp, "wb") as f:
                            f.write(encode(out, enc))
                except ET.ParseError as e:
                    report.append("    [拒绝] levelinfo XML 解析失败：%s" % e)
                    cnt = 0
            if cnt:
                gfiles += 1
                gchanged += cnt
                print("  %-46s [scroll] 改动 %3d 处" % (rel, cnt))
                for r in report:
                    print(r)

        print("  小计：%d 个文件，%d 处改动" % (gfiles, gchanged))
        print()
        total_files += gfiles
        total_items += gchanged

    print("=" * 78)
    print("合计：%d 个文件，%d 处改动（%s）"
          % (total_files, total_items, "已写盘" if apply_ else "干跑"))
    return 0


if __name__ == "__main__":
    sys.exit(main())