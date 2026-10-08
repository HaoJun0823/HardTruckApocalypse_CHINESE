# -*- coding: utf-8 -*-
r"""
build_release.py —— 把「翻译 XML + 烘好的字库 + 编译出的 DLL + 静态资源」
装配成三个可直接解压到游戏根目录的发布包，并打包为中文名 zip。

═══════════════════════════════════════════════════════════════════════════
三个包对应三个游戏（互不通用，因为锚点地址完全不同）
═══════════════════════════════════════════════════════════════════════════
  base  → HTAA 本体    hta.exe         汉化 hta_chs.asi    + Render9Fix.asi
  dlc1  → 部落崛起     Meridian113.exe 汉化 hta_chs_dlc1.asi + DLC1_MemFix.asi
  dlc2  → 街机版       emarcade.exe    汉化 hta_chs_dlc2.asi + DLC2_MemFix.asi

═══════════════════════════════════════════════════════════════════════════
包内布局（三包同构，目标目录以 base 为例）
═══════════════════════════════════════════════════════════════════════════
  <根>/
    winmm.dll                      Ultimate-ASI-Loader（静态资源，三包相同）
    X86Game4gb.exe                 LAA 补丁器（静态资源；源码在 X86Game4gb/）
    清除俄语输入法布局.exe / .ps1    卸载俄语键盘布局（静态资源）
    <游戏名>_【必装】…bat            一键 LAA（静态资源，按游戏重命名）
    <游戏名>_单核…bat                单核启动（静态资源）
    必读说明.txt                    安装/卸载/已知问题（静态资源）
    data/
      config.cfg                   ★ 中文 profile 名（静态资源，三包不同）
      if/fonts/
        fonts.xml                  ← 烘焙产物（原版 fonts.xml + CJK 图集 Item）
        cjk_<si>_<pi>.dds          ← 烘焙产物
    update/
      hta_chs*.asi                 ← 编译产物（dll 改名）
      <MemFix>.asi / .ini          ← 编译产物 + 仓库内 ini
      hta_chs_cjk*.bin             ← 烘焙产物
      data/…                       ← ★ 译文 XML 原样复制（update\ 是引擎覆盖层）
    ★ DLC2 额外：update/data/if/fonts/ 下要放**原版字体全集**（见 DLC2_NATIVE_FONTS）

═══════════════════════════════════════════════════════════════════════════
为什么译文 XML 放 update\ 而字库放 data\（实测结论，勿改）
═══════════════════════════════════════════════════════════════════════════
引擎对两个目录的读取策略不同，三个包的参考实现都是这么摆的：
  · 译文 XML（dialog/strings/diz/maps）→ update\data\ ：覆盖层，优先级高，
    不必碰原版文件，卸载即恢复。**反向验证过**：这三个文件的 update 版与
    原版**逐字节相同**（说明原作者也是原样搬运，只是走了覆盖层）。
  · 字体 fonts.xml + cjk_*.dds → data\if\fonts\ ：引擎只认这个路径下的字体表
    （update\data\if\fonts\ 在 base/dlc1 里根本不存在）。

用法：
    python tools/release/build_release.py --out release
    python tools/release/build_release.py --out release --skip-zip
"""
import argparse
import datetime
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))       # 仓库根

