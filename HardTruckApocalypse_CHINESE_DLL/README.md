# Hard Truck Apocalypse / Rise of Clans 中文插件（hta_chs）

## 结论先行

引擎**架构上不支持双字节字符**，不是编码选错。必须靠插件把「GBK 双字节」翻译成
引擎能理解的单字节序列。本目录是那个插件。

关键数字：

| 约束 | 值 | 后果 |
|---|---|---|
| 字形表项数 | **恰好 256**（`Font+0x40`，索引 = 零扩展单字节 × 4） | 每字体只有 256 个字形槽 |
| 文本遍历 | 逐字节 `+1`，无 DBCS 判定 | 汉字被拆成 2 个字节分别查表 |
| `_ismbblead` 调用点 | 全镜像仅 `start` 的 CRT argv 解析 | 字体/文本代码从不做前导字节判定 |
| `fonts.xml` `<Symbol value>` | `strlen != 1` 即拒 | 2 字节的汉字字形直接加载失败 |
| 主字号图集 | 256×256，**已用 96%** | 塞不下额外字形 |

因此：**没有「改数据文件」的出路**。`update/` 与 `update_gbk/` 的尝试已在
`exmachina.log` 里留下失败证据：

```
E font.cpp[0137] Font::CreateFromXmlNode error: invalid symbol name Ў­ for font SM_Tahoma-12.000
E font.cpp[0974] FontManager: error while loading fonts
```

---

## 逆向结论（两个游戏共用同一套字体代码）

本体 `hta.exe`（6.6MB）与资料片 `Meridian113.exe`（9.3MB）**字体代码同源但地址不同**。

### 关键函数

| 角色 | hta.exe | Meridian113.exe | 说明 |
|---|---|---|---|
| 文本度量/排版 | `0x685990` | `0x690A10` | 逐字节 `add edi,1`；处理 `@ # & \| $` 内联转义 |
| 文本绘制 | `0x685CA0` | `0x690D20` | 逐字节 `add esi,1`；每字节发射一个 quad |
| 运行时 GDI 烘图 | `0x8B9350` | `0x7492D0` | `CreateFontA`+`ExtTextOutA`，遍历 `off_A05E38` |
| `Font::CreateFromXmlNode` | `0x8B80B0` | `0x747D40` | 在此拒绝 `strlen!=1` 的 Symbol |
| `Font` 构造 | `0x8B6740` | `0x746350` | 在此 `push 0x100` 分配 256 项字形表 |

**两个 exe 的差异**：`draw` 的寄存器分配不同（HTA 把 arg0 存 `ebp`，ROC 存
`[esp+40h]`），无公共长序言 → 用「宽松匹配 + 取 measure 之后最近者」消歧。

### `Font` 对象布局

```
+0x18  float            像素高
+0x30  vector<texId>    图集页表 (begin/end/cap)   ← 引擎自带多页机制
+0x40  vector<void*>    字形表   (begin/end/cap)   ← 初始恰好 256 项
```

### 字形结构（48 字节）

```
+0   char    原始字节
+4   float   abcA          +8  float  width        +12 float  abcC
+16  int     图集页号
+20  float   u0            +24 float  v0
+28  float   u1            +32 float  v1
+36  uint32  纹理句柄对（8 字节）
+44  float   总步进 = abcA + width + abcC
```

### 已死的双字节系统

引擎里有 `TCharDictionary`（`sub_8B5080`），读 `codePage`/`charSet`，会调
`GetCPInfoExA` 并在 `MaxCharSize<2` 时抱怨 "already single-byted"。配置里也注册了
`ui_pathToCharDictionary`、`ui_forceHieroglyphicFont`。

**但它是死代码**：`sub_8B5080` 零交叉引用，不在任何 vtable，无对象构造；
`charDictionary.xml` 不存在；而且它**只做校验、不建映射表**（`chars` 属性读进来就没再用过）。
`ui_forceHieroglyphicFont="yes"` 的唯一作用是在缺字形时换占位符。

---

## 架构

```
游戏字符串 (GBK 双字节)
        │
        ▼
[挂钩点] ──► 转成引擎可逐字节处理的序列 ──► 引擎原有遍历/绘制逻辑（不改）
        │
        ▼
[字形来源] fontgen 预烘字库 或 运行时 GDI 烘图
```

