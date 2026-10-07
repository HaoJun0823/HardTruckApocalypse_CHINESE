# LAA 内存扩展崩溃：根因分析与 Render9Fix

> **状态：根因已定位，已修复并实机验证**（2026-10-07）。
> 修复实现：`Render9Fix/`（独立 ASI，运行时把 dxrender9 的小对象分配导向 2GB 以下）。
> 设计文档：[Render9Fix/README.md](../Render9Fix/README.md)
>
> 一句话结论：**hta.exe 开 LAA 后引擎内存池抬到 ≥2GB，而 dxrender9.dll 里静态链接的
> D3DX（2006 年代码）在 ≥2GB 地址上会失效 —— effect 的 technique 全部"验证失败"被跳过，
> 计数变 0，随后取 `techArr[0]`（NULL）崩溃。** 与汉化无关。

---

## 1. 症状与已知条件

| 条件 | 结果 |
|---|---|
| 不开 LAA（原版 hta.exe） | **不崩**（但汉化会因 2GB 地址空间不足而分配失败） |
| 开 LAA + 无调试器 | **进世界/关卡时崩在 `dxrender9+0x416D6`** |
| 开 LAA + x32dbg 挂载 | **不崩**（游戏变慢） |
| 开 LAA + 单核（affinity 1 核） | **仍然崩** |
| 开 LAA + 我们的调试器注入 4ms 延迟 | **不崩**（<4ms 不稳定，5ms 基本稳） |
| `debugMode="false"` | 无影响 |

崩溃时机固定在「世界加载完成、第一次渲染」附近，`exmachina.log` 里总是紧跟
`world.cpp[0247] ---- World loaded in:` 之后。

### 崩溃签名（跨 4 个 dump 逐字节一致）

```
EIP = dxrender9+0x416D6   (mov eax,[eax]，eax = [esi+0x9FC])
EAX=00000000  EBX=00000000  ECX=00000008  EDX=FFFFFFFF  EBP=00000001
ESP=039C3EA0  ESI=0x83xxxxxx（≥2GB）
```

`[esi+0x9FC]` 是 effect 的 technique 数组首项，`[esi+0xA08]` 是默认 technique 索引；
崩溃路径是「没有默认 technique → 用第一个」的兜底代码取 `techArr[0]`。

---

## 2. 取证手段

### 2.1 透明调试器 `tools/null_dbg.ps1`

自己实现的一个"什么都不做"的调试器，用来复刻 x32dbg 的效果并可精确控制延迟：

- 自动用 32 位 PowerShell 重启（`SysWOW64\WindowsPowerShell\v1.0\powershell.exe`，游戏是 32 位）
- `CreateProcess(DEBUG_ONLY_THIS_PROCESS)` / `DebugActiveProcess` / `WaitForDebugEvent` / `ContinueDebugEvent`
- `-Mode Spawn|Attach`、`-Quality Fast|Slow`、`-DelayMs N`（每个调试事件延迟 N 毫秒）
- **崩溃现场取证**：first-chance AV 落在 dxrender9 内时，放行前抓取并写 `[FX]` 行
  - effect 文件名（`EffectImpl+0x0C` 的 `CStr`）、technique 计数/数组指针/默认索引
  - 全线程 `GetThreadContext` 快照（谁的 EIP 在 dxrender9 里）
  - 每个 dxrender9 线程的栈扫描（找栈上的返回地址）
- 日志：`exceptions\nulldbg_<时间戳>.log`

实测档位：`Fast`（无延迟）约 1/2 概率复现崩溃；`-DelayMs 3` 不稳定；`4` 起基本稳定；`5` 很稳。

### 2.2 转储与日志解析

- minidump 扫描：`tools\cmp_dumps.py`（批量解析 ThreadList/ModuleList/MemoryList/ExceptionStream，
  寄存器值 + 所属模块偏移 + 栈上返回地址解析）
- 游戏自身日志：`exmachina.log`（`LogMsg` 输出），崩溃现场的最后一行是关键证据

---

## 3. 证据链（逐层递进）

### 3.1 崩溃现场：单线程、干净的"零"

```
[FX] dxrender9 base = 0x13B00000
[FX] ===== forensics: AV at dxrender9+0x0416D6 tid=191336 =====
[FX] crash thread: EIP=0x13B416D6 ESP=0x039C3EA0 ESI=0x83BE8CF4
[FX] effect file   = 'data/shaders/road.fx'
[FX] techCount     = 0
[FX] techArr       = 0x00000000
[FX] defaultIdx    = -1 (无默认)
[FX] threads=56  inDxRender9=1
```

