# DLC2_MemFix —— 街机版 `emarcade.exe` 的 LAA 内存修复插件

> 目标进程：`I:\LocalGames\HARD TRUCK APOCALYPSE ARCADE\emarcade.exe`（Death Valley 资料片）
> 产物：`build\Release\DLC2_MemFix.dll` → 部署时改名为 `DLC2_MemFix.asi`，放游戏 `update\`
> DLC1（`Meridian113.exe`）那份是**独立工程** `DLC1_MemFix\`，两者只有 4 个地址值不同

## 修什么（★ 诚实标注：这里是工作假设，不是已取证事实 ★）

emarcade 打上 LAA（4GB 补丁）后，引擎内存池会整体抬到 ≥2GB。而 **emarcade 把渲染器
（含 2006 年代的 D3DX）静态链接进了 exe**，那段老代码在 ≥2GB 地址上会失效。

这个假设的来源是**同签名已在别处取证**：
- DLC1（`Meridian113.exe`）有两份崩溃转储 —— `Exception: 0xC0000005 at 0x006FA2EA, ESI = 0 → read [0]`，
  引擎日志最后一行 `Kernel.cpp[0317] EffectImpl: failed to load fx file 'data\shaders\road.fx'`；
- 本体 hta.exe 是同一个签名（road.fx 的 technique 校验失败 → technique 表为空 → 取 `techArr[0]` 解引用），
  已被 `Render9Fix` 证实并修好。

**emarcade 自己还没有崩溃转储。** 所以本插件的价值是**让你拿到数据**：日志里的
「导向」与「高地址遗留」两个计数，可以验证或推翻这个假设。

### 当前状态：无害空转

emarcade.exe 的 PE `Characteristics = 0x10E`，**没有 LAA 位**（Meridian113.exe 是 `0x12E`）。
32 位进程用户区止于 `0x80000000`，所以 `addr < 0x80000000` 的判断恒不成立 →
插件什么都不改，游戏完全不受影响。**打上 4GB 补丁后本插件才开始真正起作用。**

## 怎么做

**不 patch 任何代码指令，只替换 3 个数据指针**（与 `Render9Fix` 同策略）。
emarcade 的布局（IDA 实证：`sub_935BE0 = Kernel::Kernel`，含 `"0 == g_Kernel"` 与
`"W:\DeathValley\trunk\Core\Kernel.cpp"` 字符串）：

```
Kernel+0x30 AllocMem   = sub_932740     ← 本体 hta.exe 是 +0x18
Kernel+0x34 ReallocMem = sub_932770     ← 本体是 +0x1C
Kernel+0x38 FreeMem    = sub_932760     ← 本体是 +0x20
g_Kernel = dword_C066C4（RVA 0x8066C4）
```

三个包装函数各再转调一层：`sub_932740→sub_9581C0`（真分配在 `sub_957F40`）、
`sub_932770→sub_958230`、`sub_932760→sub_9581E0`。

装之前两道闸，任一不过就什么都不做（游戏不受影响）：

1. 三个函数地址必须**恰好等于** IDA 期望值（排除「布局猜错」）；
2. **块头魔数探针**：分配 64 字节 → 校验 `user-4 == 0xDEADBEEF` 且 `user+size == 0xFEEBDAED` → 释放。

> 槽位偏移 `+0x30/+0x34/+0x38` 与块布局（`user = 块+16`、魔数 `0xDEADBEEF`、尾哨 `0xFEEBDAED`、
> `size` 在 `user-12`）**与 Meridian113.exe 逐字节相同**（`sub_957F40` 反编译实证）——
> 两个资料片是同源构建。这也是为什么 DLC1→DLC2 只需要换 4 个地址。

| 文件 | 作用 |
|---|---|
| `memhook.cpp` | 定位 Kernel、探针、替换三个指针、分配重导向、**emarcade 的 4 个地址常量** |
| `lowheap.cpp` | 2GB 以下的定长块分配池（与 `Render9Fix/lowheap.cpp` 同源） |
| `util.cpp` | 日志、模块/段查询、可读性缓存、INI 配置 |
| `dllmain.cpp` | ASI 入口 |
| `r9fix_dlc2.h` | 声明 + emarcade 布局实证说明 |
| `build.bat` | 一键构建 |

## ★ 两个必须记住的参数（v1 就是栽在这）

| 参数 | 说明 |
|---|---|
| `ArenaMB`（默认 **256**） | v1 用 32MB，**在世界加载前就耗尽**（`回退` 暴涨），关键时刻一笔都导向不了。日志里 **`回退` 必须恒为 0**；开始增长就调大这个值。 |
| 原块处理 | 导向后必须用**配对的引擎 free**把原块还回去。v1 故意不释放，泄漏了约 100MB 高地址内存，反过来把引擎顶到更高地址、池也更快耗尽。 |

## 日志判读

看游戏 `update\DLC2_MemFix.<时间戳>.log`：

| 现象 | 含义 |
|---|---|
| `Kernel 定位 … 校验通过 ✓` → `探针: 通过` → `安装完成` | ✅ 装上了 |
| `统计: … 回退 0` | ✅ 池够用 |
| `统计: 导向 0` 但 `本就是<2GB` 一直涨 | ✅ 正常 —— 还没打 LAA 补丁，插件在空转 |
| `未通过校验` / `等待超时` | ❌ 布局判断有偏差（换版本时要重新取证） |
| `回退` 开始增长 | ⚠ 池不够，调大 `ArenaMB` |