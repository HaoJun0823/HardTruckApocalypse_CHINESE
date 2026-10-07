# Render9Fix —— 把 dxrender9 的小对象分配导向 2GB 以下

一个独立的 ASI 插件，用来根治「hta.exe 开 LAA 后进关卡崩在 `dxrender9+0x416D6`」。
**不修改任何可执行文件，不挂钩任何代码，只替换 Kernel 结构里的 3 个函数指针。**

---

## 1. 问题与诊断链

### 崩溃现场（已确证的事实）

| 证据 | 内容 |
|---|---|
| 崩溃点 | `dxrender9+0x416D6` = `mov eax,[eax]`，`eax = [esi+0x9FC]`（technique 数组）为 NULL |
| 寄存器签名 | `EAX=0 EBX=0 ECX=8 EDX=FFFFFFFF EBP=1`（跨 4 个 dump 逐字节一致） |
| 内存占用 | 只有 ~125 MB（`mem used: 131,357,889`）——**不是 2GB 溢出** |
| 出事的 effect | `data/shaders/road.fx`（**只有 1 个 technique `Test1`**） |
| 线程 | 崩溃瞬间全进程 56 线程，只有 1 个在 dxrender9 内 —— **没有并发方** |

### 关键线索：日志里那串「乱码」

`exmachina.log` 最后一行：

```
EffectImpl() error: effect = 'data/shaders/road.fx',
could not validate technique '<乱码>', skipping it...
```

把那 29 字节原始字节取出来：

```
c3 6a 0d e8 a5 ff ff ff 59 c3 6a 0c e8 9c ff ff ff 59 c3 6a 10 68 c0 a3 37 7c e8 5d 01
```

这是一段**真实的 x86 机器码**（`ret; push 0xD; call -0x5B; pop ecx; ret; ...`，含绝对地址
`0x7C37A3C0`）。扫遍所有相关二进制，这段字节**精确命中 `msvcr71.dll+0x218F`** —— 也就是
`_free` 的 `retn` 指令。而 `0x7C34218A` / `0x7C34218F` 正以「栈残留」的形式出现在**每一个**
崩溃 dump 的栈上。

⇒ 结论：`techDesc` 结构体**根本没被写**，打印出的 `Name` 是**栈残留的旧返回地址**。
对应源码（`effect.cpp:434-437`，retruxx 1:1 还原）：

```cpp
D3DXHANDLE techHandle = m_effect->GetTechnique(i);
m_effect->GetTechniqueDesc(techHandle, &techDesc);      // ← HRESULT 没检查
if (m_effect->ValidateTechnique(techHandle) != D3D_OK)  // ← 无效句柄必然失败
    LogMsg("...could not validate technique '%s'...", techDesc.Name);
```

⇒ **`GetTechnique(0)` 返回了无效句柄** → 唯一 technique 被跳过 → 计数 0 →
走「没有默认 technique 就用第一个」的警告路径 → 取 `techArr[0]`（NULL）→ 崩。

### 为什么认定是「>= 2GB 的地址」问题

| 观察 | 解释 |
|---|---|
| 不开 LAA 一切正常 | 引擎内存池全部 < 2GB |
| 单核 + LAA + 无调试器**照样崩** | 与并发无关，是地址问题 |
| x32dbg 挂上去就能进游戏 | 调试器拖慢初始化 → 改变各线程分配交错 → 内存池布局变了 |
| 4ms 调试事件延迟能「治好」 | 同上：延迟不改变任何单线程计算结果，只改变**分配交错** ⇒ 布局变了 |
| 出事的 EffectImpl 地址 | `0x83BE8CF4` / `0x83BEE064` / `0x83BF228C` / `0x83C7FFE4` / `0x84C51C84` —— 全部 ≥ 2GB；另有一次是 `0x65AAF12C`（< 2GB） |

⇒ **dxrender9.dll 里静态链接的 D3DX（2006 年代码）在 ≥ 2GB 的地址上会出错** ——
典型的「指针被当成有符号 32 位整数参与运算」的老代码问题。与汉化无关。

---