两点决定性信息：

1. **`techCount=0 / techArr=0 / defaultIdx=-1` 三者互相自洽** —— 不是内存被写坏
   （那样会看到垃圾指针），而是"确实一个 technique 都没收集到"。
2. **`inDxRender9=1`** —— 崩溃瞬间全进程 56 个线程里只有崩溃线程自己在 dxrender9 内，
   其余全在 `ntdll` 的等待状态。**没有并发方。**

`road.fx` 本身只有 **1 个** technique（`Test1`，还带 `Default = true`）：

```
93: technique Test1
97: string  VertexFormat = "VERTEX_XYZNT1";
98: bool    Default = true;
```

### 3.2 关键钥匙：日志里那串"乱码"

`exmachina.log` 最后一行：

```
EffectImpl() error: effect = 'data/shaders/road.fx',
could not validate technique '<乱码>', skipping it...
```

把那串乱码的 **29 字节原始字节**取出来：

```
c3 6a 0d e8 a5 ff ff ff 59 c3 6a 0c e8 9c ff ff ff 59
c3 6a 10 68 c0 a3 37 7c e8 5d 01
```

这是**一段真实的 x86 机器码**（`ret; push 0xD; call -0x5B; pop ecx; ret; …`，
其中 `68 C0 A3 37 7C` = `push 0x7C37A3C0` 是绝对地址）。扫遍所有相关二进制：

```
EXACT-29  HIT: msvcr71.dll @+0x218F      ← 精确唯一命中
thunk12   HIT: msvcr71.dll @+0x2190
```

`msvcr71.dll+0x218F` 就是 **`_free` 的 `retn` 指令**。而 `0x7C34218A` / `0x7C34218F`
以"栈残留"形式出现在**每一个**崩溃 dump 的栈上（`039C3EB0` / `039C3F0C` / `039C3F18`）。

**⇒ 结论：`techDesc` 结构体根本没被写，打印出的 `Name` 是栈上残留的旧返回地址。**

### 3.3 对应源码：`GetTechnique()` 返回了无效句柄

retruxx（1:1 还原）`dxrender9/shaders/effects/effect.cpp:434-445`：

```cpp
D3DXHANDLE techHandle = m_effect->GetTechnique(i);          // ← 这里返回了无效句柄
D3DXTECHNIQUE_DESC techDesc;
m_effect->GetTechniqueDesc(techHandle, &techDesc);          // ← HRESULT 没检查！techDesc 保持栈垃圾
if (m_effect->ValidateTechnique(techHandle) != D3D_OK) {    // ← 无效句柄必然失败
    LogMsg(CStr::format_("EffectImpl() error: effect = '%s', could not validate technique '%s', skipping it...",
                         m_fileName.c_str(), techDesc.Name));    // ← 打印栈垃圾
    m_numTechniques--;
    continue;
}
```

`m_effect` 本身是**有效的**（否则 `GetDesc`/`GetTechnique` 会在虚表调用时先崩），
所以是 D3DX 对象**内部**状态不对。

### 3.4 排除法

| 假设 | 排除依据 |
|---|---|
| 2GB 内存溢出 | 实测 `mem used: 131,357,889`（约 125 MB）；4 个 dump 里**没有任何 ≥2GB 的内存区** |
| 多线程竞争/堆破坏 | `inDxRender9=1`（无并发方）；且**单核 + LAA 仍然崩** |
| 双核竞态 | 同上，单核崩 |
| 调试器端口存在与否 | 延迟 0ms 会崩、4ms 不崩 —— 同一进程同一条调试通路，差别只有时间 |
| `debugMode` 配置 | 用户实测改成 `false` 无影响 |
| 设备兼容白名单（`devicecompatible.xml`） | 未改动时同样崩/不崩 |
| 汉化导致 | 非 LAA 下汉化不崩（是另外的 2GB 分配失败）；本崩溃与汉化路径无关 |

### 3.5 为什么"跟时间有关"：地址布局

所有"能治好"的手段（x32dbg 挂载、4ms 调试事件延迟）都不改变任何**单线程计算结果**，
它们只改变**各线程的分配交错**（尤其 FMOD 音频线程，实测 83 MB 分配量）。

