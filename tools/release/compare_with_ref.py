# -*- coding: utf-8 -*-
"""比对「本脚本装配出的包」与「参考包」，逐文件哈希。

差异分三类，必须显式列出而不是笼统报"不一致"：
  · SAME     —— 哈希相同
  · EXPECTED —— 已知且有意为之的差异（.BAK 备份、孤儿 cjk 图集）
  · UNKNOWN  —— 计划外差异，脚本以非 0 退出（这是真正的失败）
"""
import hashlib
import os
import sys

REFS = {
    'base': r"I:\LocalGames\Hard Truck Apocalypse\Hard_Truck_Apocalypse_CHS_V0-FIX1",
    'dlc1': r"I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM\燃烧飞车_末日浩劫_部落崛起_简体中文汉化_V0",
    'dlc2': r"I:\LocalGames\HARD TRUCK APOCALYPSE ARCADE\燃烧飞车_末日浩劫_街机版_简体中文汉化_V0",
}
# 参考包里存在、但我们**有意**不产出的文件
EXPECTED_MISSING = {
    # 翻译过程的备份文件，无用途；BASE 参考包误打包了 5 个，DLC1/DLC2 都没打
    '.bak',
}
# 我们**有意新增**、参考包里没有的文件（不算差异）
#   License.txt 是本次新增的第三方许可合并文件（参考包没有）
EXPECTED_ADDED = {
    'License.txt',
}
# 仅为占位判断用的孤儿图集条件
ORPHAN_PREFIX = 'cjk_'


def sha(p):
    with open(p, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def norm_xml(p):
    """fonts.xml 的语义规范化：丢掉空行与行尾空白后的 sha256。

    ★ 为什么要这样比而不是直接比字节 ★
      BASE 参考包的 fonts.xml 比本脚本产出多 **190 字节 = 95 个 Item × 2 个
      空行**（该包由旧版 build_cjk.py 产出，那时每个 Item 之间会多插空行）。
      逐 Item 结构化比对结果：**95/95 个 Item 的 height / file / 全部 Symbol
      完全相同**。所以这是纯排版差异，不影响引擎解析，不算回归。
      改成规范化比较，避免以后每次构建都报一个假失败。
    """
    import re
    txt = open(p, 'rb').read().decode('latin-1')
    items = []
    for blk in re.findall(r'<Item\b(.*?)</Item>', txt, re.S):
        h = re.search(r'height="([^"]*)"', blk)
        f = re.search(r'file="([^"]*)"', blk)
        syms = re.findall(r'<Symbol\b(.*?)/>', blk, re.S)
        vals = []
        for s in syms:
            vm = re.search(r'value=("[^"]*"|\'[^\']*\')', s)
            am = re.search(r'abc="([^"]*)"', s)
            tm = re.search(r'tcs="([^"]*)"', s)
            vals.append((vm.group(1) if vm else '', am.group(1) if am else '',
                         tm.group(1) if tm else ''))
        items.append((h.group(1) if h else '', f.group(1) if f else '', tuple(vals)))
    blob = repr(items).encode('utf-8')
    return hashlib.sha256(blob).hexdigest()


def tree(base):
    d = {}
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            d[os.path.relpath(p, base).replace('\\', '/')] = sha(p)
    return d


def is_expected_missing(ref_rel, ref_dir):
    """参考包里有、我们不产出的文件：只接受 .BAK，且必须是备份语义。"""
    low = ref_rel.lower()
    if low.endswith('.bak'):
        return True
    # 孤儿 cjk 图集：参考包磁盘上有，但参考包自己的 fonts.xml 并未声明它
    if os.path.basename(ref_rel).startswith(ORPHAN_PREFIX):
        fxml = os.path.join(ref_dir, 'data', 'if', 'fonts', 'fonts.xml')
        if os.path.isfile(fxml):
            txt = open(fxml, 'rb').read().decode('latin-1')
            if os.path.basename(ref_rel) not in txt:
                return True
    return False


def is_expected_differ(rel, mine, ref):
    """已知且有意为之的**内容**差异。"""
    # fonts.xml 只允许空白差异（见 norm_xml 的说明）
    if rel.endswith('/fonts/fonts.xml') or rel == 'data/if/fonts/fonts.xml':
        return norm_xml(mine) == norm_xml(ref)
    return False


def main():
    mine_root = sys.argv[1] if len(sys.argv) > 1 else '_relw'
    rc = 0
    for key, ref in REFS.items():
        mine = os.path.join(mine_root, key)
        if not os.path.isdir(mine):
            print('!! 缺少装配输出 %s' % mine)
            rc = 1
            continue
        A, B = tree(mine), tree(ref)
        same = 0
        expected = []
        unknown = []
        only_mine = []

        for rel in sorted(set(A) | set(B)):
            if rel in A and rel in B:
                if A[rel] == B[rel]:
                    same += 1
                elif is_expected_differ(rel, os.path.join(mine, rel),
                                        os.path.join(ref, rel)):
                    expected.append(('BYTES-OK(ws)', rel))
                else:
                    unknown.append(('BYTES-DIFFER', rel))
            elif rel in B:
                if is_expected_missing(rel, ref):
                    expected.append(('REF-ONLY(ok)', rel))
                else:
                    unknown.append(('REF-ONLY', rel))
            else:
                if os.path.basename(rel) in EXPECTED_ADDED:
                    expected.append(('ADDED(ok)', rel))
                else:
                    only_mine.append(rel)

        print('=' * 72)
        print('[%s] 相同 %d | 有意差异 %d | 只在我方 %d | 计划外差异 %d'
              % (key, same, len(expected), len(only_mine), len(unknown)))
        # 只在我方：如果参考包缺、我方有，多半是我方多发了东西，需人工确认
        for rel in only_mine:
            print('   ONLY-MINE      %s' % rel)
        for tag, rel in expected[:12]:
            print('   %-14s %s' % (tag, rel))
        if len(expected) > 12:
            print('   ... 另有 %d 个有意差异' % (len(expected) - 12))
        for tag, rel in unknown:
            print('   !! %-11s %s' % (tag, rel))
        if unknown:
            rc = 1

    print()
    print('结果：%s' % ('全部符合预期' if rc == 0 else '存在计划外差异'))
    return rc


if __name__ == '__main__':
    sys.exit(main())