## 2. 静态验证（用 IDA 反编译 hta.exe 的 `Kernel::Kernel`）

```
*a1          = &off_98F770              ; +0x00 vtable
dword_A0A88C = a1                       ; ← m3d::g_Kernel（RVA 0x60A88C）
a1[1]        = new MemoryManager(0x90)  ; +0x04 m_memMan
a1[6] = sub_589410                      ; +0x18 g_mar.AllocMem    (RVA 0x189410)
a1[7] = sub_589440                      ; +0x1C g_mar.ReallocMem  (RVA 0x189440)
a1[8] = sub_589430                      ; +0x20 g_mar.FreeMem     (RVA 0x189430)
```

三个包装分别转发到 `MemoryManager::Malloc(0x748DC0)` / `Realloc(0x748FA0)` / `Free(0x748EE0)`，
块布局为「12 字节头 + 数据 + 4 字节尾哨，用户指针 = block + 12」：

```c
*(p - 12) = -1;            // 独立块
*(p -  8) = size;          // m_size
*(p -  4) = 0xDEADBEEF;    // m_magic
*(p + size) = 0xFEEBDAED;  // 尾哨
```

**`Free` 在魔数/尾哨不符时执行 `__debugbreak()`（int3）** —— 这直接决定了本插件的
安装顺序必须「先 Free/Realloc，最后 Alloc」（见下）。

---

## 3. 本插件怎么做

```
1. 抢一段 2GB 以下的地址空间（默认预留 32MB，按需提交），建定长块小分配池 lowheap
2. 等 dxrender9.dll 出现、引擎 Kernel 对象就绪
3. 把 Kernel+0x18 / +0x1C / +0x20 的三个函数指针换成我们的
4. 只拦截「调用方在 dxrender9.dll 内」且「大小 <= SizeMax（默认 64KB）」的分配，
   交给低地址池；其余一律原样转交引擎分配器
```

⇒ dxrender9 的小对象（D3DX 的 effect 对象、technique 表、参数表、fx 源码缓冲）
全部落回 2GB 以下，恢复 2006 年 D3DX 能正确处理的环境；纹理/顶点缓冲等大块
照旧留在引擎池里，LAA 带来的 4GB 空间一点不浪费。

`data\shaders` 下最大的 fx 只有 **9686 字节**，所以 64KB 的阈值足够覆盖，
低地址池实际占用在个位数 MB。

### Kernel 定位（两道保险）

1. **已知布局快路径**：直接读 `m3d::g_Kernel`（RVA 0x60A88C）—— 上面静态确认过。
2. **通用扫描兜底**：扫描 hta.exe 与驱动 DLL 的可写段，找满足
   「`[K]` 是 hta 镜像内非代码段的 vtable」+「`[K+0x18/1C/20]` 是 hta 代码段里三个互不相同的指针」
   并且**同一个值同时出现在 hta 和驱动 DLL 数据里**的候选值。

选完再做**魔数探针**：实际分配 64 字节，校验块头与尾哨，然后释放。
探针不通过就**什么都不做**（游戏行为与安装前完全一致）。

### 安全设计

| 措施 | 解决什么 |
|---|---|
| Free/Realloc 按**地址**分派 | 池内的归我们，其他一概交还原函数 —— 换指针瞬间的并发分配/释放也不会串味 |
| **先装 Free/Realloc，最后装 Alloc** | 保证「我们发出的块」绝不会被原 `FreeMem` 看到（原 Free 读到我们的块头会 `__debugbreak`） |
| 池内块带 16 字节头 + 魔数 | Free 校验失败只泄漏、不破坏池结构 |
| 池满自动回退引擎分配器 | 绝不因池耗尽而让分配失败 |
| 命名互斥体单实例 | asi 被放了两份时，第二份直接退出（否则会把我们自己的钩子当「原函数」包一层 → 无限递归） |
| 二次安装防护 | 目标结构里已经是我们的钩子时跳过 |
| 不 patch 代码、不需要可执行内存 | 零代码风险；只改数据 |
| DLL 卸载时**故意不还原**指针 | 还原后引擎会用原 Free 释放我们已发出的块 → int3 崩溃 |