⇒ 交错变 ⇒ 引擎内存池（`MemoryManager` 的 chunk）布局变 ⇒ 对象落在 2GB 的哪一侧变。

跨 dump 统计出事的 `EffectImpl` 地址：

| dump | EffectImpl 地址 | ≥2GB? |
|---|---|---|
| WER 7216 / 197716 | `0x83BE8CF4` / `0x83BEE064` | 是 |
| hta.exe0001 | `0x65AAF12C` | **否** |
| 基线 0008/0009 | `0x83BF228C` / `0x83C7FFE4` / `0x84C51C84` | 是 |

两个区间都存在 —— 布局决定生死。

### 3.6 结论（**推理结论，非直接证明**）

> dxrender9.dll 里静态链接的 D3DX（2006 年代码）在 **≥2GB 的地址**上会出错
> —— 典型的"指针被当成有符号 32 位整数参与运算"类老代码问题。

证据强度：**强**（唯一能同时解释全部 6 项观察的假设，且修复后实机验证通过），
但**没有**直接反汇编出那一处有符号运算（D3DX 静态库代码量大，未逐函数排查）。

---

## 4. 静态验证（IDA，hta.exe）

### 4.1 `Kernel::Kernel`（`sub_58C480`）—— 结构与偏移

```c
*a1          = &off_98F770;              // +0x00 vtable
a1[9] = 0;                               // +0x24 m_Log
assert(!g_Kernel); dword_A0A88C = a1;    // ← m3d::g_Kernel（RVA 0x60A88C）
v5 = new MemoryManager(0x90);            // +0x04 m_memMan，同时写入 dword_A0A880
a1[6] = sub_589410;                      // +0x18 g_mar.AllocMem    (RVA 0x189410)
a1[7] = sub_589440;                      // +0x1C g_mar.ReallocMem  (RVA 0x189440)
a1[8] = sub_589430;                      // +0x20 g_mar.FreeMem     (RVA 0x189430)
```

三个包装分别是：

```c
sub_589410(a1,a2,a3) → sub_748DC0(dword_A0A880, a1,a2,a3);   // MemoryManager::Malloc
sub_589440(a1,a2,a3,a4) → sub_748FA0(dword_A0A880, ...);     // MemoryManager::Realloc
sub_589430(p) → sub_748EE0(dword_A0A880, p);                 // MemoryManager::Free
```

### 4.2 `MemoryManager::Malloc`（`sub_748DC0`）—— 块布局

```c
EnterCriticalSection(this + 92);
if (!size) size = 1;
if (size > 1024) {                       // 大块：独立 chunk
    v12 = sub_748A00(size, 1);
    v12[0] = -1;                         // nextBlock = -1（独立块）
    v12[1] = size;                       // m_size
    v12[2] = 0xDEADBEEF;                 // m_magic
    *(int*)((char*)v12 + size + 12) = 0xFEEBDAED;   // 尾哨
    return v12 + 3;                      // ★ 用户指针 = block + 12
}
// 小块：7 档池（16/32/64/128/256/512/1024）
v8[0] = v6;                              // nextBlock = 尺寸档号
v8[1] = size; v8[2] = 0xDEADBEEF;
*(int*)((char*)v8 + size + 12) = 0xFEEBDAED;
return v8 + 3;
```

即用户指针 `p` 处：`p-12 = nextBlock`、`p-8 = size`、`p-4 = 0xDEADBEEF`、`p+size = 0xFEEBDAED`。

### 4.3 `MemoryManager::Free`（`sub_748EE0`）—— 为什么安装顺序很关键

```c
v4 = (int*)(p - 12);
if (*(int*)(p - 4) != 0xDEADBEEF)  __debugbreak();      // ★ 魔数不符 = int3
if (*(int*)(p + v4[1]) != 0xFEEBDAED) __debugbreak();   // ★ 尾哨不符 = int3
...
if (v4[0] == -1) free(p - 28);        // 独立 chunk
else { /* 挂回尺寸档空闲链 */ }
```

⇒ 如果引擎自己的 `Free` 收到了我们池里的指针，会**直接 int3 崩溃**。
这就是 Render9Fix 必须「先装 Free/Realloc、最后装 Alloc」的原因。

### 4.4 发行版 `dxrender9.dll` 的独立佐证

`sub_1002A400`（纹理加载）里有：

