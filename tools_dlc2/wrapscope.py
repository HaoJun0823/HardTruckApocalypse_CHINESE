# -*- coding: utf-8 -*-
"""断行范围白名单。

只处理「玩家会看到的长段文字」：
  · 剧情对话流      maps/dv*/strings.xml（一条一个 string）
  · 关卡开场介绍    levelinfo/levelinfo.xml 的 diz0..dizN
  · 动态对话        diz/dynamicdialogsglobal.xml 的 text
  · 过场提示        strings/truxx.xml、loadtips.xml、fadingmsgs.xml
  · 对话框长文本    dialogs/*wnd.xml 的 caption 等（DLC1 已断的同款不算）

明确不处理：
  · perksdiz / clansdiz / objectdiz  —— 称号/势力/物品的描述短句，
    DLC1 同款也没断行，UI 本就单行显示
  · credits.xml     —— 用户要求不翻译
  · uieditstrings / bindnames / gamestrings —— 零碎 UI 字符串
"""

# 只断这些文件里的这些属性
WRAP_FILES = {
    # 关卡剧情对话流
    'data/maps/dv1/strings.xml': ('string', 'value'),
    'data/maps/dv2/strings.xml': ('string', 'value'),
    'data/maps/dv4/strings.xml': ('string', 'value'),
    'data/maps/dv5/strings.xml': ('string', 'value'),
    'data/maps/dv6/strings.xml': ('string', 'value'),
    'data/maps/dv7/strings.xml': ('string', 'value'),
    'data/maps/dv8/strings.xml': ('string', 'value'),
    'data/maps/dv9/strings.xml': ('string', 'value'),
    # 关卡开场介绍
    'data/if/levelinfo/levelinfo.xml': ('LevelInfo', None),      # None = 全部 diz* 属性
    # 动态对话
    'data/if/diz/dynamicdialogsglobal.xml': ('Reply', 'text'),
    # 过场/提示
    'data/if/strings/truxx.xml': ('string', 'value'),
    'data/if/strings/loadtips.xml': ('string', 'value'),
    'data/if/strings/fadingmsgs.xml': ('string', 'value'),
    # 注意：dialogs/*wnd.xml 的 caption 全部是 ^LocalKey^ 本地化键引用，
    # 断行会破坏查表，已确认排除（见 _istext.has_hazard）。
}

# levelinfo 里的 diz 系列
DIZ_ATTRS = ['diz%d' % i for i in range(0, 12)]


def in_scope(rel, tag, attr):
    spec = WRAP_FILES.get(rel)
    if not spec:
        return False
    wtag, wattr = spec
    if wtag and tag != wtag:
        return False
    if wattr is None:                       # 该文件全部 diz*
        return attr in DIZ_ATTRS
    return attr == wattr