# -*- coding: utf-8 -*-
r"""
verify_release.py —— 发布包结构与指纹校验（独立于装配脚本，故意重复断言）

为什么要在 build_release.py 之外再核一遍：
  装配脚本自己检查自己等于没检查。这里从**产物侧**重新打开每个文件，独立
  断言「该有的在不在、体积指纹对不对、fonts.xml 声明与磁盘是否一致、
  zip 里的中文条目有没有置 UTF-8 标志位」。任一不符即以非 0 退出。

用法：
    python tools/release/verify_release.py release
"""
import glob
import os
import re
import sys
import zipfile

# ── 期望的 DLL 体积指纹 ──────────────────────────────────────────────────
# ★ 与 tools/release/compile_dlls.sh 里的闸门同源。这里再断言一次是刻意的：
#   体积是「工具集 + SDK + flags」的确定指纹，工具链回退必然改变体积。
#   本机用真 v141(14.16.27023) + SDK 26100 复现并逐字节核对过参考包。
WANT_ASI = {
    'hta_chs.asi':     178176,
    'hta_chs_dlc1.asi': 178176,
    'hta_chs_dlc2.asi': 195584,
    'Render9Fix.asi':  144384,
    'DLC1_MemFix.asi': 140800,
    'DLC2_MemFix.asi': 140288,
}

# ── 每个包的结构规格 ─────────────────────────────────────────────────────
#   pages: 图集页数（由译文汉字数与各字号格数决定，是稳定值；
#          改译文/改字号会改变它 —— 那种情况需要同步更新这里）
PKG_SPEC = {
    'base': dict(
        exe='hta.exe', bat_prefix='hta',
        asi=['hta_chs.asi', 'Render9Fix.asi'],
        ini=['Render9Fix.ini'],
        bin='hta_chs_cjk.bin', pages=85, text_min=130),
    'dlc1': dict(
        exe='Meridian113.exe', bat_prefix='Meridian113',
        asi=['hta_chs_dlc1.asi', 'DLC1_MemFix.asi'],
        ini=['DLC1_MemFix.ini'],
        bin='hta_chs_cjk_dlc1.bin', pages=81, text_min=128),
    'dlc2': dict(
        exe='emarcade.exe', bat_prefix='emarcade',
        asi=['hta_chs_dlc2.asi', 'DLC2_MemFix.asi'],
        ini=['DLC2_MemFix.ini'],
        bin='hta_chs_cjk_dlc2.bin', pages=88, text_min=64),
}

# 每个包都必须有的根目录静态资源
#   License.txt 是 License/ 下第三方许可原文合并而成（见 build_release.py 3b）
ROOT_REQ = ['winmm.dll', 'X86Game4gb.exe', '必读说明.txt', 'License.txt',
            '清除俄语输入法布局.exe', '清除俄语输入法布局.ps1']