```c
v23 = (*...)(v11);                       // 文件大小
v22 = (*(kernel + 24))(v23, 0, 0);       // ★ [kernel+0x18] = g_mar.AllocMem(size, 0, 0)
(*...)(v11, v22, v23);                   // 读入缓冲
```

读的是 `dword_101A72E0`（驱动自己的 kernel 指针副本）+ 偏移 **0x18** ——
与我们的实现逐字节吻合。

---

## 5. 修复：Render9Fix

不改可执行文件、不挂钩任何代码，只替换 Kernel 结构里的 **3 个数据指针**：

```
1. 抢一段 2GB 以下的地址空间（默认预留 32MB，按需提交），建定长块小分配池
2. 定位 Kernel（已知布局快路径 RVA 0x60A88C → 失败则通用扫描）
3. 魔数探针实测一次「分配 64 字节 → 校验块头/尾哨 → 释放」
4. 通过后按「Realloc → Free → Alloc」顺序替换三个指针
5. 只拦截「调用方在 dxrender9.dll 内」且「大小 ≤ 64KB」的分配 → 低地址池；
   其余原样转交引擎分配器
```

关键取舍：

| 决定 | 理由 |
|---|---|
| 只导向 dxrender9 的小对象（≤64KB），不动纹理/顶点缓冲 | 恢复 D3DX 需要的地址语义即可；低地址池占用仅 ~1.8 MB，LAA 的 4GB 照旧可用 |
| 按**地址**分派 Free/Realloc | 换指针瞬间的并发分配/释放也不会串味 |
| 先装 Free/Realloc、最后装 Alloc | 见 §4.3：我们的块绝不能落到原 Free 手里 |
| 池满自动回退引擎分配器 | 绝不因池耗尽让分配失败 |
| 命名互斥体 + 二次安装检测 | 防止 asi 被放两份时把自己的钩子当"原函数"再包一层 |
| DLL 卸载时**故意不还原**指针 | 还原后引擎会用原 Free 释放我们已发出的块（§4.3） |

细节与配置见 [Render9Fix/README.md](../Render9Fix/README.md)。

---

## 6. 验证

### 6.1 端到端合成测试（不启动游戏）

构造「假 Kernel + 假 dxrender9.dll」（`Render9Fix/test/`，一键 `build_and_run.ps1`），把完整链路真跑一遍：

```
扫描: hta 候选 1 / 驱动候选 1 / 合计 1 个值  →  候选: 0x0069C000[hta+drv]
探针: 块头 = [0xFFFFFFFF 0x00000040 0xDEADBEEF] 尾哨 = 0xFEEBDAED  →  通过
```

| 请求大小 | 返回地址 | 判定 |
|---|---|---|
| 16 / 64 / 220 / 1024 / 2200 / 9686 / 65536 | 全部 `0x6000xxxx`（池内，< 2GB） | ✅ 已导向 |
| 100000（> 阈值） | `0x00D37B14` | ✅ 原样走引擎 |

释放 8 块无崩溃、内容完整；`realloc` 放大/缩小都正确。

### 6.2 实机（2026-10-07 21:04，Steam 版 hta.exe）

```
已知布局命中: g_Kernel 槽位=0x00A0A88C(RVA 0x60A88C) → Kernel=0x00A0A890;
             Alloc/Realloc/Free = 0x00589410/0x00589440/0x00589430
             （期望 RVA 0x189410/0x189440/0x189430：完全一致 ✓）
探针: 块头 = [0x00000002 0x00000040 0xDEADBEEF] 尾哨 = 0xFEEBDAED → 通过
统计: 导向 254332 笔 / 161434383 字节 | 池: 存活 567200 峰值 1821472 提交 3997696/33554432
      | 高地址遗留 2 笔 / 262400 字节 | 回退 0
```

| 指标 | 实测 | 判读 |
|---|---|---|
| 导向量 | 41 秒 254,332 笔 / 161 MB 累计 | 小对象分配极频繁，全部落回 <2GB |
| 低地址池占用 | 峰值 **1.82 MB**、提交 3.9 MB / 32 MB | 对 2GB 以下空间几乎零影响 |
| 池耗尽回退 | **0 次** | 32 MB 预留绰绰有余 |
| 高地址遗留 | 2 笔，均 131200 字节 | 见下 |