三条可选落地路径：

| | 路径 A：扩表 + 双字节推进 | 路径 C：槽位重映射 | **路径 D：整体替绘（推荐）** |
|---|---|---|---|
| 做法 | 字形表 256→65536；改约 15 处单字节索引 | 保留 256 槽，把汉字映射到槽位 | detour `sub_685CA0`，自己发射 quad |
| 汉字上限 | 65536 | 每字号 128（区间 0x80..0xFF） | 65536+（自建表 + 自建图集页） |
| 改动面 | 大：≥15 处索引站点，每处加载宽度不同 | 中：转换层 + 源码注入 | 中：约 600 行排版逻辑 + 6 个引擎原语 |
| 是否解决测量 | 否（还要另改 `sub_685990`） | 是（等长替换） | 是 |

**推荐路径 D**，理由是它**不需要碰 256 项字形表**。关键逆向发现：

> `sub_685CA0` **不设置任何渲染器状态**。FVF、stride、顶点数组基址全部由调用方
> `sub_687AF0` 在进入前建立（`push 4; call sub_7B0C70`），退出后 `sub_7B0100` 冲刷。
> 因此 detour 入口**即继承一个配置正确的渲染器**，只需调用 3 个原语即可发射 quad：
> `sub_7B0110`（预留顶点）、`IRenderer::vtbl[0x3CC]`（绑定纹理页）、`sub_7AFC50`（冲刷）。

路径 A 之所以更差：单字节索引散布在 **≥15 处独立站点**，且每处的**加载宽度不同**
（裸 `movzx` / `mov al`），必须逐条改写指令；而且**它不解决测量问题**——
`sub_685990` 同样按字节行走，CJK 宽度与换行依旧错误，最终还是得替换它。
一旦写出正确的双字节测量，配套的绘制只是容易的那一半。

当前工程**已具备**路径 D 的全部基础设施（原语定位 + quad 发射 + 纹理上传），
转换层 `transcode.cpp` 也已就位（路径 C 与 D 共用）。

### 槽位字节的禁区（路径 C 的硬约束）

已逐指令核实，绘制路径 `sub_685CA0` 会劫持这些字节：

| 字节 | 行为 | 地址 |
|---|---|---|
| `0x00–0x1F` | 静默跳过、零宽、**不查字形** | `0x6864E3` / `0x685A8B` |
| `0x23` `'#'` | 切状态位，跳过 | `0x68657D` |
| `0x24` `'$'` | 状态位未置位时跳过 | `0x686590` |
| `0x26` `'&'` | 同上 | `0x686595` |
| **`0x40` `'@'`** | **8 字节 hex 颜色转义**：读 `[p+1..p+8]` 送 `sscanf("%x")`，然后 `add esi,8` | `0x6864F2`，`sscanf` @`0x686569` |
| `0x7C` `'|'` | 度量里有状态切换，按危险处理 | `0x685ABB` |

因此 `transcode.cpp` 的槽位**只取 `0x80..0xFF`**（全部 ≥0x20 且避开上述全部禁区），
转义标记取 `0x1B`（<0x20，被静默跳过）。启动时会跑 `SelfTest()` 复核这条不变量
（早期版本用下标 `0..127` 当槽号，会命中 `0x40` 从而**吃掉后续 8 字节**——已是真实 bug）。

### `ui_codePageName` / `TCharDictionary`：半成品，别指望

配置里已经注册了 `ui_codePageName`（默认 `windows-1251`）、`ui_pathToCharDictionary`、
`ui_forceHieroglyphicFont`，看起来像是为多字节准备的。实测结论：

- `sub_5A77C0` 会按 `ui_codePageName` 调 `GetCPInfoExA` 把 **LeadByte 表**存到
  `dword_A0B55C+0x8AF10` —— 但该偏移**只有 2 处引用，都在同函数的报错日志里**，
  引擎拿了表却完全没用。
- `TCharDictionary`（`sub_8B5080`）**零交叉引用**，不在任何 vtable，无对象构造；
  且它**只做校验、不建映射表**（`chars` 属性读进来就没再用过）。