# 预期之外的扩展名（打包事故的典型信号）
BAD_EXT = ('.bak', '.pdb', '.obj', '.ilk', '.user', '.suo')


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'release'
    ok = True

    for key, sp in PKG_SPEC.items():
        d = os.path.join(root, key)
        if not os.path.isdir(d):
            print('!! 缺少装配目录 %s' % d)
            ok = False
            continue

        # 1) 根目录静态资源
        for f in ROOT_REQ:
            if not os.path.isfile(os.path.join(d, f)):
                print('!! [%s] 缺根文件 %s' % (key, f))
                ok = False

        # 2) bat：恰好 2 个，且前缀正确
        bats = sorted(f for f in os.listdir(d) if f.lower().endswith('.bat'))
        if len(bats) != 2:
            print('!! [%s] bat 有 %d 个（应 2 个）: %s' % (key, len(bats), bats))
            ok = False
        for b in bats:
            if not b.startswith(sp['bat_prefix'] + '_'):
                print('!! [%s] bat 前缀不对：%s' % (key, b))
                ok = False

        # 3) config.cfg
        if not os.path.isfile(os.path.join(d, 'data', 'config.cfg')):
            print('!! [%s] 缺 data/config.cfg' % key)
            ok = False

        # 3b) License.txt：必须是多份许可合并（含 ASI Loader 与思源黑体）
        lic = os.path.join(d, 'License.txt')
        if not os.path.isfile(lic):
            print('!! [%s] 缺 License.txt' % key)
            ok = False
        else:
            ltxt = open(lic, encoding='utf-8', errors='replace').read()
            for must_have in ('Ultimate ASI Loader', 'ThirteenAG',
                              'SIL OPEN FONT LICENSE', 'MIT License'):
                if must_have not in ltxt:
                    print('!! [%s] License.txt 缺少「%s」' % (key, must_have))
                    ok = False
            if ltxt.count('==========') < 2:
                print('!! [%s] License.txt 疑似只有 1 份许可（缺分隔）' % key)
                ok = False

        # 3c) 必读说明：三个包统一用系列通用版，应覆盖 DLC 相关说明
        rd = os.path.join(d, '必读说明.txt')
        if not os.path.isfile(rd):
            print('!! [%s] 缺 必读说明.txt' % key)
            ok = False
        else:
            rtxt = open(rd, encoding='utf-8', errors='replace').read()
            #    ★ 底包/资料片统一使用系列通用版，该版含 DLC 段落；
            #      若误用本体专用版，这两条会缺失。
            for must_have in ('DLC1', '系列'):
                if must_have not in rtxt:
                    print('!! [%s] 必读说明.txt 缺少「%s」（像是用错了版本）'
                          % (key, must_have))
                    ok = False

        # 4) 字库：fonts.xml + 图集页，且声明与磁盘一致（防孤儿 / 防漏发）
        fdir = os.path.join(d, 'data', 'if', 'fonts')
        fxml = os.path.join(fdir, 'fonts.xml')
        if not os.path.isfile(fxml):
            print('!! [%s] 缺 data/if/fonts/fonts.xml' % key)
            ok = False
            pages = 0
        else:
            ondisk = set(os.path.basename(p)
                         for p in glob.glob(os.path.join(fdir, 'cjk_*.dds')))
            pages = len(ondisk)
            decl = set(re.findall(
                r'cjk_\d+_\d+\.dds', open(fxml, 'rb').read().decode('latin-1')))
            if decl != ondisk:
                print('!! [%s] fonts.xml 声明与磁盘不一致：磁盘多 %s / 声明多 %s'
                      % (key, sorted(ondisk - decl), sorted(decl - ondisk)))
                ok = False
            if pages != sp['pages']:
                print('!! [%s] 图集 %d 张（期望 %d）—— 译文或字号变了？'
                      % (key, pages, sp['pages']))
                ok = False

        # 5) asi / ini 与体积指纹
        for a in sp['asi']:
            p = os.path.join(d, 'update', a)
            if not os.path.isfile(p):
                print('!! [%s] 缺 update/%s' % (key, a))
                ok = False
                continue
            sz = os.path.getsize(p)
            if sz != WANT_ASI[a]:
                print('!! [%s] %s = %d B（期望 %d B）—— 工具链或 flags 回退？'
                      % (key, a, sz, WANT_ASI[a]))
                ok = False
        for a in sp['ini']:
            if not os.path.isfile(os.path.join(d, 'update', a)):
                print('!! [%s] 缺 update/%s' % (key, a))
                ok = False

        # 6) 字库包
        if not os.path.isfile(os.path.join(d, 'update', sp['bin'])):
            print('!! [%s] 缺 update/%s' % (key, sp['bin']))
            ok = False

        # 7) 译文
        ntxt = len(glob.glob(os.path.join(d, 'update', 'data', '**', '*.xml'),
                             recursive=True))
        if ntxt < sp['text_min']:
            print('!! [%s] update/data 只有 %d 个 xml（期望 >= %d）'
                  % (key, ntxt, sp['text_min']))
            ok = False

        # 8) DLC2 的 update/data/if/fonts 必须**不存在**：
        #    该路径会覆盖游戏 data 的原版字体，实测是错误做法（见
        #    build_release.py 的 keep_text）。字库只走 data/if/fonts。
        if key == 'dlc2':
            bogus = os.path.join(d, 'update', 'data', 'if', 'fonts')
            if os.path.exists(bogus):
                print('!! [%s] 不该出现 update/data/if/fonts（会覆盖游戏 data）'
                      % key)
                ok = False

        # 9) 不该出现的构建产物 / 备份
        for p in glob.glob(os.path.join(d, '**', '*'), recursive=True):
            if os.path.isfile(p) and p.lower().endswith(BAD_EXT):
                print('!! [%s] 混入了不该发布的文件：%s'
                      % (key, os.path.relpath(p, d)))
                ok = False

        print('  OK %-5s 图集 %-3d 张, 译文 xml %-4d 个, bat %d 个'
              % (key, pages, ntxt, len(bats)))

    # ── zip 层：数量、中文条目名的 UTF-8 标志位 ──────────────────────────
    zips = sorted(glob.glob(os.path.join(root, '*.zip')))
    if len(zips) != len(PKG_SPEC):
        print('!! 期望 %d 个 zip，实际 %d 个：%s'
              % (len(PKG_SPEC), len(zips), [os.path.basename(z) for z in zips]))
        ok = False
    for z in zips:
        with zipfile.ZipFile(z) as zf:
            infos = zf.infolist()
            names = [i.filename for i in infos]
            cn = [i for i in infos
                  if any('\u4e00' <= c <= '\u9fff' for c in i.filename)]
            bad = [i.filename for i in cn if not (i.flag_bits & 0x800)]
            if bad:
                # ★ Python zipfile 对非 ASCII 名会自动置 bit 11；不置的话
                #   Windows 自带解压器会按 GBK 猜测，跨区域设置就乱码。
                print('!! %s 的中文条目未置 UTF-8 标志位：%s'
                      % (os.path.basename(z), bad[:5]))
                ok = False
            # 反斜杠路径分隔符会让部分解压器出错
            if any('\\' in n for n in names):
                print('!! %s 内存在反斜杠路径' % os.path.basename(z))
                ok = False
            # 排除混入的构建产物
            if any(n.lower().endswith(BAD_EXT) for n in names):
                print('!! %s 内含构建产物/备份' % os.path.basename(z))
                ok = False
            dupe = len(names) != len(set(names))
            if dupe:
                print('!! %s 内含重复条目名' % os.path.basename(z))
                ok = False
            print('  OK %-52s %5d 条目, 中文名 %d 个, %5.1f MB'
                  % (os.path.basename(z), len(infos), len(cn),
                     os.path.getsize(z) / 1048576.0))

    print()
    print('校验结果：%s' % ('通过' if ok else '未通过'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