# ───────────────────────────────────────────────────────────────────────────
# 三个包的规格
# ───────────────────────────────────────────────────────────────────────────
# native_fonts: 需要从原版数据目录**整份复制**进包的原版字体文件（仅 DLC2 需要）
#   原因：DLC2 的参考包在 update\data\if\fonts\ 下同时放了原版那一整套
#   （fonts.xml + sm_*.dds + languagetable.txt），并且**故意用原版 fonts.xml
#   覆盖引擎的字体表**。实测证据：该 fonts.xml 与游戏的 fonts.xml.orig.bak
#   逐字节相同；而烘焙产物（含 CJK Item）放在 data\if\fonts\。
#   emarcade 的字体是运行期由 FontManager 枚举建立的，与 base/dlc1 的
#   「fonts.xml 里多加 Item」机制不同，两者搭配才是实机验证过的组合。
#
# zip 名（ASCII，供 Release 附件用）与内部中文名（玩家看到的）分开：
#   GitHub Release 附件名不支持中文（会被破坏成 _._._），故附件用 ASCII。
PKGS = {
    'base': {
        'title': '燃烧飞车：末日浩劫 简体中文汉化包',
        'zip_ascii': 'HTA-Base-CHS-Patch',
        'zip_cn': '燃烧飞车_末日浩劫_简体中文汉化',
        'exe': 'hta.exe',
        'bat_prefix': 'hta',
        'config': 'base',            # dist/config/base.cfg
        # ★ 三个包统一用 dlc.txt（系列通用版，覆盖三个游戏的差异，见步骤 3 说明）
        'readme': 'dlc',
        'fonts_src': 'Original_DATA/data/if/fonts/fonts.xml',
        'text_dir': 'Original_DATA_CHS',
        'cjk_bin_src': None,         # base 的 bin 由烘焙产出，改名 hta_chs_cjk.bin
        'asi': [
            ('HardTruckApocalypse_CHINESE_DLL', 'hta_chs', 'hta_chs.asi', 'build/Release/hta_chs.dll'),
            ('Render9Fix', 'Render9Fix', 'Render9Fix.asi', 'build/Release/Render9Fix.dll'),
        ],
        'ini': [('Render9Fix/Render9Fix.ini', 'Render9Fix.ini')],
        'native_fonts': None,
    },
    'dlc1': {
        'title': '燃烧飞车：末日浩劫 部落崛起 简体中文汉化包',
        'zip_ascii': 'HTA-DLC1-RiseOfClans-CHS-Patch',
        'zip_cn': '燃烧飞车_末日浩劫_部落崛起_简体中文汉化',
        'exe': 'Meridian113.exe',
        'bat_prefix': 'Meridian113',
        'config': 'dlc1',
        'readme': 'dlc',
        'fonts_src': 'DLC1_DATA/data/if/fonts/fonts.xml',
        'text_dir': 'DLC1_DATA_CHS',
        'cjk_bin_src': None,
        'asi': [
            ('HardTruckApocalypse_CHINESE_DLL_DLC1', 'hta_chs_dlc1',
             'hta_chs_dlc1.asi', 'build/Release/hta_chs_dlc1.dll'),
            ('DLC1_MemFix', 'DLC1_MemFix', 'DLC1_MemFix.asi', 'build/Release/DLC1_MemFix.dll'),
        ],
        'ini': [('DLC1_MemFix/DLC1_MemFix.ini', 'DLC1_MemFix.ini')],
        'native_fonts': None,
        # ★ 中文主菜单图标：参考包里 DLC1 也带它（实测与 BASE 包同一份哈希）。
        'extra': ['Original_DATA_CHS/data/if/ico/mainmenu/mainmenu_down.dds'],
    },
    'dlc2': {
        'title': '燃烧飞车：末日浩劫 街机版 简体中文汉化包',
        'zip_ascii': 'HTA-DLC2-Arcade-CHS-Patch',
        'zip_cn': '燃烧飞车_末日浩劫_街机版_简体中文汉化',
        'exe': 'emarcade.exe',
        'bat_prefix': 'emarcade',
        'config': 'dlc2',
        'readme': 'dlc',
        # ★ DLC2 的烘焙源 fonts.xml 不在 DLC2_DATA/（那是第三方原版数据、不入库），
        #   而正好等于 DLC2_DATA_CHS/data/if/fonts/fonts.xml（实测与
        #   游戏 fonts.xml.orig.bak 逐字节相同）。用后者即可，无需额外源。
        'fonts_src': 'DLC2_DATA_CHS/data/if/fonts/fonts.xml',
        'text_dir': 'DLC2_DATA_CHS',
        'cjk_bin_src': None,
        'asi': [
            ('HardTruckApocalypse_CHINESE_DLL_DLC2', 'hta_chs_dlc2',
             'hta_chs_dlc2.asi', 'build/Release/hta_chs_dlc2.dll'),
            ('DLC2_MemFix', 'DLC2_MemFix', 'DLC2_MemFix.asi', 'build/Release/DLC2_MemFix.dll'),
        ],
        'ini': [('DLC2_MemFix/DLC2_MemFix.ini', 'DLC2_MemFix.ini')],
        # 从 DLC2_DATA_CHS/data/if/fonts 复制的**非 cjk** 原版字体文件
        'native_fonts': 'DLC2_DATA_CHS/data/if/fonts',
        'extra': ['Original_DATA_CHS/data/if/ico/mainmenu/mainmenu_down.dds'],
    },
}

