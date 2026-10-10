# 燃烧飞车：末日浩劫 系列 · 简体中文汉化

《Hard Truck Apocalypse / Ex Machina》系列三个游戏的简体中文汉化 + 老引擎兼容修复。

作者：[HAOJUN0823](https://www.haojun0823.xyz/)　·　主页：<https://github.com/HaoJun0823/HardTruckApocalypse_CHINESE>

---

## 三个游戏、三套包（不能混用）

| 游戏 | 主程序 | 汉化插件 | 崩溃修复插件 |
|---|---|---|---|
| 本体 | `hta.exe` | `hta_chs.asi` | `Render9Fix.asi` |
| 部落崛起（资料片 113） | `Meridian113.exe` | `hta_chs_dlc1.asi` | `DLC1_MemFix.asi` |
| 街机版 | `emarcade.exe` | `hta_chs_dlc2.asi` | `DLC2_MemFix.asi` |

> ⚠️ **三套包不能互相覆盖**。三个 exe 的字体/文本代码虽然同源，但**地址、
> 寄存器分配、结构体偏移三者都不同**，补丁点必须逐点重新对齐。装错版本会启动即崩。
> 锚点差异对照见 [docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md](docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md)。

---

## 发布流水线（CI）

`.github/workflows/build-release.yml` 在 **ubuntu-latest + msvc-wine** 上完成全部构建，
产出三个中文名 zip 并自动发 Release。

```
①检出仓库 + msvc-wine        ②装 MSVC v141(14.16) + Win10 SDK 26100（带缓存）
③编译 6 个插件 DLL           ④为 wine 配 texconv 的 4 个 VC141 运行库
⑤烘三套字库                  ⑥装配三个发布包
⑦校验结构与体积指纹          ⑧上传 artifact → 同 run 内发 Release
```

| 步骤 | 脚本 |
|---|---|
| 编译 DLL（含体积闸门） | [tools/release/compile_dlls.sh](tools/release/compile_dlls.sh) |
| 烘字库 | [fontgen/build_cjk.py](fontgen/build_cjk.py) |
| 装配发布包 | [tools/release/build_release.py](tools/release/build_release.py) |
| 校验产物 | [tools/release/verify_release.py](tools/release/verify_release.py) |

### 为什么要 ubuntu + msvc-wine，而不是 windows runner

windows-2022/2025 镜像**不带 v141 (MSVC 14.16)**，构建会静默回退到新版工具集，
产物体积随之改变。msvc-wine 从微软官方 manifest 精确拉 VS2017 15.9 的
`VC.Tools.14.16`，用 wine 跑真 `cl.exe`/`link.exe`。

本仓库 6 个 DLL 都有历史确认的**体积指纹**，`compile_dlls.sh` 末尾逐一断言：

| 产物 | 体积 |
|---|---|
| `hta_chs.dll` | 178176 B |
| `hta_chs_dlc1.dll` | 178176 B |
| `hta_chs_dlc2.dll` | 195584 B |
| `Render9Fix.dll` | 144384 B |
| `DLC1_MemFix.dll` | 140800 B |
| `DLC2_MemFix.dll` | 140288 B |

体积是「工具集 + SDK + flags 组合」的确定指纹：任一项回退都会改变体积 → 构建当场失败，
绝不把错 DLL 发出去。

### 字库烘焙为什么必须在 CI 里做

汉字图集（`cjk_<字号>_<页>.dds`）、`fonts.xml` 与 `hta_chs_cjk*.bin` 都是**由译文 XML
动态生成**的：改一句译文就可能引入新汉字，页数随之变化。所以不预先提交成品，
而是每次构建按当前译文重新烘（`build_cjk.py` 会扫描译文目录收集汉字集）。

---

## 本地构建

```bash
# 1) 编译 DLL（需 MSVC v141 = 14.16；Windows 本机或 msvc-wine）
sh tools/release/compile_dlls.sh

# 2) 烘字库（需 Pillow；texconv 由脚本自动调用）
#    Windows 本机直接跑；Linux 上通过 HTA_TEXCONV 环境变量用 wine
python fontgen/build_cjk.py \
  --src-fonts Original_DATA/data/if/fonts/fonts.xml \
  --text-dir Original_DATA_CHS --out baked_fonts/base
python fontgen/build_cjk.py \
  --src-fonts DLC1_DATA/data/if/fonts/fonts.xml \
  --text-dir DLC1_DATA_CHS --out baked_fonts/dlc1
python fontgen/build_cjk.py \
  --src-fonts DLC2_DATA/data/if/fonts/fonts.xml \
  --text-dir DLC2_DATA_CHS --out baked_fonts/dlc2

# 3) 装配 + 校验
python tools/release/build_release.py --out release --fonts baked_fonts
python tools/release/verify_release.py release
```

---

## 目录结构

| 目录 | 内容 |
|---|---|
| `HardTruckApocalypse_CHINESE_DLL/` | 本体汉化插件（`hta_chs.asi`）源码 |
| `HardTruckApocalypse_CHINESE_DLL_DLC1/` | 部落崛起汉化插件源码 |
| `HardTruckApocalypse_CHINESE_DLL_DLC2/` | 街机版汉化插件源码 |
| `Render9Fix/` | 本体崩溃修复插件（`dxrender9` 分配导向低地址） |
| `DLC1_MemFix/` / `DLC2_MemFix/` | 两个资料片的等价修复插件 |
| `fontgen/` | 字库烘焙（`build_cjk.py`、`texconv.exe`、思源黑体） |
| `Original_DATA_CHS/` `DLC1_DATA_CHS/` `DLC2_DATA_CHS/` | **译文 XML**（update 覆盖层的内容） |
| `Original_DATA/` `DLC1_DATA/` | 原版俄文资料（烘焙源的 `fonts.xml` 在这里） |
| `dist/` | 静态原料库（winmm、工具 exe、bat、config、必读说明） |
| `License/` | 第三方组件许可原文（装配时合并为包内 `License.txt`） |
| `tools/release/` | 发布流水线脚本 |
| `docs/` | 根因分析、移植记录、[路径 D 总设计](docs/PATH_D_DESIGN.md) |
| `archives/` | 历史快照与一次性脚手架（**不入库**，见 `.gitignore`） |

---

## 发布包内布局（三个包同构）

```
<包根>/
  winmm.dll            Ultimate ASI Loader（MIT）
  X86Game4gb.exe       LAA 补丁器
  清除俄语输入法布局.exe / .ps1
  <游戏名>_【必装】…bat  一键打 LAA 补丁
  <游戏名>_单核…bat      单核启动
  必读说明.txt          安装/卸载/已知问题（三包同一份，系列通用版）
  License.txt          第三方许可合并（ASI Loader + 思源黑体 + texconv）
  data/
    config.cfg         中文 profile 名
    if/fonts/          fonts.xml + cjk_*.dds（烘焙产物）
  update/
    hta_chs*.asi       汉化插件
    <MemFix>.asi/.ini  崩溃修复插件
    hta_chs_cjk*.bin   字形包
    data/…             译文 XML（引擎的覆盖层）
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/PATH_D_DESIGN.md](docs/PATH_D_DESIGN.md) | 路径 D 总设计：让单字节渲染引擎显示 GBK 双字节汉字 |
| [docs/2026-10-08-…LAA崩溃修复.md](docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md) | 113 移植的锚点差异 + LAA 崩溃取证 + MemFix 实现陷阱 |
| [docs/2026-10-07-LAA内存扩展崩溃-根因与Render9Fix.md](docs/2026-10-07-LAA内存扩展崩溃-根因与Render9Fix.md) | 本体 LAA 崩溃的完整取证链 |
| [docs/2026-10-07-汉化冻结门时序失败-根因与修复.md](docs/2026-10-07-汉化冻结门时序失败-根因与修复.md) | 冷启动/热重启两个时序竞态 |
| [docs/README.md](docs/README.md) | 文档索引 |

---

## 第三方组件与许可

| 组件 | 版权 | 许可 | 是否随包分发 |
|---|---|---|---|
| `winmm.dll`（Ultimate ASI Loader 9.7.4） | ThirteenAG | MIT | ✅ 是 |
| `fontgen/SourceHanSansHWSC-VF.ttf`（思源黑体） | Adobe | SIL OFL 1.1 | ✅ 字形烘进 `cjk_*.dds` |
| `fontgen/texconv.exe`（DirectXTex） | Microsoft | MIT | ❌ 仅构建期用 |

许可原文见 [License/](License/)。发布包内合并为单个 `License.txt`。

---

## 免责声明

本汉化包仅包含**文本翻译与兼容性补丁**，不含任何游戏本体文件、不修改游戏主程序
（全部补丁在运行时施加）。请自行购买并合法持有游戏。翻译文本版权归作者所有，
转载请注明出处。