- 附带风险：改 `ui_codePageName` 会同时影响 `sub_8B4DA0`（`TranslateCharsetInfo`），
  进而改变烘图的 GDI 字符集选择。**不要动这个默认值。**

### 为什么不用「原地改字符串」

引擎的本地化文本在**加载期**就被拷进各消费者自己的 `String3d`，节点缓冲由红黑树
共享（`sub_5F5200` 返回的是**map 节点内部指针**，改它会破坏排序键并污染所有消费者）。
所以转换层必须**产出自己的缓冲**，不能原地改。

反之，`sub_414740`（XML 属性 → `String3d`）是**全部 XML 文本字段的唯一入口**，
且第三个参数就是属性名字符串指针（`"text"`/`"value"`/`"briefDiz"`/`"fullDiz"`/
`"literaryDiz"`/`"Name"`/`"FullName"`/`"hirerName"`），可白名单过滤，
天然绕开 `"id"`/`"name"`/`"path"`/`"file"` 这类不能改的键。**这是路径 C/D 的推荐拦截点。**

---

## 目录结构

```
HardTruckApocalypse_CHINESE_DLL/
├─ plugin.h          公共声明：日志、扫描、挂钩、字体引擎、转码层、渲染器原语
├─ pch.h/cpp         预编译头
├─ framework.h       windows.h
├─ log.cpp           hta_chs.log 日志（与 exe 同目录）
├─ ldisasm.cpp       x86-32 长度反汇编器（供 trampoline 用）
├─ pattern.cpp       特征码扫描（含 ?? 通配）
├─ hook.cpp          内联挂钩（trampoline，自动修正相对位移）
├─ font_hooks.cpp    字体引擎定位（5 个目标，已验证）
├─ transcode.cpp     GBK 双字节 → 单字节槽位（含槽位禁区自检）
├─ render.cpp        渲染器原语（预留顶点/绑页/建纹理/上传 + quad 发射）
├─ dllmain.cpp       ASI 入口，独立线程做初始化
├─ build.bat         一键构建 Release/Win32
├─ tools/
│  ├─ sigtest.cpp        离线验证特征码（不注入游戏）
│  └─ build_tools.bat    构建 sigtest
└─ build/Release/hta_chs.dll   产物
```

---

## 构建

```bat
build.bat
```

依赖：Visual Studio（本机为 VS18 Community），工具集 `v141`，Windows SDK 10.0.26100。
产物 `build\Release\hta_chs.dll`，**静态链接 CRT（/MT）**，只依赖 `KERNEL32.dll`。

### 验证特征码（无需启动游戏）

```bat
tools\build_tools.bat
tools\sigtest.exe "I:\...\hta.exe" "I:\...\Meridian113.exe"
```

预期输出「全部通过」。这个工具把 exe 以 `SEC_IMAGE` 映射进自身进程，调用与插件
**完全相同**的扫描代码，因此能提前发现特征码失效，而不用反复启动游戏试错。

> 注意：比较必须在 **RVA 空间**进行。`SEC_IMAGE` 的实际映射基址未必等于
> `ImageBase`，绝对地址不可比。

---

## 部署

`winmm.dll` 是 Ultimate-ASI-Loader（ThirteenAG）。把 `hta_chs.dll` 改名为
`hta_chs.asi` 放进游戏根目录即可被加载。

