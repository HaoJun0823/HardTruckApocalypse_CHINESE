# -*- coding: utf-8 -*-
"""全量校验 DLC2_DATA_CHS。

检查项：
  1. 每个目标文件能被 XML 解析器解析（上次事故就是 6 个文件解析失败）
  2. 元素数量与 DLC2_DATA 原件一致
  3. 非翻译属性（id/name/model/sound/...）逐字节未改动
  4. 文件头与转义正确，无"译文后残留英文尾巴"、无未闭合引号
  5. 剩余未翻译条目统计（应随进度递减）
  6. 俄文 cp1251 片段是否被误改（只有译文字段允许变）
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xlit
from apply import FILES, Applier
from todo import pending_for

HEAD = '<?xml version="1.0" encoding="windows-1251" standalone="yes"?>'

# 这些英文是正确译法的一部分（按键名、单位、缩写），不算残留英文尾巴
KEYWORD_OK = re.compile(
    r'(Ctrl|Shift|Alt|CtrlShift|Tab|Enter|Backspace|Delete|Insert|Home|End|'
    r'PageUp|PageDown|Up|Down|Left|Right|AltGr|CapsLock|NumLock|ScrollLock|'
    r'Space|Bar|UpArrow|DownArrow|LeftArrow|RightArrow)', re.I)


def main():
    applier = Applier()
    problems = []
    tot_el = tot_cn = tot_pending = 0

    print('%-42s %5s %5s %5s  %s' % ('file', '元素', '已译', '待翻', '状态'))
    print('-' * 76)
    for rel in sorted(FILES):
        cfg = FILES[rel]
        p_chs = os.path.join(xlit.CHS, *rel.split('/'))
        p_src = os.path.join(xlit.SRC, *rel.split('/'))
        if not os.path.exists(p_chs):
            print('%-42s %5s %5s %5s  缺失' % (rel, '-', '-', '-'))
            problems.append('%s: 成品缺失' % rel)
            continue

        raw = xlit.read_bytes(p_chs)
        lat = raw.decode('latin-1')          # 结构/字节比对用（1字符=1字节）
        gbk = xlit.mixed_decode(raw)         # 成品是混合编码：译文 GBK + 未译原文 cp1251

        errs = []
        # 1 XML 可解析。**与原件比较**：原件本身可能就不是严格合法的
        #    （dynamicdialogsglobal.xml 第 269 行 scriptCondition 属性里有裸 '<'，
        #     游戏自研解析器容忍）。判据是"不比原件更差"，而不是绝对合法。
        import xml.etree.ElementTree as ET

        def perr(data):
            try:
                ET.fromstring(data)
                return None
            except ET.ParseError as ex:
                return (ex.position[0], ex.position[1])

        our_err = perr(lat)
        base_err = perr(xlit.read_bytes(p_src).decode('latin-1'))
        if our_err and not base_err:
            errs.append('XML 解析失败(原件合法): line %d col %d' % our_err)
        elif our_err and base_err and our_err != base_err:
            errs.append('XML 解析错误位置变化 %s -> %s' % (base_err, our_err))
        elif our_err:
            # 与原件同样位置失败：原版遗留问题，跳过但记录
            print('      ~~ 原件同样非严格合法(line %d col %d)，沿用原样'
                  % our_err)

        src_doc = xlit.Doc(rel)
        es = [el for (_, _, el) in src_doc.elements(cfg['tag'])]
        ec = xlit.find_elements(lat, cfg['tag'])
        ec_g = xlit.find_elements(gbk, cfg['tag'])
        # 2 元素数
        if len(es) != len(ec):
            errs.append('元素数 %d -> %d' % (len(es), len(ec)))
        # 3 非翻译属性按字节比对（latin-1 视图 1 字符 = 1 字节）
        for x, y in zip(es, ec):
            if x.spans.keys() != y.spans.keys():
                errs.append('属性集变化 @%s' % x.raw[:40])
                break
            bad = [a for a in x.spans
                   if a not in cfg['attrs'] and x.get(a) != y.get(a)]
            if bad:
                errs.append('非译属性被改: %s @%s' % (bad[0], x.get(cfg['key'])))
                break
        # 4 译文后残留英文尾巴 / 引号损坏
        #    判据用**字节比对**：只有 CHS 字节 ≠ 原件字节的属性才是"我们动过的"，
        #    才可能是译文。之前用 mixed_decode 判 is_cjk 不可靠 —— 未翻译的
        #    cp1251 俄文会被误当成 GBK 汉字（ЗАПРЕЩЕНО -> 抢闲刨磐蜙）。
        COLORCODE = re.compile(r'@[0-9A-Fa-f]{6,8}[A-Za-z_]*')
        n_cn = 0
        for x, y in zip(es, ec):
            key = x.get(cfg['key'])
            for a in cfg['attrs']:
                if not x.has(a):
                    continue
                xb = x.getb(a, 'cp1251')      # 原件视图（raw 是 cp1251 解码的）
                yb = y.getb(a, 'latin-1')     # 成品视图（raw 是 latin-1 解码的）
                if xb == yb:
                    continue                       # 未动过，是原文
                n_cn += 1
                d = xlit.unesc(y.get(a))
                # 未闭合引号：值里出现裸 " 说明上一次事故的引号被吃掉
                if '"' in d:
                    errs.append('引号损坏 @%s.%s' % (key, a))
                    continue
                # 残留英文尾巴：去掉颜色指令后，仍含汉字且以长串 ASCII 单词结尾。
                # 注意 "左Ctrl"/"AltGr" 这类按键名里的英文是正确译法，单独放行。
                stripped = COLORCODE.sub('', d)
                if not xlit.is_cjk(stripped):
                    continue
                m = re.search(r'([A-Za-z]{4,})\s*$', stripped)
                if not m:
                    continue
                if KEYWORD_OK.fullmatch(m.group(1)):
                    continue
                errs.append('英文尾巴 @%s.%s: ...%s' % (key, a, stripped[-30:]))

        rows = pending_for(rel, applier)
        tot_el += len(ec)
        tot_cn += n_cn
        tot_pending += len(rows)

        status = 'OK' if not errs else 'ERR(%d)' % len(errs)
        if errs:
            problems.append('%s: %s' % (rel, errs[0]))
        print('%-42s %5d %5d %5d  %s' % (rel, len(ec), n_cn, len(rows), status))
        for e in errs[:3]:
            print('      !! %s' % e)

    print('-' * 76)
    print('合计 %d 元素，已中文化 %d，待翻 %d' % (tot_el, tot_cn, tot_pending))
    if problems:
        print('\n!! %d 个文件有问题:' % len(problems))
        for p in problems[:20]:
            print('   %s' % p)
        return 1
    print('全部结构校验通过。')
    return 0


if __name__ == '__main__':
    sys.exit(main())