# 烘焙产出的字库包文件名（build_cjk.py 固定产物名）
CJK_BIN_NAME = 'hta_chs_cjk.bin'

# 三个包共用的静态资源（dist/ 下）
STATIC_ROOT_FILES = [
    'winmm.dll',
    'X86Game4gb.exe',
    '清除俄语输入法布局.exe',
    '清除俄语输入法布局.ps1',
]
STATIC_BAT_DIR = 'dist/bat'


def log(msg):
    print(msg, flush=True)


def must(cond, msg):
    if not cond:
        raise SystemExit('!! ' + msg)


def walk_files(base):
    """列出 base 下所有文件的相对路径（'/' 分隔，排序）——保证可复现。"""
    out = []
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            out.append(os.path.relpath(os.path.join(root, f), base).replace('\\', '/'))
    return sorted(out)


def copy_tree(src, dst, only=None):
    """把 src 整棵树复制到 dst。only 给定谓词时只复制其为真的文件。"""
    n = 0
    for rel in walk_files(src):
        if only and not only(rel):
            continue
        s, d = os.path.join(src, rel), os.path.join(dst, rel)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
        n += 1
    return n


def build_one(key, spec, fonts_out_dir, out_root, date, do_zip):
    """装配单个发布包并（可选）打包。返回 (zip 路径 或 None, 统计 dict)。"""
    dst = os.path.join(out_root, key)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)

    stats = {}

    # ── 1) 静态资源：根目录文件 ─────────────────────────────────────────
    for name in STATIC_ROOT_FILES:
        s = os.path.join(ROOT, 'dist', name)
        must(os.path.isfile(s), '缺少静态资源 dist/%s' % name)
        shutil.copy2(s, os.path.join(dst, name))

    # ── 2) 静态资源：6 个 bat，按游戏只取该游戏的两个并去掉前缀 ──────────
    #    参考包的 bat 名形如 `hta_【必装】…bat` / `Meridian113_【从这里进入游戏】…bat`
    #    —— 就是「游戏名 + 功能」，与 dist/bat/ 里的命名一致，直接用。
    pre = spec['bat_prefix'] + '_'
    bats = [f for f in walk_files(os.path.join(ROOT, STATIC_BAT_DIR))
            if os.path.basename(f).startswith(pre)]
    must(len(bats) == 2,
         '%s 期望 2 个 bat（前缀 %r），实际 %d 个: %s'
         % (key, pre, len(bats), bats))
    for f in bats:
        shutil.copy2(os.path.join(ROOT, STATIC_BAT_DIR, f), os.path.join(dst, f))
    stats['bat'] = len(bats)

    # ── 3) 静态资源：必读说明 + config.cfg ──────────────────────────────
    #    ★ 三个包用**同一份**必读说明（`dist/必读说明/dlc.txt`）：
    #      它是"系列通用"版，覆盖三个游戏的差异（街机版/部落崛起各自的
    #      注意事项都写在里面）；本体专用的那份信息量更少且只提 hta.exe，
    #      对两个资料片是错的。故 spec['readme'] 一律指 dlc.txt。
    shutil.copy2(os.path.join(ROOT, 'dist', '必读说明', spec['readme'] + '.txt'),
                 os.path.join(dst, '必读说明.txt'))
    os.makedirs(os.path.join(dst, 'data'), exist_ok=True)
    shutil.copy2(os.path.join(ROOT, 'dist', 'config', spec['config'] + '.cfg'),
                 os.path.join(dst, 'data', 'config.cfg'))

    # ── 3b) License/License.txt ────────────────────────────────────────
    #    把 License/ 下的第三方许可原文按**文件名排序**拼成单个 License.txt
    #    放进包根（与 MajestyIIExtend 的做法一致）。顺序固定 → 产物可复现。
    #    内容为该包真正分发的组件：ASI Loader（winmm.dll）+ 思源黑体（字形
    #    烘进了 cjk 图集）+ texconv（仅构建期用，一并列明以免疑义）。
    lic_dir = os.path.join(ROOT, 'License')
    must(os.path.isdir(lic_dir), '缺少 License/ 目录')
    lic_files = sorted(f for f in os.listdir(lic_dir) if f.lower().endswith('.txt'))
    must(lic_files, 'License/ 目录里没有任何 .txt')
    parts = []
    for i, f in enumerate(lic_files):
        # 用 UTF-8 读、CRLF 写（Windows 记事本对 LF 兼容不佳）
        txt = open(os.path.join(lic_dir, f), encoding='utf-8').read()
        if i:
            parts.append('\n\n' + '=' * 78 + '\n\n')
        parts.append(txt)
    lic_out = os.path.join(dst, 'License.txt')
    with open(lic_out, 'wb') as fh:
        fh.write(''.join(parts).replace('\r\n', '\n').replace('\n', '\r\n')
                 .encode('utf-8'))
    stats['license_files'] = lic_files
    log('  License.txt: 合并 %d 份（%s）' % (len(lic_files), ', '.join(lic_files)))

    # ── 4) 字库（烘焙产物）→ data/if/fonts ─────────────────────────────
    #   烘焙输出目录布局：<fonts_out>/<key>/{data/if/fonts/*, hta_chs_cjk.bin}
    baked = os.path.join(fonts_out_dir, key)
    must(os.path.isdir(baked),
         '%s 缺少烘焙产物目录 %s（先跑 fontgen/build_cjk.py）' % (key, baked))
    baked_fonts = os.path.join(baked, 'data', 'if', 'fonts')
    must(os.path.isfile(os.path.join(baked_fonts, 'fonts.xml')),
         '%s 烘焙产物缺 fonts.xml' % key)

    dst_fonts = os.path.join(dst, 'data', 'if', 'fonts')
    os.makedirs(dst_fonts, exist_ok=True)
    n = 0
    for f in walk_files(baked_fonts):
        shutil.copy2(os.path.join(baked_fonts, f), os.path.join(dst_fonts, f))
        n += 1
    stats['cjk_pages'] = n - 1        # 去掉 fonts.xml
    log('  字库: fonts.xml + %d 张 cjk 图集' % stats['cjk_pages'])

    # ── 5) DLC2 额外的原版字体全集 → update/data/if/fonts ──────────────
    if spec['native_fonts']:
        nf_src = os.path.join(ROOT, spec['native_fonts'])
        must(os.path.isdir(nf_src), '缺少 %s' % spec['native_fonts'])
        nf_dst = os.path.join(dst, 'update', 'data', 'if', 'fonts')
        os.makedirs(nf_dst, exist_ok=True)
        n = copy_tree(nf_src, nf_dst,
                      only=lambda rel: not os.path.basename(rel).startswith('cjk_'))
        stats['native_fonts'] = n
        log('  原版字体全集: %d 个（update/data/if/fonts）' % n)

    # ── 6) 译文资源 → update/data（原样复制）────────────────────────────
    #    ★ 不只收 .xml：译文目录里还带图形资源（如中文主菜单图标
    #      data/if/ico/mainmenu/mainmenu_down.dds），参考包里在 update\data\
    #      下原样分发。唯一要排除的是 .BAK（历史备份，参考包不分发）。
    txt_src = os.path.join(ROOT, spec['text_dir'], 'data')
    must(os.path.isdir(txt_src), '缺少译文目录 %s' % spec['text_dir'])
    txt_dst = os.path.join(dst, 'update', 'data')
    #    DLC2 的译文目录里同时含原版字体（fonts.xml + sm_*.dds），那部分由
    #    上面的 native_fonts 专门负责，这里跳过以免重复处理。
    nf_rel = None
    if spec['native_fonts']:
        nf_rel = os.path.relpath(os.path.join(ROOT, spec['native_fonts']),
                                 txt_src).replace('\\', '/') + '/'

    def keep_text(rel):
        if os.path.splitext(rel)[1].upper() == '.BAK':
            return False
        if nf_rel and rel.startswith(nf_rel):
            return False
        return True

    n = copy_tree(txt_src, txt_dst, only=keep_text)
    stats['text_files'] = n
    log('  译文资源: %d 个' % n)

    # ── 6b) 跨游戏共享的补充资源 ───────────────────────────────────────
    #    中文主菜单图标只有本体那份（Original_DATA_CHS），但资料片/街机版
    #    的参考包里同样分发了它 —— 属于「同一个中文 UI 资源三包共用」。
    for rel in spec.get('extra', []):
        s = os.path.join(ROOT, rel)
        must(os.path.isfile(s), '%s 缺少补充资源 %s' % (key, rel))
        # 落到包内的位置：去掉来源目录的 `<XXX>_CHS/data/` 前缀
        m = rel.split('/data/', 1)
        must(len(m) == 2, '补充资源路径须含 /data/：%s' % rel)
        d = os.path.join(dst, 'update', 'data', m[1])
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
        stats.setdefault('extra', []).append(m[1])
        log('  补充资源: %s' % m[1])

    # ── 7) 编译产物 asi + ini → update\ ────────────────────────────────
    upd = os.path.join(dst, 'update')
    os.makedirs(upd, exist_ok=True)
    sizes = {}
    for proj, target, asi_name, rel in spec['asi']:
        src = os.path.join(ROOT, proj, rel)
        must(os.path.isfile(src), '%s 缺少编译产物 %s' % (key, src))
        shutil.copy2(src, os.path.join(upd, asi_name))
        sizes[asi_name] = os.path.getsize(src)
    for rel, ini_name in spec['ini']:
        src = os.path.join(ROOT, rel)
        must(os.path.isfile(src), '%s 缺少 %s' % (key, rel))
        shutil.copy2(src, os.path.join(upd, ini_name))
    stats['asi_sizes'] = sizes
    for k, v in sizes.items():
        log('  %-22s %8d B' % (k, v))

    # ── 8) 字库包 → update\ ────────────────────────────────────────────
    bin_src = os.path.join(baked, CJK_BIN_NAME)
    must(os.path.isfile(bin_src), '%s 烘破产物缺 %s' % (key, CJK_BIN_NAME))
    #    ★ bin 在包内的名字按游戏区分（与 asi 里写死的文件名一致）：
    #        base → hta_chs_cjk.bin
    #        dlc1 → hta_chs_cjk_dlc1.bin
    #        dlc2 → hta_chs_cjk_dlc2.bin
    bin_dst_name = 'hta_chs_cjk.bin' if key == 'base' else 'hta_chs_cjk_%s.bin' % key
    shutil.copy2(bin_src, os.path.join(upd, bin_dst_name))
    log('  %-22s %8d B' % (bin_dst_name, os.path.getsize(bin_src)))

    # ── 9) 打包 ────────────────────────────────────────────────────────
    zip_path = None
    if do_zip:
        zip_path = os.path.join(out_root, '%s_%s.zip' % (spec['zip_cn'], date))
        if os.path.isfile(zip_path):
            os.remove(zip_path)
        #    ★ Python zipfile：非 ASCII 条目名会自动置 **bit 11（UTF-8 标志）**，
        #      Windows 自带解压器才能正确显示中文；7-Zip 需额外参数，
        #      Compress-Archive 根本不设该位（会乱码）。与本仓库其他项目一致。
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED,
                             compresslevel=9) as z:
            for rel in walk_files(dst):
                z.write(os.path.join(dst, rel), rel.replace('\\', '/'))
        stats['zip'] = zip_path
        stats['zip_size'] = os.path.getsize(zip_path)
        log('  → %s  (%.1f MB)' % (os.path.basename(zip_path),
                                   stats['zip_size'] / 1048576.0))
    return zip_path, stats