---

## 4. 测试

### 单元级（真实加载冒烟）

把 `Render9Fix.dll` 载入一个普通 32 位 PowerShell 进程：池预留成功、
自检通过（10 次分配/释放复用命中、地址全部 < 2GB）、等不到 dxrender9 时
2 秒超时后安全退出，进程无任何异常。

### 端到端合成测试（`Render9Fix\test\`，一键 `build_and_run.ps1`）

构造一个「假 Kernel + 假 dxrender9.dll」，把完整链路真跑一遍。结果：

```
扫描: hta 候选 1 / 驱动候选 1 / 合计 1 个值
  候选: 0x0069C000[hta+drv]
选中 0x0069C000（hta 与驱动 DLL 里同时出现）
安装: Kernel=0x0069C000  AllocMem=0x00681070  ReallocMem=0x00681150  FreeMem=0x006810D0
探针: 块头 = [0xFFFFFFFF 0x00000040 0xDEADBEEF] 尾哨 = 0xFEEBDAED
探针: 通过 —— 分配器配对确认
安装完成: 只导向 dxrender9.dll 且 <= 65536 字节的分配
```

假驱动通过 `kernel->g_mar.AllocMem` 分配的结果：

| 请求大小 | 返回地址 | 判定 |
|---|---|---|
| 16 / 64 / 220 / 1024 / 2200 / 9686 / 65536 | 全部 `0x6000xxxx`（池内，< 2GB） | ✅ 已导向 |
| 100000（> SizeMax） | `0x00D37B14` | ✅ 原样走引擎 |

释放 8 块无崩溃；`realloc(64 → 4096)` 内容保留且仍在池内；`realloc(4096 → 32)` 原地成功。
所有块的内容完整性校验通过。

---

## 5. 编译与部署

```powershell
# 编译（VS2017/v141 或 VS18 均可，需要 v141 工具集）
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  Render9Fix.vcxproj /p:Configuration=Release /p:Platform=Win32
# 产物：Render9Fix\build\Release\Render9Fix.dll

# 部署：把 dll 改名成 .asi，连同 ini 一起放进 update\（hta_chs.asi 就在那里）
Copy-Item build\Release\Render9Fix.dll 'I:\...\Hard Truck Apocalypse STEAM\update\Render9Fix.asi'
Copy-Item Render9Fix.ini            'I:\...\Hard Truck Apocalypse STEAM\update\'
```

也可以用 `tools\deploy_render9fix.ps1`（本机执行策略会拦 .ps1，需
`powershell -ExecutionPolicy Bypass -File tools\deploy_render9fix.ps1`）。

**卸载**：删掉游戏里的 `update\Render9Fix.asi` 与 `update\Render9Fix.ini` 即可
（或 `deploy_render9fix.ps1 -Revert`）。

---

## 6. 看日志判读

日志写在 asi 同目录：`update\Render9Fix.<时间戳>.log`。

| 日志内容 | 含义 |
|---|---|
| `已知布局命中: g_Kernel 槽位=0xA0A88C ... 完全一致 ✓` | 定位成功，且与静态确认的布局完全吻合 |
| `选中 0x........（hta 与驱动 DLL 里同时出现）` | 走的是通用扫描路径，也定位成功 |
| `探针: 通过` | 分配器配对确认，即将安装 |
| `安装完成: ...` | 挂钩生效 |
| `统计: 导向 N 笔 / M 字节 ... 高地址遗留 K 笔` | 正常工作中；**K 应该很小或为 0** |
| `高地址遗留` 条数很大 | dxrender9 还有较大的分配落在 ≥2GB —— 把 `SizeMax` 调大再试 |
| `等待超时` / `未通过校验` | 定位失败，插件没有做任何干预（附候选值与拒因统计，可直接发我） |

