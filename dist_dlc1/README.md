# dist_dlc1 —— 《Hard Truck Apocalypse: Rise of Clans》(Meridian113.exe) 汉化交付包

## 里面是什么

| 文件 | 放到哪 | 作用 |
|---|---|---|
| `winmm.dll` | 游戏**根目录** | Ultimate-ASI-Loader 9.7.4（本体那份，通用） |
| `hta_chs_dlc1.asi` | 游戏 `update\` | 汉化插件：16 位字形索引 + CJK 图集装配 |
| `DLC1_MemFix.asi` | 游戏 `update\` | LAA 内存修复：把 ≥2GB 的小对象分配导向 2GB 以下 |
| `DLC1_MemFix.ini` | 游戏 `update\` | 上面那个的配置（可删，删了用默认值） |
| `data\if\fonts\fonts.xml` | 覆盖游戏 `data\if\fonts\` | 原版 fonts.xml + 81 个 CJK 图集 Item |
| `data\if\fonts\cjk_*.dds` | 同上 | 10 个字号 × 2022 个汉字的图集页 |
| `update\hta_chs_cjk_dlc1.bin` | 游戏 `update\` | 字形包（插件读它填 64K 码表） |
| `update\data\...` | 游戏 `update\data\` | 131 个译文 XML（含 r5/r6/sin 地图文本） |

一键安装（会自动备份原 `fonts.xml`）：

```powershell
powershell -ExecutionPolicy Bypass -File .\deploy.ps1
```

卸载：`.\uninstall.ps1`（恢复 `fonts.xml`，删掉 asi 与 cjk dds）。

---

## 关键尺寸与能力

| 项 | 值 |
|---|---|
| 汉字数 | **2022**（取自 `DLC1_DATA_CHS`） |
| 字号数 | 10（7.813 ~ 18.750） |
| 图集页 | 81 张，512×256 DXT5 |
| 字形索引上限 | 65536（16 位，不再受 256 槽限制） |
| 插件产物 | x86 / GUI / 只依赖 KERNEL32（CRT 静态链接） |

---

## 两个插件分别在修什么

### 1. `hta_chs_dlc1.asi` —— 让引擎能显示汉字

引擎架构上不支持双字节：字形表 `Font+0x40` 恰好 256 项、文本遍历逐字节 `+1`、
`fonts.xml` 的 `<Symbol value>` 在 `strlen != 1` 时被拒。

113 的字体代码与本体 hta.exe **同源但地址不同**，且绘制函数多了 0x60 字节、
预扫循环的索引寄存器从 `edi` 换成了 `esi`。本插件把这些差异逐条对齐（详见
`HardTruckApocalypse_CHINESE_DLL_DLC1/README.md`），运行时：

- 挂钩 `sub_406F50` 的 113 等价物 —— 把 GBK 双字节换成 `[0x1B][槽位]`（等长替换）
- 挂钩文本度量/绘制，让字模索引读到 16 位
- 在 `Application::init` 出口的**冻结门**里同步完成字形装配（时机 100% 确定）

### 2. `DLC1_MemFix.asi` —— 修 LAA 崩溃

113 把渲染器（含 2006 年的 D3DX）**静态链接进 exe**，所以本体的 `Render9Fix`
（hook `dxrender9.dll`）用不了。两份崩溃转储给出同一个现场：

```
Exception: 0xC0000005 at 0x006FA2EA,  ESI = 0  →  read [0]
引擎日志: Kernel.cpp[0317] EffectImpl: failed to load fx file
          'data\shaders\road.fx' with: No D3DX error message.
```

本插件用 IDA 实证的 113 布局（`sub_5C3EE0 = Kernel::Kernel`）：

```
Kernel+0x30 AllocMem   = sub_5C0CE0
Kernel+0x34 ReallocMem = sub_5C0D10
Kernel+0x38 FreeMem    = sub_5C0D00
g_Kernel = dword_C7BA0C (RVA 0x87BA0C)
```

做法与本体 Render9Fix 一致：**不 patch 任何代码**，只替换这 3 个函数指针，
把原本会返回 ≥2GB 的小对象改到低地址池。

### 实测结论（2026-10-08 定案）

「根因是 ≥2GB 地址」已由实机数据验证成立，但有**两个实现细节是决定性的**：

| 细节 | v1（失败） | v2（成功） |
|---|---|---|
| 低地址池大小 | 32 MB —— 在**世界加载前**就满了（`提交 33554432/33554432`），关键时刻一笔都导向不了 | **256 MB** |
| 原块处理 | 故意**不释放**（当时当作"安全设计"）⇒ 64.6 万笔 ≈ **100MB 高地址内存被泄漏**，反过来把引擎顶到更高地址、池也更快耗尽 | 用**配对的 `g_free` 归还**原块 |
| 日志表现 | `回退 56647 → 85420`，`road.fx` 加载失败 → 崩 | `回退 0`，`池 158MB/256MB`，**不再产生新转储** |

> 注意崩溃是**间歇性**的：同一台机器上 `ROC 0001` 那次就成功跑过了 `Roadset loaded`。
> 所以判断"修好了"要看**连续 2~3 次**都稳定，而不是一次没崩。

---

## 怎么看有没有生效

启动游戏后看 `update\` 下的日志：

**汉化（`hta_chs_dlc1.*.log`）**
- 成功：`路径 D 初始化` → `包文件载入成功` → `冻结门` → `[装配] 逐字号填充`
- 失败：只有「初始化完成」就停住 → 冻结门没触发（多半是内存不够）

**内存修复（`DLC1_MemFix.*.log`）**
- 成功：`Kernel 定位: ... 校验通过 ✓` → `探针: 通过` → `安装完成`
- 判读：`统计:` 行里 **`回退` 应该恒为 0**（池不满）；
  **「高地址遗留」**只应是 >64KB 的大块（纹理/顶点缓冲），笔数几百到几千；
  若 `回退` 开始增长，说明池不够 —— 把 `DLC1_MemFix.ini` 的 `ArenaMB` 调大。
- 若进关卡不再崩在 `0x6FA2EA`（`road.fx` 那条路），即为修复成功。
