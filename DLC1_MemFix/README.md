# DLC1_MemFix —— 资料片 `Meridian113.exe` 的 LAA 内存修复插件

> 产物：`build\Release\DLC1_MemFix.dll` → 部署时改名为 `DLC1_MemFix.asi`，放游戏 `update\`
> 完整取证与设计说明见 [docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md](../docs/2026-10-08-资料片113汉化移植与LAA崩溃修复.md)

## 修什么

113 开 LAA 后，引擎内存池抬到 ≥2GB，而 **113 把渲染器（含 2006 年的 D3DX）静态链接进了 exe**，
那段老代码在 ≥2GB 地址上会失效 —— `road.fx` 的 effect 加载失败，随后崩在 `0x6FA2EA`（`read [0]`，ESI=0）。

本体 hta.exe 有同一个签名，但解法（`Render9Fix`）hook 的是 `dxrender9.dll`；113 **没有这个 DLL**，
所以要换 `Kernel` 对象的三个分配器指针。

## 怎么做

**不 patch 任何代码指令，只替换 3 个数据指针**（与 `Render9Fix` 同策略）。
113 的布局（IDA 实证：`sub_5C3EE0 = Kernel::Kernel`）：

```
Kernel+0x30 AllocMem   = sub_5C0CE0     ← 本体是 +0x18
Kernel+0x34 ReallocMem = sub_5C0D10     ← 本体是 +0x1C
Kernel+0x38 FreeMem    = sub_5C0D00     ← 本体是 +0x20
g_Kernel = dword_C7BA0C（RVA 0x87BA0C）
```

装之前两道闸，任一不过就什么都不做（游戏不受影响）：

1. 三个函数地址必须**恰好等于** IDA 期望值（排除「布局猜错」）；
2. **块头魔数探针**：分配 64 字节 → 校验 `user-4 == 0xDEADBEEF` 且 `user+size == 0xFEEBDAED` → 释放。

| 文件 | 作用 |
|---|---|
| `memhook.cpp` | 定位 Kernel、探针、替换三个指针、分配重导向 |
| `lowheap.cpp` | 2GB 以下的定长块分配池（与 `Render9Fix/lowheap.cpp` 同源） |
| `util.cpp` | 日志、模块/段查询、可读性缓存、INI 配置 |
| `dllmain.cpp` | ASI 入口 |
| `r9fix_dlc1.h` | 声明 + 113 布局实证说明 |
| `build.bat` | 一键构建 |

## ★ 两个必须记住的参数（v1 就是栽在这）

| 参数 | 说明 |
|---|---|
| `ArenaMB`（默认 **256**） | v1 用 32MB，**在世界加载前就耗尽**（`回退` 暴涨），关键时刻一笔都导向不了。日志里 **`回退` 必须恒为 0**；开始增长就调大这个值。 |
| 原块处理 | 导向后必须用**配对的引擎 free**把原块还回去。v1 故意不释放，泄漏了约 100MB 高地址内存，反过来把引擎顶到更高地址、池也更快耗尽。 |

## 日志判读

看游戏 `update\DLC1_MemFix.<时间戳>.log`：

| 现象 | 含义 |
|---|---|
| `Kernel 定位 … 校验通过 ✓` → `探针: 通过` → `安装完成` | ✅ 装上了 |
| `统计: … 回退 0` | ✅ 池够用 |
| `未通过校验` / `等待超时` | ❌ 布局判断有偏差（换版本时要重新取证） |
| `回退` 开始增长 | ⚠ 池不够，调大 `ArenaMB` |