### 配置（`Render9Fix.ini`，可选）

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | 1 | 总开关 |
| `SizeMax` | 65536 | 只导向不超过这个大小的分配（最大 65536） |
| `OnlyDxRender9` | 1 | 只处理 dxrender9 发起的分配 |
| `ArenaMB` | 32 | 低地址池预留大小（只预留，按需提交） |
| `ArenaBase` | 60000000 | 首选基址，失败会自动下探 |
| `LogAlloc` | 0 | 1 = 记录每一笔（排查用） |
| `WaitSec` | 60 | 等 Kernel 出现的上限 |

---

## 7. 实机验证结果（2026-10-07 21:04，Steam 版 hta.exe）

**结论：原来 100% 崩的那一步这次走过去了，而且不是靠时序绕开 —— effect 真的验证通过了。**

插件日志 `update\Render9Fix.20261007_210419.log`：

```
已知布局命中: g_Kernel 槽位=0x00A0A88C(RVA 0x60A88C) → Kernel=0x00A0A890;
             Alloc/Realloc/Free = 0x00589410/0x00589440/0x00589430
             （期望 RVA 0x189410/0x189440/0x189430：完全一致 ✓）
探针: 块头 = [0x00000002 0x00000040 0xDEADBEEF] 尾哨 = 0xFEEBDAED  →  通过
安装完成: 只导向 dxrender9.dll 且 <= 65536 字节的分配
...
统计: 导向 254332 笔 / 161434383 字节 | 池: 存活 567200 峰值 1821472 提交 3997696/33554432
      | 高地址遗留 2 笔 / 262400 字节 | 回退 0
```

| 指标 | 实机结果 | 判读 |
|---|---|---|
| Kernel 定位 | 与静态逆向**完全一致** | 三个函数的 RVA 分毫不差 |
| 探针块头 | `[nextBlock=2, size=0x40, magic=0xDEADBEEF]` + 尾哨 | 分配器配对确认（class 2 = 64 字节池） |
| 导向量 | 41 秒内 254,332 笔 / 161 MB 累计 | 小对象分配极频繁，全部落回 <2GB |
| 低地址池占用 | 峰值 **1.82 MB**、提交 3.9 MB / 32 MB | 占用极小，对 2GB 以下空间几乎无影响 |
| 池耗尽回退 | **0 次** | 32MB 预留绰绰有余 |
| 高地址遗留 | 仅 **2 笔**，都是 131200 字节 | 见下 |

那 2 笔 131200 字节的遗留经 IDA 定位是 `sub_1002A400`（纹理加载，
含 `CreateTextureFromFileInMemoryEx error` 字符串）——**把一个 128KB 的 DDS 文件整体读入
的缓冲**，与 effect/technique 路径无关，且超过 64KB 阈值所以按设计留给引擎分配器。
因此 `SizeMax=65536` 不需要调整。

游戏日志 `exmachina.log` 同期内容：

```
-- Loading Level: data\maps\mainmenu.ssl --
World loaded in: 0        ← 原来就是崩在这一步之后
Roads loaded in: 0        ← road.fx 所在的道路资源
Engine inited in: 0       ← 主菜单初始化完成
```

并且整份日志里**没有任何** `could not validate technique` / `failed to load fx` ——
road.fx 的 technique 这次全部验证通过。**这正是根因被消除的直接证据**（若是靠时序侥幸，
日志里仍会出现验证失败的行）。

---

## 8. 已知边界

- 只在 32 位、`hta.exe`（Steam 版 1.03）上验证过静态布局；资料片 `Meridian113.exe`
  会自动走通用扫描路径（靠运行时校验，不依赖硬编码地址）。
- 依赖两个前提：驱动 DLL 确实**每次调用都从 Kernel 结构里现读**函数指针（已在
  retruxx 的 1:1 还原里确认），以及没有别的代码缓存了原始函数指针。若某个第三方
  插件把 `g_mar.FreeMem` 缓存下来去释放**我们池里**的块，会触发原 Free 的魔数断言 ——
  当前已知的 `hta_chs.asi` 不会（它缓存的是分配函数本身，且释放的指针都来自它自己
  通过该函数分配的内存，会被我们的按地址分派正确转交）。
- 插件的目的是让 dxrender9 回到「< 2GB 的习惯环境」，不改动引擎或汉化逻辑。
