# -*- coding: utf-8 -*-
"""静态检查所有 .fx：找出"唯一 technique 且无 fallback"的高危文件。

判定依据（来自 dxrender9 的 EffectImpl::ValidateEffect 反汇编 0x10041140）：
  循环收集 technique 时，任何一个未通过 ValidateTechnique 的会被跳过；
  若**全部**被跳过，m_techDescs 为空 → 后续 m_techDescs[0] 空指针崩溃。
  所以危险条件 = technique 总数 == 1。
  （总数 >1 时即使个别被跳过，至少还有 1 个可用 technique，不崩。）

同时统计：
  - IsPs20 标记（源码 L458 分支，现代驱动上 PS_2_0 常被拒绝）
  - 用到的 shader model 等级（vs_1_1/ps_1_1 等最老的在现代硬件上易失败）
"""
import os, re, sys, glob
sys.stdout.reconfigure(encoding='utf-8')

GAMES = {
    'STEAM (hta.exe v1.03)': r'I:\LocalGames\Hard Truck Apocalypse STEAM\data\shaders',
    'GOG (Rise of Clans)':   r'I:\LocalGames\HARD.TRUCK.APOCALYPSE.RISE.OF.CLANS GOG\HARD TRUCK APOCALYPSE RISE OF CLANS\data\shaders',
}

# technique <Name> ... { ... }   —— 数出 technique 块
RE_TECH = re.compile(r'\btechnique\s+([A-Za-z_]\w*)', re.I)
RE_PASS = re.compile(r'\bpass\b', re.I)
RE_PS20 = re.compile(r'IsPs20\s*=\s*true', re.I)
RE_DEFAULT = re.compile(r'\bDefault\s*=\s*true', re.I)
# shader 编译等级
RE_SM = re.compile(r'\b([vp]s[_\s]?)(\d)[_ ](\d)\b', re.I)
RE_INCLUDE = re.compile(r'#include\s+"([^"]+)"', re.I)


def scan(d, label):
    files = sorted(glob.glob(os.path.join(d, '*.fx')))
    print('=' * 100)
    print('%s   共 %d 个 .fx' % (label, len(files)))
    print('=' * 100)
    print('%-34s %5s %5s %6s %6s  %-22s %s'
          % ('file', 'tech', 'pass', 'ps2.0', 'deflt', 'shader最低等级', 'include'))
    print('-' * 118)

    risky, risky_multi, fine = [], [], []
    for p in files:
        raw = open(p, 'rb').read()
        txt = raw.decode('latin-1', 'ignore')
        # 去掉注释，避免注释里的 technique 关键字误计
        txt = re.sub(r'//[^\n]*', '', txt)
        txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)

        techs = RE_TECH.findall(txt)
        ntech = len(techs)
        npass = len(RE_PASS.findall(txt))
        ps20  = bool(RE_PS20.search(txt))
        dflt  = bool(RE_DEFAULT.search(txt))
        sms   = RE_SM.findall(txt)
        mins  = sorted({'%s%s_%s' % (k[0][0], k[1], k[2]) for k in sms}) if sms else []
        mins_s = ','.join(mins[:4]) if mins else '-'
        incs   = ','.join(RE_INCLUDE.findall(txt)) or '-'

        name = os.path.basename(p)
        flag = ''
        if ntech == 0:
            flag = '★无 technique'
        elif ntech == 1:
            flag = '★★仅 1 个 technique'
        row = (name, ntech, npass, 'Y' if ps20 else '-', 'Y' if dflt else '-', mins_s, incs)
        print('%-34s %5d %5d %6s %6s  %-22s %s %s'
              % (row[0], row[1], row[2], row[3], row[4], row[5], row[6], flag))

        if ntech == 1:
            (risky if not dflt else risky_multi).append((name, mins_s, ps20, dflt))
        elif ntech == 0:
            risky.append((name, mins_s, ps20, dflt))

    print()
    print('─' * 118)
    print('★★ 高危：technique 总数 == 1（一旦它验证失败，m_techDescs 为空 → 空指针崩溃）')
    for n, m, p20, d in risky:
        print('   %-34s  最低shader=%-16s ps2.0=%s' % (n, m, 'Y' if p20 else '-'))
    if not risky:
        print('   （无）')
    print()
    print('─' * 118)
    print('总结: 高危 %d 个 / 合计 %d 个' % (len(risky), len(files)))
    print()
    return risky, files


allrisky = {}
for label, d in GAMES.items():
    if not os.path.isdir(d):
        print('【跳过】目录不存在: %s' % d)
        continue
    r, f = scan(d, label)
    allrisky[label] = (r, f)

print('=' * 100)
print('两游戏交叉对比：road.fx 等同名文件是否一致')
print('=' * 100)
gs = list(allrisky)
if len(gs) == 2:
    a = {os.path.basename(p): open(p,'rb').read() for p in allrisky[gs[0]][1]}
    b = {os.path.basename(p): open(p,'rb').read() for p in allrisky[gs[1]][1]}
    same, diff, onlya, onlyb = [], [], [], []
    for k in sorted(set(a) | set(b)):
        if k not in a: onlyb.append(k)
        elif k not in b: onlya.append(k)
        elif a[k] == b[k]: same.append(k)
        else: diff.append(k)
    print('完全相同 %d 个；内容不同 %d 个；仅 %s 有 %d 个；仅 %s 有 %d 个'
          % (len(same), len(diff), gs[0][:5], len(onlya), gs[1][:5], len(onlyb)))
    print()
    print('内容不同的文件:')
    for k in diff:
        ra = a[k].decode('latin-1','ignore')
        rb = b[k].decode('latin-1','ignore')
        ta = re.findall(r'\btechnique\s+(\w+)', re.sub(r'//[^\n]*','',ra), re.I)
        tb = re.findall(r'\btechnique\s+(\w+)', re.sub(r'//[^\n]*','',rb), re.I)
        print('   %-34s %s:%d个  %s:%d个  %s'
              % (k, gs[0][:5], len(ta), gs[1][:5], len(tb),
                 '★两边都仅1个' if len(ta) == 1 and len(tb) == 1 else ''))
    if onlya: print('仅 %s 独有: %s' % (gs[0][:5], ', '.join(onlya)))
    if onlyb: print('仅 %s 独有: %s' % (gs[1][:5], ', '.join(onlyb)))