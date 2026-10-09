# dist —— 发布流水线的**静态原料库**

这里放的是**不参与构建、直接随包分发**的静态资源。发布流水线
（`.github/workflows/build-release.yml` → `tools/release/build_release.py`）
把本目录的资源 + 编译出的 DLL + 烘好的字库 + 译文 XML 装配成三个发布包。

> 使用方式（玩家侧）：解压发布包到游戏根目录，双击对应的 `.bat`。
> `X86Game4gb.exe` 与 `清除俄语输入法布局.exe` 是**独立小工具**，不依赖任何 ASI 插件。

## 目录结构

| 路径 | 用途 | 目标位置（包内） |
|---|---|---|
| `winmm.dll` | Ultimate-ASI-Loader（三个游戏同一份，哈希一致） | 游戏根目录 |
| `X86Game4gb.exe` | LAA 补丁器；源码在 [X86Game4gb/](../X86Game4gb/) | 游戏根目录 |
| `清除俄语输入法布局.exe` / `.ps1` | 卸载俄语键盘布局 KLID `00000419` | 游戏根目录 |
| `bat/` | 6 个启动/补丁 bat（按游戏前缀区分，装配时取该游戏的 2 个） | 游戏根目录 |
| `config/base.cfg` `dlc1.cfg` `dlc2.cfg` | 三份**中文 profile 名**的配置（各游戏一份） | `data/config.cfg` |
| `必读说明/dlc.txt` | 安装/卸载/已知问题 —— **系列通用版，三个包共用** | `必读说明.txt` |
| `runtime-x64/` | texconv 在 wine 下运行所需的 4 个 x64 VC 运行库（**仅 CI 构建期用，不进包**） | — |

> 第三方许可原文在 [License/](../License/)，装配时按文件名排序合并成包内
> `License.txt`（做法与 [MajestyIIExtend](../../MajestyIIExtend/) 一致）。

> `X86Game4gb.exe` 与 [X86Game4gb/Release/X86Game4gb.exe](../X86Game4gb/) 哈希一致
> （`3E1DA3B1…`）；`清除俄语输入法布局.exe` 与
> [Clean_RUS_Layout/Release/Clean_RUS_Layout.exe](../Clean_RUS_Layout/) 哈希一致。
> 本目录的副本是**发布的唯一来源**，改源码后要同步更新这里。

### `runtime-x64/` 是什么

[fontgen/texconv.exe](../fontgen/) 是 Windows 程序，导入
`MSVCP140` / `VCRUNTIME140` / `VCRUNTIME140_1` / `VCOMP140`。在 CI 的 wine 里
运行它必须把这 4 个真 DLL 放到 exe 同目录（wine 内置实现不完整；Windows 的
DLL 搜索顺序里 exe 同目录优先）。

它们**不在** msvc-wine 下载的 VC 14.16 工具链里：该工具链的 `Hostx64/x64`
只有 `msvcp140 / vcruntime140 / msvcp140_1 / msvcp140_2`，**没有
`vcruntime140_1.dll`**（VS2019/14.20 才引入）也没有 `vcomp140.dll`（在
VC/Redist 的 OpenMP 组件下，而 vsdownload 不拉 Redist）。

所以把 4 个 x64 DLL 直接入库（共 865 KB），与 `winmm.dll` 同样处理：
不构建、直接用，消除外网下载与 nuget 包 ID 失效的风险。版本是
`14.29.30157`（VC142）—— MSVC 14.x 系列保持二进制向后兼容，可给 VS2017
(14.16) 编出的 texconv.exe 用（本地实测加载成功）。

### 为什么必读说明只有一份（dlc.txt）

`dlc.txt` 是「系列通用」版，明确覆盖三个游戏的差异（部落崛起必须单核启动、
街机版不能单核、`游戏名称.exe.bak` 的还原方式等）。本体专用版信息量更少，
且只提 `hta.exe`，对两个资料片是错的。故三个包统一用这一份。

### 为什么 winmm.dll 直接存在仓库里

它是第三方 Ultimate-ASI-Loader（5.4 MB），**不构建、直接分发**：
与其在 CI 里下载（外网依赖 + 可能失效），不如存一份。三个游戏用的是同一份
（已核对哈希一致），所以只存一个。

## 6 个 bat 的对应关系

| 游戏 | bat 前缀 | 包内文件 |
|---|---|---|
| 本体 `hta.exe` | `hta_` | `hta_【必装】…bat`、`hta_单核…bat` |
| 部落崛起 `Meridian113.exe` | `Meridian113_` | `Meridian113_【从这里进入游戏】单核…bat`、`Meridian113_【必装】…bat` |
| 街机版 `emarcade.exe` | `emarcade_` | `emarcade_【会卡】单核…bat`、`emarcade_【必装】…bat` |

