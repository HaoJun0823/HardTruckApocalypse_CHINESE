# Render9Fix 端到端合成测试

**不启动游戏**，把 Render9Fix 的完整链路真跑一遍：定位 Kernel → 魔数探针 → 装钩子 →
按策略导向 → 释放/重分配。用来在改动插件后做回归。

```powershell
powershell -ExecutionPolicy Bypass -File Render9Fix\test\build_and_run.ps1
```

## 它怎么模拟真实环境

| 真实环境 | 本测试的替身 |
|---|---|
| `hta.exe` 里的 `Kernel` 对象（vtable@0、`g_mar.AllocMem@0x18`/`ReallocMem@0x1C`/`FreeMem@0x20`） | `host.cpp` 里的 `FakeKernel : VTableSource`（成员顺序刻意与真实布局一致，`offsetof` 会打印出来核对） |
| `m3d::g_Kernel` 全局指针 | `host.cpp` 的 `g_kernelPtr`（可写数据里的一个指针，插件靠扫描它定位） |
| 驱动 DLL 里的 kernel 指针副本 | `r9fakedx.cpp` 编译成**名为 `dxrender9.dll`** 的假驱动，`Prepare()` 把同一指针写进自己的数据段 |
| 引擎 `MemoryManager::Malloc/Free` 的块布局（12 字节头 + 数据 + 4 字节尾哨） | `host.cpp` 的 `EngineAlloc/EngineFree/EngineRealloc`（`0xDEADBEEF` / `0xFEEBDAED`，用户指针 = block+12） |
| D3DX 通过 `operator new` 发起的小对象分配 | 假驱动 `RunTest()` 里通过 `kernel->g_mar.AllocMem` 的一组分配 |
| 假驱动的**调用方模块名** | 真实场景靠 `_ReturnAddress()` 落在 dxrender9.dll 内来判定，这里就是那个假 DLL |

## 判据

| 请求大小 | 期望 |
|---|---|
| ≤ 65536（16/64/220/1024/2200/9686/65536） | 全部落在低地址池 `0x60xxxxxx`（< 2GB）→ `OK redirected` |
| 100000（> `SizeMax`） | 原样走假引擎分配器 → `OK left-to-engine` |

另外会验证：内容完整性、释放不崩、`realloc` 放大后内容保留且仍在池内、`realloc` 缩小原地成功。
**任何一行出现 `FAIL` 或 `CONTENT-CORRUPTED` 就是回归。**

参考输出（2026-10-07）：

```
扫描: hta 候选 1 / 驱动候选 1 / 合计 1 个值
  候选: 0x0069C000[hta+drv]
选中 0x0069C000（hta 与驱动 DLL 里同时出现）
探针: 块头 = [0xFFFFFFFF 0x00000040 0xDEADBEEF] 尾哨 = 0xFEEBDAED
探针: 通过 —— 分配器配对确认
安装完成: 只导向 dxrender9.dll 且 <= 65536 字节的分配
```

| size | ptr | zone | verdict |
|---|---|---|---|
| 16 / 64 / 220 / 1024 / 2200 / 9686 / 65536 | `0x6000xxxx` | pool(<2GB) | OK redirected |
| 100000 | `0x00D37B14` | other-low | OK left-to-engine |

## 注意

- 测试目录里的 `Render9Fix.dll`、`host.exe`、`dxrender9.dll`、`*.obj/lib/exp`、`*.log`
  都是**构建产物**，已在 `.gitignore` 里忽略。
- 假驱动的名字必须叫 `dxrender9.dll`（插件按模块名/地址范围判定调用方）；
  所以**不能在游戏目录里跑**这个测试，否则会与真驱动混淆。
- 这个测试只覆盖"分配导向"这条链，不覆盖游戏内的实际渲染效果 —— 那要靠实机跑关卡。
