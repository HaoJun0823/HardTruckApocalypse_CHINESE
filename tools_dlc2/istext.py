# -*- coding: utf-8 -*-
"""真正的长文本判定 —— 排除脚本/ID/坐标/文件名等非显示文本。

必须断行的属性（承载玩家可见的文字）：
  text value fullDiz briefDiz diz0..dizN name title comment text1 desc
排除（绝不能动）：
  scriptResult scriptCondition scriptAction nextReplies prevReplies
  org clientEdges x y width height scale rotate
  file locForm modelName targetObjName modelSkin modelCfg modelSlot sound
  backimage fontName paneName class role id name(当值是路径/ID)
"""
import re

# 明确排除的属性
NEVER = set('''scriptResult scriptCondition scriptAction scriptCommand
nextReplies prevReplies nextReply prevReply
org clientEdges x y width height scale rotate angle
file locForm modelName targetObjName modelSkin modelCfg modelSlot
sound backimage fontName paneName class role time numButtons
gamelevel splashes musicBlocks north south east west
onShow onHide onClick onKey event type varName varType'''.split())

# 玩家可见文本的属性
TEXTISH = set('''text value fullDiz briefDiz comment title desc caption
label tooltip header footer message text1 text2 name'''.split())

# 值里像代码/路径/ID 的特征
_CODE = re.compile(r'[A-Za-z_]\w*\s*\(|;\s*\w+\s*\(|::|\(\)|=\s*\d|^\s*[A-Za-z_]\w*\(')
_IDLIST = re.compile(r'^[A-Za-z_][\w:\.\-]*(\s+[A-Za-z_][\w:\.\-]*)*$')      # 纯标识符列表
_PATHY = re.compile(r'[\\/]|\.(xml|tga|dds|ogg|wav|ini|sav|ssl|bak)\b', re.I)
_NUMONLY = re.compile(r'^[\s\d\.\-]+$')
_URLISH = re.compile(r'^\s*(www\.|http|[A-Za-z]:\\)')
# 整串就是本地化键引用 ^Key^ —— 断行会破坏查表
_LOCALKEY = re.compile(r'^\^[^^]+\^$')
# 颜色控制码 @AARRGGBB@ / @RRGGBB@ 标记
_COLORCODE = re.compile(r'@[0-9A-Fa-f]{6,8}@')
# 格式占位符 %1s %1d %2d %1m %2b %s %d
# 引擎的类型字母不止 sdfm：DLC2 用到 %2b（帮派名）等，故放宽到单字母。
_PLACEHOLDER = re.compile(r'%\d*[a-zA-Z]')


def has_hazard(val):
    """值里含不得被换行破坏的结构：本地化键 / 颜色码。
    格式占位符 %2b 等**不在此列** —— 断行器已把它们当作不可切原子，
    整条文本仍可正常换行，只是占位符本身不会被拆开。"""
    v = val.strip()
    if _LOCALKEY.match(v):
        return '本地化键引用 ^Key^'
    if _COLORCODE.search(v):
        return '颜色控制码 @AARRGGBB@'
    return None


def is_display_text(tag, key, attr, val):
    """判断该属性值是否为玩家可见的长文本（可能需要断行）"""
    v = val.strip()
    if not v:
        return False
    if has_hazard(v):
        return False
    if attr in NEVER:
        return False
    # 值本身不像自然语言
    if _NUMONLY.match(v):
        return False
    if _PATHY.search(v):
        return False
    if _URLISH.match(v):
        return False
    if _CODE.search(v):
        return False
    # 纯标识符/ID 列表（如 Dlg_pl_000216 Dlg_pl_000426）
    if _IDLIST.match(v) and not re.search(r'[一-鿿]', v):
        return False
    # 属性名明确不是文本属性，且值里没有中文 -> 多半是 ID/路径
    if attr not in TEXTISH and not re.search(r'[一-鿿]', v):
        return False
    return True


def has_cjk(v):
    return bool(re.search(r'[一-鿿]', v))