> `build_release.py` 按前缀筛选（每个游戏恰好 2 个），筛选不到就报错退出。

---

## 1. `X86Game4gb.exe` —— LAA（大地址感知）补丁器

给 32 位 PE 打 `IMAGE_FILE_LARGE_ADDRESS_AWARE`，让进程能用满 4GB 用户地址空间。

```bat
X86Game4gb.exe <目标.exe>          :: 例：X86Game4gb.exe hta.exe
```

- 打补丁前会**自动备份**成 `<目标.exe>.bak.N`（N 递增，备份过多会拒绝执行）
- 只改 PE 头 `FileHeader.Characteristics` 里的 `0x0020` 位，不动其它任何字节
- 已打过补丁的文件会直接提示并跳过
- 源码：仓库 `X86Game4gb/`（本 exe 与 `X86Game4gb/Release/X86Game4gb.exe` 哈希一致：
  `3E1DA3B17313C2027AEA649F11B087F7D7362F3D33824140163E667C58625292`）

**为什么必装**：不挂 LAA 时，32 位进程的用户地址空间只有 2GB —— 引擎+汉化字库一起挤在这儿，
装配汉字图集时会因分配失败而崩。挂了 LAA 才有 4GB 可用。

---

## 2. `hta_【必装】扩展32位游戏内存分配来缓解崩溃问题.bat`

就是第 1 项的一键封装（**原始文件名里带"必装"，保留原名以对应玩家手里的版本**）：

```bat
cd %~dp0
call .\X86Game4gb.exe .\hta.exe
pause
```

---

## 3. `hta_单核运行游戏来解决老游戏不兼容问题.bat`

```bat
cd %~dp0
start /AFFINITY 0x2 hta
```

`/AFFINITY 0x2` = 只允许跑在 **CPU 序号 1**（第二颗逻辑核）上 —— 老引擎对多核调度敏感时的
兜底手段。`hta` 不带扩展名，由命令解释器补 `.exe`。

> 注意：这个手段**不能**解决 LAA 崩溃 —— 实测「单核 + LAA + 无调试器」照样崩在
> `dxrender9+0x416D6`（见 [LAA 崩溃分析](../docs/2026-10-07-LAA内存扩展崩溃-根因与Render9Fix.md)）。
> 那个问题由 [Render9Fix](../Render9Fix/README.md) 解决。

---

## 4/5. `清除俄语输入法布局.ps1` / `.exe`

游戏/系统会带进俄语键盘布局（KLID `00000419`），偶尔干扰输入。这个工具把它卸载掉：

```powershell
powershell -ExecutionPolicy Bypass -File 清除俄语输入法布局.ps1
```

实现：`LoadKeyboardLayout("00000419", KLF_NOTELLSHELL)` 拿到**已存在**的布局句柄
（不会新增重复项）→ `UnloadKeyboardLayout`。`.exe` 是同一功能的编译版（无需 PowerShell）。

---

## 资料片（`Meridian113.exe`）怎么用

同一套工具，只把目标名换掉：

```bat
:: 1) LAA 补丁（备份会自动生成为 Meridian113.exe.bak.N）
X86Game4gb.exe Meridian113.exe

:: 2) 单核启动
start /AFFINITY 0x2 Meridian113

:: 3) 俄语布局清理：与本体完全相同
```

两点差异要知道（详见 [PATH_D_DESIGN.md §10](../docs/PATH_D_DESIGN.md)）：

| 项 | 本体 `hta.exe` | 资料片 `Meridian113.exe` |
|---|---|---|
| LAA 补丁 / 单核 / 俄语清理 | ✅ | ✅ 同一套工具 |
| 汉化 ASI | `hta_chs.asi` | ❌ **不能直接复用**：地址不同，需要独立的补丁集（`HardTruckApocalypse_CHINESE_DLL_DLC1/`，目前只有脚手架） |
| 字体引擎特征码 | ✅ | ✅ 同源，`HardTruckApocalypse_CHINESE_DLL/pattern.cpp` 就是为此用特征码而非硬编码地址 |

---

## 本体当前推荐的完整组合

| 组件 | 位置 | 作用 |
|---|---|---|
| LAA 补丁 | 本目录的工具 | 给 32 位进程 4GB 地址空间（汉化与引擎都要） |
| `Render9Fix.asi` | 游戏 `update\` | 把 dxrender9 的小对象分配导向 2GB 以下 —— 让 LAA 不再触发 `dxrender9+0x416D6` 崩溃 |
| `hta_chs.asi` | 游戏 `update\` | 汉化本体（16 位字形索引 + 图集装配） |