- 本体：`I:\LocalGames\Hard Truck Apocalypse STEAM\`
- 资料片：`I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM\`
  （该目录**没有 ASI 宿主 DLL**，需额外放一份 `winmm.dll`/加载器）

加载后同目录生成 `hta_chs.log`，内含特征码扫描结果与后续动作日志 —— 排查靠它。

---

## 当前状态

已完成：

- [x] 完整的字体引擎逆向（两个游戏，含地址差异与消歧策略）
- [x] 完整字符串子系统逆向（定位 `sub_414740` 为唯一 XML 文本入口）
- [x] 完整渲染器逆向（确认 `sub_685CA0` 不设渲染器状态 → 可整体替绘）
- [x] 插件骨架：日志、特征码扫描、长度反汇编器、trampoline 挂钩、ASI 入口
- [x] **9 项签名在两个 exe 上全部精确定位**（`sigtest` 离线验证通过）
- [x] 转码层 `transcode.cpp`（含槽位禁区自检，已修掉 `0x40` 吃字节的真实 bug）
- [x] 渲染器原语 `render.cpp`（预留顶点 / 绑页 / 建纹理 / 上传 / quad 发射）
- [x] 一键构建 + 离线验证工具

### 本轮新增（已完成并验证）

- [x] **完整字符串子系统逆向**：确认 `sub_414740`（XML 属性 → String3d）是全部
      XML 文本字段的唯一入口；`sub_406F50` 是文本落到内存的物理必经之路
- [x] **确定 `tcs`/`abc` 精确语义**（读 `sub_8B80B0` @0x8B8B7F-0x8B8BBE 确认）：
      `tcs = [page, u0, v0, u1, v1]`，`abc = [abcA, width, abcC]`；
      `height` 是**点**，点阵高 = `height/72*120`（12pt → 20px 单元）
- [x] **字库烘焙管线** `fontgen/build_atlas.py`
- [x] **文本转码挂钩** `text_hooks.cpp`：钩 `sub_406F50`，**就地等长**替换
      GBK 双字节 → `[0x1B][槽位]`，因此引擎原有度量与绘制**零改动**即可工作
- [x] **槽位映射表加载** `slotmap.cpp`（外部文件，与离线烘的字库严格配套）
- [x] **转码逻辑单测** `tools/test_transcode.cpp`：8 项断言全通过
- [x] **★ 实机验证通过（hta.exe）**：主菜单正确显示「新游戏」「读取存档」，
      尺寸/位置/颜色与其它按钮一致（见 `update_chs_test/game_screenshot6.png`）

### ⚠ 字库烘焙的六个致命坑（都已踩过并修正，勿改回去）

这六条都是实机调试出来的，任何一条错了都会导致「字形错乱 / 压扁 / 缺下半截 /
整个字体加载失败」：

1. **图集尺寸绝对不能改**
   引擎算字形屏幕尺寸用的是 `sub_8B5AE0`：
   ```
   宽 = (u1-u0) * texW * Font[+0x20]
   高 = (v1-v0) * texH * Font[+0x20]
   ```
   `texW/texH` 来自 `sub_66FF30`，实测等价于**固定的字体基准尺寸**（≈255×256），
   **不随 DDS 的真实像素高度变化**。所以一旦把图集从 256 加高到 512 并
   按 512 归一化 v，所有字形的 `(v1-v0)` 都会变成一半 → 屏幕上一律**纵向压扁**。
   → 汉字只能放进原有 `v∈[0,1]` 的像素范围内。

2. **槽号必须所有字号统一（不能各字号各自征用）**
   转码发生在 `sub_406F50`（String3d 赋值）时，那时**还不知道字号**，
   所以一个 GBK 汉字只能映射到唯一一个槽号。
   若让各字号各自「从底部往上征用」，不同字号行结构不同 → 征用到的槽号集合
   也不同，而 slotmap 只能写一份 → 只有参考字号正确，其余字号拿这个槽号去查
   **另一个格子**，采到错误像素区域（表现为字形被截断/错位）。
   → 先用参考字号算出一份全局槽位顺序，所有字号都用这批槽号。

3. **汉字格子必须是方形，且要"整行征用"**
   原图集的西里尔槽位是按**字母墨迹宽度**紧凑排布的，单格只有 9~12px 宽、20px 高
   （宽高比约 0.45）。汉字是方块字，塞进 9px 宽的格子里只能缩成 9×9 贴在格子中间
   → 画面上表现为「字小得只剩一点点、贴在最上边」。
   → 必须**从底部往上整行征用**，把整行 `W` 宽度按 `cell` 重切成方形格子，
   该行原有的 Symbol 全部移除。

4. **PIL 画布必须按字体真实度量给足高度，否则静默裁掉字形底部**
   `ImageFont.getmetrics()` 在 size=N 时返回的 `ascent+descent ≈ 1.47N`
   （实测 size=17 → (20,5) → 共 25px）。若画布只有 `N+8=25`，加上 `pad=4`
   就需要 29px —— **底部 4 行直接没了**，实机表现为「汉字下半截显示不出来」。
   诊断特征：`getbbox()` 的底边**正好等于画布边界**。
   → 画布高一律用 `ascent + descent + 2*pad`。

5. **缩放必须用统一系数 `k = box/em`，不能按各自墨迹缩放**
   若对每个字独立地「把墨迹缩放到填满 box」，笔画多的字（藏）和笔画少的字
   （一、口）会被拉到同一外接尺寸 —— 字库里看是"都填满了"，
   实机上就是**高矮不齐**。
   → 所有字形共用同一个 `k = box/em`，只与 em 有关、与具体字形无关，
   这样「一」保持细线、「口」自然偏小，相对比例与字体设计一致。
   自检：各字墨迹高 / 字体自然墨迹高 的比值应基本恒定（实测 ≈0.84）。

6. **`value` 属性必须回写"原文"**
   原 `fonts.xml` 用 `value='"'`（**单引号**）表示双引号字符，用 `&amp;` 表示 `&`。
   如果把 value 解析成字符再用双引号重新拼出去，会产生 `value="""` 这种非法 XML，
   引擎会**直接拒绝加载整个 fonts.xml** 并回退到原字体
   （日志：`ReadXmlFile: Cannot parse file`）。
   → 解析时保留 `value=` 的原始文本（含引号形式与实体），回写时原样输出。

### 关键简化（相比早期方案）

早期计划要「替换绘制函数 `sub_685CA0` + 自建 65536 字形表 + 自建图集页」。
读透引擎后发现有**更简单且已验证**的路子：

> 引擎的度量与绘制对 `<0x20` 的字节**一律静默跳过、零宽度、不查字形表**
> （`sub_685990` @0x685A91、`sub_685CA0` @0x6864E6）。
> 于是把 GBK 双字节换成 `[0x1B][槽位]`，引擎**原有的两个循环一行都不用改**
> 就得到正确结果 —— 因为 `[0x1B]` 贡献 0 宽度，`[槽位]` 被当普通单字节字形渲染。
> 而且这是**等长替换**（2→2 字节），所以不需要额外缓冲、不改长度/容量、
> 不涉及所有权，可以安全地**就地**做。

因此最终架构只有一条挂钩（`sub_406F50`），不需要替换任何引擎函数。
`render.cpp` 的替绘原语保留备用（若将来汉字数超过单字号槽位上限，
需要自建表 + 自建图集页时才用得上）。

### 槽位容量与部署

| 项 | 值 |
|---|---|
| 单字号可用槽位 | **127** 个（`0x80..0xFF` 去掉被劫持的 `0x40 '@'`） |
| 汉字字形尺寸 | 单元 = `height/72*120` px，等宽推进 |
| 图集 | 高度翻倍（256×256 → 256×512），上半部原样、下半部放汉字 |
| 若译文超过 127 字 | 需改用自建表 + 自建图集页（`render.cpp` 已备好原语） |

部署三步：

```bat
REM 1) 烘字库（用实际译文，或先用探针字符集试）
python fontgen\build_atlas.py --text-dir Original_DATA_CHS --text-dir DLC1_DATA_CHS --out update_chs

REM 2) 覆盖数据 + 放映射表
REM    update_chs\data            -> 游戏 update\ 下
REM    update_chs\hta_chs_slotmap.txt -> 游戏根目录（与 hta_chs.asi 同目录）
REM    build\Release\hta_chs.dll -> 改名为 hta_chs.asi，放游戏根目录

REM 3) 启动游戏，看同目录 hta_chs.log
```

### 前置依赖：翻译内容

当前工作区的 `Original_DATA_CHS/` 与 `DLC1_DATA_CHS/` 与原文**逐字节完全相同**
（已用 MD5 全量比对：112 + 122 个文件，0 处差异），也就是说**还没有真正的译文**。

因此本插件与字库管线**已就绪但尚未在实机跑通完整汉化** —— 缺少的是译文本身，
不是技术环节。拿到译文后：

```bat
python fontgen\charset_report.py     REM 先看需要多少个不同汉字
python fontgen\build_atlas.py --text-dir ... --out update_chs
```

`charset_report.py` 会直接给出「127 个槽位够不够」的结论。

### 离线验证（不需要启动游戏）

```bat
tools\run_tests.bat
```

两步：特征码扫描（对两个 exe 共 18 项断言）+ 转码逻辑单测（8 项断言）。
当前状态：**全部通过**。

