# -*- coding: utf-8 -*-
"""XML 属性采集公共模块（无副作用）。"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools_dlc2'))
import xlit

META = set('id name class locForm file encoding standalone backimage fontName '
           'paneName wndColor role modelName scriptCondition scriptAction '
           'targetObjName modelSkin modelCfg modelSlot sound time numButtons '
           'splashes musicBlocks gamelevel north south east west levelName '
           'version abc tcs wndHandle x y width height'.split())
WS = os.path.dirname(os.path.abspath(__file__))
MAX = 16
KEYATTRS = ('id', 'name', 'questName')


def harvest(tree, rel):
    """返回 {(tag,key,attr): 已unescape的值}"""
    p = os.path.join(WS, tree, *rel.split('/'))
    if not os.path.exists(p):
        return {}
    raw = xlit.read_bytes(p)
    try:
        t = xlit.mixed_decode(raw)
    except Exception:
        return {}
    t = re.sub(r'<!--.*?-->', '', t, flags=re.S)
    out = {}
    for m in re.finditer(r'<([A-Za-z_][\w:.\-]*)\b', t):
        tag = m.group(1)
        j = m.end()
        inq = False
        while j < len(t):
            c = t[j]
            if c == '"':
                inq = not inq
            elif c == '>' and not inq:
                break
            j += 1
        head = t[m.start():j]
        key = '?'
        for ka in KEYATTRS:
            km = re.search(r'\b%s="([^"]*)"' % ka, head)
            if km:
                key = km.group(1)
                break
        for am in re.finditer(r'([\w:.\-]+)\s*=\s*"([^"]*)"', head):
            a, v = am.group(1), am.group(2)
            if a in META or a in KEYATTRS or not v:
                continue
            out[(tag, key, a)] = xlit.unesc(v)
    return out


def list_xmls(tree):
    root = os.path.join(WS, tree)
    out = []
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.lower().endswith('.xml'):
                out.append(os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'))
    out.sort()
    return out


def needs_wrap(v, maxlen=MAX):
    lines = v.split('|')
    mx = max((len(x) for x in lines), default=0)
    return (mx > maxlen) or ('|' not in v and len(v) > maxlen)