那 2 笔遗留经 IDA 定位是 `sub_1002A400`（纹理加载，
含 `CreateTextureFromFileInMemoryEx error` 字符串）——**把一个 128KB 的 DDS 文件整体读入的缓冲**，
与 effect/technique 路径无关，且超过 64KB 阈值所以按设计留给引擎。

### 6.3 游戏日志：根因被消除的直接证据

```
-- Loading Level: data\maps\mainmenu.ssl --
World loaded in: 0        ← 原来崩在这之后
Roads loaded in: 0        ← road.fx 所在的道路资源
Engine inited in: 0       ← 主菜单初始化完成
```

整份 `exmachina.log` 里**没有任何** `could not validate technique` / `failed to load fx`。

**这条最关键**：如果是靠时序侥幸绕过去，日志里仍会出现"验证失败被跳过"；
现在是**验证本身成功了** ⇒ `GetTechnique(0)` 不再返回无效句柄 ⇒ 地址问题被消除。

---

## 7. 排除过的错误方向（避免重复走）

| 方向 | 为什么当时诱人 | 为什么排除 |
|---|---|---|
| 2GB 溢出 / 分配失败 | LAA 就是为了突破 2GB，直觉上"越界" | 实测只用了 125 MB；dump 里无 ≥2GB 区 |
| 多线程竞态 | 崩溃点像"数据被并发写坏" | `inDxRender9=1`；单核仍崩 |
| 双核调度 | 老游戏常见 | 单核仍崩 |
| 栈残留 `7C34218A` 是真调用边 | 它在每个 dump 的栈上都出现 | 是 `_free` 内部的返回地址，属栈残留（曾据此查过 `msvcr71+0x218A`，是假的调用边） |
| 汉化插件的分配破坏堆 | 时间上"装了汉化才崩" | 非 LAA 下汉化不崩；本崩溃路径与字体/字符串无关 |
| 堆破坏（魔数/尾哨） | 引擎有 `0xDEADBEEF`/`0xFEEBDAED` 校验 | 崩溃现场三个字段自洽（count=0/arr=0/default=-1），不是垃圾 |
| `ReloadShaders` 参与崩溃链 | 它也在 effect 代码里 | 崩溃链是 `hta → vtable sub_10040EB0 → sub_10041740 → ValidateEffect`，与它无关 |
| `debugMode` / 设备白名单 | 便于快速排除 | 实测均无影响 |

---

## 8. 复现与工具

```powershell
# 1) 不装 Render9Fix，用透明调试器复现崩溃并取证（Fast = 无延迟）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\null_dbg.ps1
#    → 抓 exceptions\nulldbg_*.log 里的 [FX] 行

# 2) 批量比对 minidump
python tools\cmp_dumps.py '<dump 通配>' out.txt

# 3) 扫 fx 文件的 technique 数量（找"单 technique"风险文件）
python tools\fxscan.py
```

稳定复现档位：`-Quality Fast`（约 1/2）；`-DelayMs 3` 不稳定；`≥4` 基本治住。

---

## 9. 相关文件

| 文件 | 内容 |
|---|---|
| [Render9Fix/README.md](../Render9Fix/README.md) | 插件设计、安全设计、配置、日志判读 |
| [Render9Fix/r9fix.h](../Render9Fix/r9fix.h) | 诊断链与安全设计（源码注释版） |
| [Render9Fix/memhook.cpp](../Render9Fix/memhook.cpp) | Kernel 定位（快路径 + 扫描 + 探针）与三个挂钩 |
| [tools/null_dbg.ps1](../tools/null_dbg.ps1) | 透明调试器（延迟注入 + 崩溃取证） |
| [tools/null_dbg_check.ps1](../tools/null_dbg_check.ps1) | 调试器内嵌 C# 的秒级编译自检（不启动游戏） |
| [tools/cmp_dumps.py](../tools/cmp_dumps.py) | 批量比对 minidump |
| [tools/fxscan.py](../tools/fxscan.py) | 扫 fx 的 technique 数量 |
| [tools/deploy_render9fix.ps1](../tools/deploy_render9fix.ps1) | 安装/卸载 |
| [Render9Fix/test/](../Render9Fix/test/) | 端到端合成测试（假 Kernel + 假 dxrender9），一键 `build_and_run.ps1` |
| [dist/](../dist/) | 游戏侧工具存档：LAA 补丁器、单核启动、俄语布局清理（本体+资料片通用） |