def main():
    ap = argparse.ArgumentParser(description='装配三个游戏的汉化发布包')
    ap.add_argument('--out', default=os.path.join(ROOT, 'release'),
                    help='输出根目录（默认 <repo>/release）')
    ap.add_argument('--fonts', default=None,
                    help='烘焙产物根目录（默认 <out>/../baked_fonts）')
    ap.add_argument('--date', default=None, help='日期戳 YYYYMMDD（默认 UTC+8 今天）')
    ap.add_argument('--targets', default=','.join(PKGS), help='只做指定包（逗号分隔）')
    ap.add_argument('--skip-zip', action='store_true', help='只装配目录，不打 zip')
    args = ap.parse_args()

    out_root = os.path.abspath(args.out)
    os.makedirs(out_root, exist_ok=True)
    fonts_out = os.path.abspath(args.fonts) if args.fonts \
        else os.path.join(os.path.dirname(out_root), 'baked_fonts')

    date = args.date or (datetime.datetime.now(datetime.timezone.utc)
                         + datetime.timedelta(hours=8)).strftime('%Y%m%d')

    targets = [t.strip() for t in args.targets.split(',') if t.strip()]
    unknown = [t for t in targets if t not in PKGS]
    must(not unknown, '未知目标: %s' % unknown)

    # ★ 先做**全量存在性检查**，再动手：宁可一个都不产出，也不要产出半套。
    for key in targets:
        spec = PKGS[key]
        for f in STATIC_ROOT_FILES:
            must(os.path.isfile(os.path.join(ROOT, 'dist', f)), '缺少 dist/%s' % f)
        must(os.path.isfile(os.path.join(ROOT, 'dist', 'config', spec['config'] + '.cfg')),
             '缺少 dist/config/%s.cfg' % spec['config'])
        must(os.path.isfile(os.path.join(ROOT, 'dist', '必读说明', spec['readme'] + '.txt')),
             '缺少 dist/必读说明/%s.txt' % spec['readme'])
        must(os.path.isdir(os.path.join(ROOT, spec['text_dir'])),
             '缺少译文目录 %s' % spec['text_dir'])
        for proj, _t, _a, rel in spec['asi']:
            must(os.path.isfile(os.path.join(ROOT, proj, rel)),
                 '缺少编译产物 %s/%s（先编译 DLL）' % (proj, rel))

    log('装配 %d 个包  date=%s  out=%s  fonts=%s'
        % (len(targets), date, out_root, fonts_out))

    results = {}
    for key in targets:
        log('')
        log('=' * 66)
        log('[%s] %s' % (key, PKGS[key]['title']))
        log('=' * 66)
        zip_path, stats = build_one(key, PKGS[key], fonts_out, out_root,
                                    date, not args.skip_zip)
        results[key] = (zip_path, stats)

    log('')
    log('=' * 66)
    log('汇总')
    log('=' * 66)
    for key in targets:
        z, st = results[key]
        if z:
            log('  %-6s %-52s %.1f MB' % (key, os.path.basename(z),
                                          st['zip_size'] / 1048576.0))
        else:
            log('  %-6s (仅装配目录 %s)' % (key, key))
    return 0


if __name__ == '__main__':
    sys.exit(main())
