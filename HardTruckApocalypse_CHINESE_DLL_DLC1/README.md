# hta_chs_dlc1 —— 资料片 `Meridian113.exe` 的中文插件

> 目标进程：`I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM\Meridian113.exe`
> 产物：`build\Release\hta_chs_dlc1.dll` → 部署时改名为 `hta_chs_dlc1.asi`，放游戏 `update\`
> 完整的设计/取证说明见 [docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md](../docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md)

---

## 结论先行

引擎**架构上不支持双字节字符**（字形表 `Font+0x40` 恰好 256 项、文本遍历逐字节 `+1`、
`fonts.xml` 的 `<Symbol value>` 在 `strlen != 1` 时被拒）。

本插件走的是**路径 D**（见 [PATH_D_DESIGN.md](../docs/PATH_D_DESIGN.md)）：把字形索引拓宽到 **16 位**，
让汉字直接以 GBK 原值当索引（`(b1<<8)|b2`），汉字上限 65536。

> ⚠ 早期版本的本 README 描述的是「路径 C（256 槽位重映射）」，那只是回落方案；
> 实际生效的是路径 D，实现在 `pathd.cpp`。

---

## 与本体的关系

113 的字体/文本代码与本体 `hta.exe` **同源但三处不同**，所以**不能平移地址**：

| 差异 | 影响 |
|---|---|
| 各段位移互不相干（不存在常数偏移） | 所有锚点独立定位（见下表） |
| 绘制函数比本体**多 0x60 字节** | P4 之后地址整体 +0x60 |
| 预扫循环的索引寄存器 `edi` → **`esi`** | P2/P3 特征码与助手都要改 |
| `Kernel` 的分配器槽位 `+0x18/0x1C/0x20` → **`+0x30/0x34/0x38`** | 只影响内存修复插件（`DLC1_MemFix`），与本插件无关 |

**但裸汇编助手依赖的栈偏移完全一致** —— P4/P4b/P5 的签名字节逐字节相同即证明这点，
所以助手可原样复用。

### 锚点对照（全部 IDA + `sigtest` 双验证）

| 锚点 | hta.exe | Meridian113.exe |
|---|---|---|
| 文本度量 | 0x685990 | 0x690A10 |
| 文本绘制 | 0x685CA0 | 0x690D20 |
| 运行时烘图 | 0x8B9350 | 0x7492D0 |
| `Font::CreateFromXmlNode` | 0x8B80B0 | 0x747D40 |
| `Font` 构造 | 0x8B6740 | 0x746350 |
| P4 主查表 | 0x6865A2 | 0x691682 |
| P4b 第二处查表 | 0x686A26 | 0x691B06 |
| P5 主遍历 | 0x686A52 | 0x691B32 |
| P5 的 4 条 rel32 跳入点 | 0x6864ED/507/58B/59C | 0x6915CD/5E7/66B/67C |
| P7 度量宽度 | 0x685B21 | 0x690BA1 |
| P8 度量推进 | 0x685BD1 | 0x690C51 |
| 度量循环头 / 退出 | 0x685A80 / 0x685C12 | 0x690B00 / 0x690C92 |
| 冻结门（`Application::init` 出口） | 0x5AA388 | 0x6208C1 |
| `Application::init` | 0x5A9040 | 0x61E9C0 |
| GfxServer 入口 | 0x6843F0 | 0x688C50 |
| GfxServer 全局 | 0xA13CC0 | 0xC9BA10 |
| uiCore 全局 | 0xA0A88C | 0xC7BA0C |
| 引擎分配器包装 | 0x589410 | 0x5C0CE0 |
| `RVA_EngineCore` | 0x60A88C | **0x87BA0C**（注意是 RVA 不是 VA） |
| `FontManager` 偏移 | `+0x4A4` | `+0x4A4`（相同） |

---

## 目录内容

| 文件 | 作用 |
|---|---|
| `pathd.cpp` | 路径 D 主体：16 位索引补丁（P4/P4b/P5/P7/P8）、图集装配、逐字体 CJK 表 |
| `font_hooks.cpp` | 字体引擎五个目标的定位（特征码） |
| `render.cpp` | 渲染器原语（预留顶点 / 冲刷批次） |
| `hook.cpp` · `ldisasm.cpp` · `pattern.cpp` | 内联挂钩、长度反汇编、特征码扫描 |
| `text_hooks.cpp` · `transcode.cpp` · `slotmap.cpp` | 路径 C 的回落方案（默认不启用） |
| `dllmain.cpp` · `log.cpp` · `plugin.h` · `pch.*` | ASI 入口与基础设施 |
| `tools/` | **离线校验**：`run_tests.bat` 对 hta.exe 与 Meridian113.exe 双目标逐项断言全部锚点 |

---

## 构建 / 校验 / 部署

```bat
build.bat                          :: -> build\Release\hta_chs_dlc1.dll
tools\run_tests.bat                :: 离线校验（不启动游戏），全绿才注入
```

部署（连同内存修复插件与字库一起）：

```powershell
powershell -ExecutionPolicy Bypass -File ..\dist_dlc1\deploy.ps1
```

字库用 `fontgen\build_cjk.py` 烘焙，产出 `fonts.xml` + `cjk_*.dds` + `hta_chs_cjk_dlc1.bin`。
实测：汉字 **2022** 个 / 字号 **10** 个 / 图集页 512×256 DXT5 **81** 张。

---

## 日志判读

看游戏 `update\hta_chs_dlc1.<时间戳>.log`：

| 现象 | 含义 |
|---|---|
| `包文件载入成功` → `路径 D 准备完成：N 个字号已装入汉字` | ✅ 成功 |
| `字形齐备（2022/2022，无缺失）` | ✅ 该字号全部字形就位 |
| 只有 `初始化完成` 就停住 | ❌ 冻结门没触发（多半是内存不够，见 `DLC1_MemFix`） |
| `[冻结门] ★已等待 … 依赖仍未就绪，放弃★` | ❌ 初始化依赖没交接上 |
