# -*- coding: utf-8 -*-
"""
修复 CHS 翻译 XML 里的未转义字符。

════════════════════════════════════════════════════════════════════════
病因（引擎日志 exmachina.log 实证，不是猜测）
════════════════════════════════════════════════════════════════════════
    uiStrings.cpp[0051] WndStation::Create ReadXmlFile: Cannot parse file ...
    GameUiManager.cpp[1743] Interface: error load strings from file ...

=> 整个文件的字符串表加载失败 → 引擎查表失败 → 界面显示 "ID!MISSING"。

俄文原版用 « »（书名号，非 XML 特殊字符），汉化时写成了裸英文双引号 "，
而 " 在 XML 属性值里是**属性终止符**，必须写成 &quot;。

════════════════════════════════════════════════════════════════════════
修复策略：逐行状态机（不用正则）
════════════════════════════════════════════════════════════════════════
这些翻译文件的格式高度规整，属性总是写成：

        name="值" />
    或  name="值"

即：**开引号 = 第一个 " ；闭引号 = 行尾最后一个 "**。
中间的所有 " 都是值内部的裸引号，需要转义。

为什么不能用正则：属性值含裸引号时，`name="([^"]*)"` 会提前在第一个内部
引号处结束，导致后续全部错位 —— 这正是第一版脚本把标签结构也转义坏掉、
把 XML 声明搞坏的原因。

════════════════════════════════════════════════════════════════════════
安全边界
════════════════════════════════════════════════════════════════════════
· 只处理 name="..." 形式的属性行，绝不动 <?xml ...?> 声明和 <tag> 结构。
· 逐字节操作，GBK 字节原样保留（不做任何 decode/encode）。
· 修改前自动 .BAK 备份；改完立刻用 XML 解析器验证。
"""
import os
import re
import sys
import shutil

# 一个属性行：  空白 name="值" [/]> 可选尾随空白
ATTR_LINE = re.compile(
    rb'^(\s*)([A-Za-z_][A-Za-z0-9_:\-]*)(\s*=\s*")(.*)("\s*(?:/?>)?\s*)$',
    re.S)


def fix_attr_line(line: bytes, stats: dict) -> bytes:
    """修复单行属性。返回新行。"""
    m = ATTR_LINE.match(line)
    if not m:
        return line
    indent, name, eq, val, tail = m.groups()
    # val 内部：转义 " 和 <
    nq = val.count(b'"')
    if nq:
        val = val.replace(b'"', b'&quot;')
        stats['quot'] += nq
    # 先把已有的合法实体 & 保护起来，再处理裸 <，最后还原
    val = val.replace(b'&', b'\x01')
    nlt = val.count(b'<')
    if nlt:
        val = val.replace(b'<', b'&lt;')
        stats['lt'] += nlt
    # 裸 & （不以 # 或已知实体名开头）
    val = re.sub(rb'&(?!#\d+;|#x[0-9A-Fa-f]+;|amp;|lt;|gt;|quot;|apos;)',
                 b'&amp;', val)
    val = val.replace(b'\x01', b'&')
    return indent + name + eq + val + tail


def repair(data: bytes):
    out = bytearray()
    stats = {'quot': 0, 'lt': 0}
    # 按 \n 切分，保留行尾
    lines = data.split(b'\n')
    for i, ln in enumerate(lines):
        # 去掉可能的 \r
        had_cr = ln.endswith(b'\r')
        core = ln[:-1] if had_cr else ln
        newcore = fix_attr_line(core, stats)
        out += newcore
        if had_cr:
            out += b'\r'
        if i != len(lines) - 1:
            out += b'\n'
    return bytes(out), stats


def is_valid(data: bytes):
    try:
        import xml.dom.minidom
        xml.dom.minidom.parseString(data)
        return True, ''
    except Exception as e:
        return False, str(e)


def process(path: str, apply: bool):
    data = open(path, 'rb').read()
    ok, err = is_valid(data)
    if ok:
        return ('skip', 0, 0)
    new, stats = repair(data)
    ok2, err2 = is_valid(new)
    if not ok2:
        return ('fail', err2, stats)
    if apply:
        shutil.copy2(path, path + '.BAK')
        open(path, 'wb').write(new)
    return ('ok', stats, None)


def main():
    if len(sys.argv) < 2:
        print("用法: python fix_xml_escapes.py <目录> [--apply]")
        return 1
    root = sys.argv[1]
    apply = '--apply' in sys.argv
    targets = []
    for dp, dn, fns in os.walk(root):
        for fn in fns:
            if fn.lower().endswith('.xml'):
                targets.append(os.path.join(dp, fn))
    print("扫描 %d 个 xml  %s\n" % (len(targets), '(执行修复)' if apply else '(只读诊断)'))
    n_ok = n_fail = n_skip = 0
    for p in sorted(targets):
        st, a, b = process(p, apply)
        rel = os.path.relpath(p, root)
        if st == 'skip':
            n_skip += 1
        elif st == 'ok':
            n_ok += 1
            print("  [修复] %-52s quot=%-4d lt=%d" % (rel, a['quot'], a['lt']))
        else:
            n_fail += 1
            print("  [仍失败] %-50s %s" % (rel, str(a)[:60]))
    print("\n合法(跳过) %d   修复成功 %d   仍失败 %d" % (n_skip, n_ok, n_fail))
    return 0 if n_fail == 0 else 2


if __name__ == '__main__':
    sys.exit(main())
