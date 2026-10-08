# hta_chs_dlc2 —— 街机版 `emarcade.exe` 的中文插件

> 目标进程：`I:\LocalGames\HARD TRUCK APOCALYPSE ARCADE\emarcade.exe`
> 产物：`build\Release\hta_chs_dlc2.dll` → 部署时改名为 `hta_chs_dlc2.asi`，放游戏 `update\`

---

## 结论先行

引擎**架构上不支持双字节字符**（字形表 `Font+0x40` 恰好 256 项、文本遍历逐字节 `+1`、
`fonts.xml` 的 `<Symbol value>` 在 `strlen != 1` 时被拒）。

本插件走的是**路径 D**：把字形索引拓宽到 **16 位**，
让汉字直接以 GBK 原值当索引（`(b1<<8)|b2`），汉字上限 65536。

> ⚠ 早期版本的 README 描述的是「路径 C（256 槽位重映射）」，那只是回落方案；
> 实际生效的是路径 D，实现在 `pathd.cpp`。

---

## 与 DLC1 / 本体的关系

emarcade 与 `Meridian113.exe`（DLC1）、`hta.exe`（本体）的字体/文本代码**同源但地址互不相干**，
所以**不能平移地址**：

| 二进制 | 版本 | 说明 |
|---|---|---|
| `hta.exe` | 本体 1.03 | 原始 Steam 版，6.6 MB |
| `Meridian113.exe` | DLC1（Rise of Clans） | 9.3 MB，预扫索引用 `esi` |
| `emarcade.exe` | DLC2（街机版 / Death Valley） | 8.6 MB，预扫索引同样用 `esi` |

**关键事实：** 三个 exe 的裸汇编助手依赖的栈偏移完全一致 —— P4/P4b/P5 的签名字节逐字节相同即证明这点，
所以助手可原样复用；需要改的只是**硬编码地址**。

> 📌 本工程**不**在同一个 DLL 里支持多个版本（那是 DLC1 的旧做法，已被用户否决）。
> 每个 DLL 只服务一个 exe。MemFix 同理，见 `DLC2_MemFix\`。

### 锚点对照（全部 IDA + `sigtest` 双验证）

| 锚点 | Meridian113.exe (DLC1) | **emarcade.exe (DLC2)** |
|---|---|---|
| 文本度量 | 0x690A10 | **0x82DD90** |
| 文本绘制 | 0x690D20 | **0x82E0A0**（另一处 0x9AFC80，取度量之后最近者） |
| 运行时烘图 | 0x7492D0 | **0x8F2940**（栈帧 `A8`，113 是 `A4`） |
| `Font::CreateFromXmlNode` | 0x747D40 | **0x8F13A0** |
| `Font` 构造 | 0x746350 | **0x8EF910** |
| P4 主查表（锚点） | 0x691682 | **0x82EA02**（补丁起点 = 锚点 + 4 = 0x82EA06） |
| P4b 第二处查表 | 0x691B06 | **0x82EE86** |
| P5 主遍历 | 0x691B32 | **0x82EEB2** |
| P5 的 4 条 rel32 跳入点 | 0x6915CD/E7/66B/67C | **0x82E94D / 0x82E967 / 0x82E9EB / 0x82E9FC** |
| P7 度量宽度 | 0x690BA1 | **0x82DF21**（落点 `fadd` = 0x82DF2B） |
| P8 度量推进（锚点 → 推进点） | 0x690C43 → 0x690C51 | **0x82DFC3 → 0x82DFD1** |
| 度量循环头 / 退出 | 0x690B00 / 0x690C92 | **0x82DE80 / 0x82E012** |
| 冻结门（`Application::init` 出口） | 0x6208C1 | **0x7C18BB** |
| `Application::init` | 0x61E9C0 | **0x7BFF80**（首指令 `A1 C4 66 C0 00`） |
| GfxServer 入口 | 0x688C50 | **0x825F60** |
| GfxServer 全局 | 0xC9BA10 | **0xBF3298** |
| uiCore 全局 | 0xC7BA0C | **0xC066C4** |
| 引擎分配器包装 | 0x5C0CE0 | **0x932740** |
| `RVA_EngineCore` | 0x87BA0C | **0x8066C4**（注意是 RVA 不是 VA） |
| `FontManager` 偏移 | `+0x4A4` | `+0x4A4`（相同） |

`sigtest` 里的两条结构不变式断言仍然成立，可当交叉校验：
- 「引擎分配器读的全局」`0xC066B8` **+ 0xC == uiCore** `0xC066C4`
- `RVA_EngineCore == uiCore − 0x400000`

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
| `tools/` | **离线校验**：`run_tests.bat` 对 `emarcade.exe` 逐项断言全部锚点 |

---

## 构建 / 校验 / 部署

```bat
build.bat                          :: -> build\Release\hta_chs_dlc2.dll
tools\run_tests.bat                :: 离线校验（不启动游戏），全绿才注入
```

⚠ 部署前须知：**emarcade 的安装目录没有 `update\` 子目录，也没有 `winmm.dll`**。
部署脚本需要自己创建 `update\` 并放入 Ultimate-ASI-Loader 的 `winmm.dll`，
否则插件不会被加载。

字库用 `fontgen\build_cjk.py` 烘焙，产出 `fonts.xml` + `cjk_*.dds` + `hta_chs_cjk_dlc2.bin`。
实测（113 上验证过的参数）：汉字 **2022** 个 / 字号 **10** 个 / 图集页 512×256 DXT5 **81** 张。

---

## 日志判读

看游戏 `update\hta_chs_dlc2.<时间戳>.log`：

| 现象 | 含义 |
|---|---|
| `包文件载入成功` → `路径 D 准备完成：N 个字号已装入汉字` | ✅ 成功 |
| `字形齐备（2022/2022，无缺失）` | ✅ 该字号全部字形就位 |
| 只有 `初始化完成` 就停住 | ❌ 冻结门没触发（多半是内存不够，见 `DLC2_MemFix`） |
| `[冻结门] ★已等待 … 依赖仍未就绪，放弃★` | ❌ 初始化依赖没交接上 |
| `★…字节 ≠ …，拒绝安装★` | ❌ 校验字节不符 → 说明锚点表与当前 exe 不匹配，**这是设计上的保护**，不会盲写 |

---

## 当前状态

| 项 | 状态 |
|---|---|
| 源码地址改写 | ✅ 完成 |
| `sigtest` 锚点离线校验 | ✅ 全部通过（10 条签名唯一命中） |
| 游戏内实测 | ⏳ 待做（需要先烘焙 CJK 字库 + 建 `update\` + 装 `winmm.dll`） |
| LAA | ❌ emarcade.exe 出厂 `Characteristics = 0x10E`，**没有** LAA 位；需要打 4GB 补丁 |
