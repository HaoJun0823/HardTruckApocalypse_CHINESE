// memhook.cpp —— 替换 Kernel 对象的三个分配器函数指针
//
// ★ 本文件是 DLC2 专用（目标进程 = emarcade.exe，Death Valley 资料片）★
//
// emarcade 的 Kernel 布局（IDA 实证：sub_935BE0 = Kernel::Kernel，含
// "0 == g_Kernel" 与 "W:\DeathValley\trunk\Core\Kernel.cpp" 两个字符串，
// 0x935BE0 处）：
//
//     +0x00  vtable      = off_B604B0
//     +0x08  MemoryManager（*(_DWORD *)(a1 + 8) = CriticalSection）
//     +0x30  AllocMem    = sub_932740   ← 换掉
//     +0x34  ReallocMem  = sub_932770   ← 换掉
//     +0x38  FreeMem     = sub_932760   ← 换掉
//
//   全局：g_Kernel = dword_C066C4（RVA 0x8066C4）
//
//   Kernel::Kernel 的原话（反编译 0x935c4d..0x935c5b）：
//       *(_DWORD *)(a1 + 48) = sub_932740;
//       *(_DWORD *)(a1 + 52) = sub_932770;
//       *(_DWORD *)(a1 + 56) = sub_932760;
//     ⇒ 与 Meridian113.exe 完全一致的 +0x30/+0x34/+0x38（两个资料片同源构建）。
//     三个包装函数再各转调一层：sub_932740→sub_9581C0（alloc，本体 0x5C0CE0→…
//     0x9581C0 里 GetCurrentThreadId + sub_957EA0 + sub_957F40 真正分配）、
//     sub_932770→sub_958230（realloc）、sub_932760→sub_9581E0（free）。
//
//   ★ 与 DLC1（Meridian113.exe）的唯一差别就是下面这四个地址值 ★
//     槽位偏移、块布局、魔数、ini 配置、探测与安装逻辑**全部不变**。
#include "r9fix_dlc2.h"

namespace memhook {
namespace {

// ---------------------------------------------------------------------------
// Kernel 布局常量（emarcade.exe 专用）
// ---------------------------------------------------------------------------
const uint32_t kKernelSlotRva     = 0x8066C4;   // dword_C066C4 = g_Kernel
const uint32_t kOffAllocMem       = 0x30;       // 本体 hta.exe 是 0x18
const uint32_t kOffReallocMem     = 0x34;       // 本体是 0x1C
const uint32_t kOffFreeMem        = 0x38;       // 本体是 0x20

const uint32_t kExpectAllocRva    = 0x532740;   // sub_932740（用于交叉校验）
const uint32_t kExpectReallocRva  = 0x532770;   // sub_932770
const uint32_t kExpectFreeRva     = 0x532760;   // sub_932760

// ---------------------------------------------------------------------------
// 引擎分配器函数类型
// ---------------------------------------------------------------------------
typedef void*(__fastcall* AllocFn)(uint32_t size, const char* src, int line);
typedef void*(__fastcall* ReallocFn)(void* p, uint32_t size, const char* src, int line);
typedef void (__fastcall* FreeFn)(void* p, const char* src, int line);

AllocFn   g_alloc   = nullptr;
ReallocFn g_realloc = nullptr;
FreeFn    g_free    = nullptr;

ModInfo   g_hta;
uint32_t  g_kernelAddr = 0;

volatile LONG g_installed = 0;

// 引擎块头/尾哨（emarcade 实测：用户指针 = 块 + 16，用户-4 = 0xDEADBEEF）
const uint32_t kEngineMagic   = 0xDEADBEEFu;
const uint32_t kEngineTrailer = 0xFEEBDAEDu;

// ---------------------------------------------------------------------------
// 统计
//
// ★ 为什么是 LONG（32 位）而不是 LONG64（2026-10-09 性能修正）★
//   HookAlloc 是**极热路径** —— emarcade 实测 5 秒内就有 500 万+ 次调用。
//   原来每次调用要两次 InterlockedIncrement64，而 x86 上 64 位原子加没有
//   单指令形式，编译器只能生成 `cmpxchg8b` 重试循环 —— 比 32 位的
//   `lock xadd` 贵一个数量级，还会抢总线、影响其它线程。
//
//   32 位 InterlockedIncrement 编译成单条 `lock xadd`，是 x86 上最便宜的
//   原子加。代价是计数到 21 亿会回绕 —— 但那只是日志里的诊断数字，
//   回绕不影响任何逻辑（导向/放行判断完全不依赖它）。
//
//   ★ 更关键的是低地址快路径现在**完全不做原子操作**（见 HookAlloc）★
//     绝大多数调用（实测 100% 在导向生效前）走的就是这条路。
// ---------------------------------------------------------------------------
volatile LONG g_hookCalls      = 0;   // Alloc 被调用次数
volatile LONG g_redirectCount  = 0;   // 因返回 >=2GB 而重导向的笔数
volatile LONG g_redirectBytes  = 0;
volatile LONG g_fallbackCount  = 0;   // 池满/尺寸档不足而回退
volatile LONG g_lowDirectCount = 0;   // 本来就 <2GB，直接放行
volatile LONG g_highLeftCount  = 0;   // 未导向且仍 >=2GB 的笔数（诊断关键）

volatile LONG g_lastReportedRedirect = -1;
volatile LONG g_lastReportedHigh     = -1;

// ---------------------------------------------------------------------------
// 探针：分配 64 字节 → 校验块头魔数/尾哨 → 释放
//
// emarcade 的块布局（sub_957F40 反编译实证，与 Meridian113 逐字节相同）：
//     user = block + 16
//     block[3]      = 0xDEADBEEF      （即 user-4）
//     *(user+size)  = 0xFEEBDAED      （尾哨）
//     block[1]      = size            （即 user-12）
// 本函数内不能有任何需要析构的局部对象，否则 __try 编译不过。
// ---------------------------------------------------------------------------
bool ProbeAllocPair() {
    bool ok = false;
    __try {
        void* p = g_alloc(64, "DLC2MFProbe", 0);
        if (!p) {
            Logf("探针: 分配返回 NULL —— 放弃安装");
        } else {
            const uint8_t* b = (const uint8_t*)p;
            uint32_t h0 = *(const uint32_t*)(b - 16);
            uint32_t h1 = *(const uint32_t*)(b - 12);
            uint32_t h2 = *(const uint32_t*)(b - 8);
            uint32_t h3 = *(const uint32_t*)(b - 4);
            uint32_t trailer = *(const uint32_t*)(b + 64);

            bool magicOk   = (h0 == kEngineMagic || h1 == kEngineMagic ||
                              h2 == kEngineMagic || h3 == kEngineMagic);
            bool sizeOk    = (h0 == 64 || h1 == 64 || h2 == 64 || h3 == 64);
            bool trailerOk = (trailer == kEngineTrailer);

            g_free(p, "DLC2MFProbe", 0);

            Logf("探针: 块头 = [0x%08X 0x%08X 0x%08X 0x%08X] 尾哨 = 0x%08X",
                 h0, h1, h2, h3, trailer);
            ok = magicOk && sizeOk && trailerOk;
            if (!ok) {
                Logf("探针: 校验失败 (magic=%d size=%d trailer=%d) —— 放弃安装",
                     (int)magicOk, (int)sizeOk, (int)trailerOk);
            } else {
                Logf("探针: 通过 —— Alloc/Free 配对确认");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("探针: 异常 0x%08X —— Kernel 定位错误，放弃安装", GetExceptionCode());
        ok = false;
    }
    return ok;
}

// 引擎块的实际容量（块+4 处的 size，用户指针 -12）
uint32_t EngineBlockSize(const void* p) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p;
    if (!ReadableCached(b - 12, 16)) return 0;
    return *(const uint32_t*)(b - 12);
}

// ---------------------------------------------------------------------------
// 挂钩实体
//
// 策略：只拦「原本会返回 >= 2GB 的分配」，把它们改到低地址池。
//   * 如果原分配返回 < 2GB → 原样返回（不改任何行为）
//   * 如果 >= 2GB 且 size 能进池 → 复制内容到低地址，**不释放原块**
//     （引擎的 Free 迟早会来，但那时它拿到的是我们的指针；原块交还由
//      HookFree 判断 —— 见下）
//
//   ★ 关于「原块怎么还」★
//     最安全的做法：原块**不还给引擎**（泄漏），因为我们没法保证引擎的 Free
//     会用同一个指针调用。代价是每次导向泄漏一份高地址内存。
//     实测导向笔数很小（只有 fx/effect 相关的小对象），可接受。
//     这与 Render9Fix 的做法一致（它也是先分配低地址块，不再碰原块）。
// ---------------------------------------------------------------------------
void* __fastcall HookAlloc(uint32_t size, const char* src, int line) {
    // ★★ 热路径性能（2026-10-09）★★
    //   本函数每秒被调用几十万次（emarcade 实测 5 秒 500 万+），所以：
    //     · 两个「纯统计」计数改用**非原子**自增 —— 它们只服务于日志，
    //       多线程并发下可能丢几笔，但绝不会损坏任何逻辑（对齐 32 位写，
    //       最坏是丢失更新，不会撕裂/崩溃）。
    //     · 只有**真正导向/遗留**这类稀有分支才用原子 —— 那里需要精确。
    //   这样「本就 <2GB」这条占绝对多数的路径上，一次原子操作都没有。
    ++g_hookCalls;

    void* p = g_alloc(size, src, line);
    if (!p) return nullptr;

    uintptr_t addr = (uintptr_t)p;
    if (addr < 0x80000000u) {
        ++g_lowDirectCount;              // 非原子：纯统计
        return p;
    }

    // >= 2GB：尝试导向
    if (size > 0 && size <= g_cfg.sizeMax) {
        void* low = lowheap::Alloc(size);
        if (low) {
            memcpy(low, p, size);
            // ★ 关键：把原块还回引擎分配器 ★（v1 故意泄漏，实测 646K 笔≈100MB，
            //   反过来把引擎内存顶到更高地址、池也更快耗尽。p 本来就是 g_alloc
            //   发的合法块，用配对的 g_free 释放是安全的。）
            g_free(p, src, line);
            LONG n = InterlockedIncrement(&g_redirectCount);
            InterlockedExchangeAdd(&g_redirectBytes, (LONG)size);
            if (g_cfg.logAlloc || n <= 20) {
                Logf("[导向] size=%u  %p -> %p", size, p, low);
            }
            return low;
        }
        InterlockedIncrement(&g_fallbackCount);
    }

    // 没接（太大 / 池满）：记一笔「高地址遗留」，原样返回
    LONG n = InterlockedIncrement(&g_highLeftCount);
    if (g_cfg.logAlloc || n <= 30) {
        Logf("[高地址遗留] size=%u addr=%p（未导向）", size, p);
    }
    return p;
}

void* __fastcall HookRealloc(void* p, uint32_t size, const char* src, int line) {
    if (lowheap::Owns(p)) {
        void* n = lowheap::Realloc(p, size);
        if (n) return n;
        if (size == 0) return nullptr;
        // 池满：用原分配器兜底
        void* e = g_alloc(size, src, line);
        if (e) {
            uint32_t old = lowheap::BlockSize(p);
            memcpy(e, p, old < size ? old : size);
            lowheap::Free(p);
        }
        InterlockedIncrement(&g_fallbackCount);
        return e;
    }
    return g_realloc(p, size, src, line);
}

void __fastcall HookFree(void* p, const char* src, int line) {
    // ★ 快路径优先：Owns() 只做两次无锁比较，比任何原子操作都便宜。
    //   绝大多数指针都不是我们的（导向还没发生时是 100%），
    //   所以先判归属再决定走哪条路，避免在热路径上碰锁。
    if (lowheap::Owns(p)) {
        lowheap::Free(p);
        return;
    }
    g_free(p, src, line);
}

// ---------------------------------------------------------------------------
// 定位 Kernel
//
// emarcade 的 g_Kernel 全局槽位是静态确定的（Kernel::Kernel 里 `dword_C066C4 = a1`），
// 所以直接读即可；再用三条硬校验确认身份：
//   1) 对象可读，vtable 落在 exe 镜像内且不在代码段
//   2) +0x30/+0x34/+0x38 三个指针都落在 exe 代码段
//   3) 三者与 IDA 期望的 RVA 完全一致（0x532740 / 0x532770 / 0x532760）
//     —— 这一条把「布局猜错」的可能性彻底排除。
// ---------------------------------------------------------------------------
bool ValidateKernelInner(uint32_t kv, char* why, size_t cap) {
    #define FAIL(msg) do { if (why) _snprintf_s(why, cap, _TRUNCATE, msg); return false; } while (0)

    if (kv < 0x10000u)   FAIL("值太小");
    if (kv & 3u)         FAIL("未 4 字节对齐");
    if (!ReadableCached((const void*)(uintptr_t)kv, 0x40)) FAIL("Kernel 对象不可读");

    uint32_t vtable = *(const uint32_t*)(uintptr_t)kv;
    if (!InRange(vtable, g_hta.base, g_hta.size)) FAIL("vtable 不在 exe 镜像内");
    if (AddrInExec(g_hta, vtable))                FAIL("vtable 落在代码段");

    uint32_t a = *(const uint32_t*)(uintptr_t)(kv + kOffAllocMem);
    uint32_t r = *(const uint32_t*)(uintptr_t)(kv + kOffReallocMem);
    uint32_t f = *(const uint32_t*)(uintptr_t)(kv + kOffFreeMem);
    if (!AddrInExec(g_hta, a) || !AddrInExec(g_hta, r) || !AddrInExec(g_hta, f))
        FAIL("三个函数指针不都在代码段");
    if (a == r || a == f || r == f) FAIL("三个函数地址有重复");

    uint32_t ea = (uint32_t)(g_hta.base + kExpectAllocRva);
    uint32_t er = (uint32_t)(g_hta.base + kExpectReallocRva);
    uint32_t ef = (uint32_t)(g_hta.base + kExpectFreeRva);
    if (a != ea || r != er || f != ef) FAIL("三个函数地址与 IDA 期望不符");

    #undef FAIL
    if (why) _snprintf_s(why, cap, _TRUNCATE, "ok");
    return true;
}

bool ValidateKernel(uint32_t kv, char* why, size_t cap) {
    bool ok = false;
    __try { ok = ValidateKernelInner(kv, why, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (why) _snprintf_s(why, cap, _TRUNCATE, "读取异常 0x%08X", GetExceptionCode());
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
bool InstallAt(uint32_t kv) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)kv, &mbi, sizeof(mbi))) {
        Logf("安装: VirtualQuery 失败");
        return false;
    }
    DWORD prot = mbi.Protect & 0xFF;
    bool writable = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                     prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
    if (!writable) {
        Logf("安装: Kernel 所在页不可写 (prot=0x%X) —— 放弃", prot);
        return false;
    }

    g_kernelAddr = kv;
    g_alloc   = *(AllocFn  *)(uintptr_t)(kv + kOffAllocMem);
    g_realloc = *(ReallocFn*)(uintptr_t)(kv + kOffReallocMem);
    g_free    = *(FreeFn   *)(uintptr_t)(kv + kOffFreeMem);

    // 二次安装防护（asi 被加载两份时会无限递归）
    if ((void*)g_alloc == (void*)&HookAlloc ||
        (void*)g_realloc == (void*)&HookRealloc ||
        (void*)g_free == (void*)&HookFree) {
        Logf("安装: 目标结构里已经是本插件的钩子 —— 跳过");
        return false;
    }

    Logf("安装: Kernel=0x%08X  AllocMem=0x%08X  ReallocMem=0x%08X  FreeMem=0x%08X",
         kv, (unsigned)(uintptr_t)g_alloc, (unsigned)(uintptr_t)g_realloc,
         (unsigned)(uintptr_t)g_free);

    if (!ProbeAllocPair()) return false;

    // 顺序：先 Realloc/Free，最后 Alloc。
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + kOffReallocMem), (PVOID)(uintptr_t)&HookRealloc);
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + kOffFreeMem),    (PVOID)(uintptr_t)&HookFree);
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + kOffAllocMem),   (PVOID)(uintptr_t)&HookAlloc);

    InterlockedExchange(&g_installed, 1);
    Logf("安装完成: 只导向「原本返回 >=2GB 且 <= %u 字节」的分配；其余原样放行", g_cfg.sizeMax);
    return true;
}

}  // namespace

// ===========================================================================
// 对外
// ===========================================================================
bool InstallWhenReady(uint32_t timeoutMs) {
    if (!g_cfg.enabled) {
        Logf("配置里 Enabled=0 —— 不安装");
        return false;
    }
    if (!GetModInfo(GetModuleHandleA(NULL), &g_hta)) {
        Logf("拿不到主模块信息 —— 放弃");
        return false;
    }
    Logf("宿主: base=0x%08X size=0x%X", (unsigned)g_hta.base, (unsigned)g_hta.size);

    DWORD t0 = GetTickCount();
    for (;;) {
        uint32_t* slot = (uint32_t*)(g_hta.base + kKernelSlotRva);
        uint32_t kv = 0;
        __try { kv = *slot; } __except (EXCEPTION_EXECUTE_HANDLER) { kv = 0; }

        if (kv) {
            char why[64];
            if (ValidateKernel(kv, why, sizeof(why))) {
                Logf("Kernel 定位: 槽位 0x%08X(RVA 0x%X) → Kernel=0x%08X  校验通过 ✓",
                     (unsigned)(uintptr_t)slot, kKernelSlotRva, kv);
                if (InstallAt(kv)) return true;
                Logf("安装失败 —— 不再重试");
                return false;
            } else {
                static bool once = false;
                if (!once) {
                    once = true;
                    Logf("槽位 0x%08X 里的值 0x%08X 未通过校验(%s) —— 继续等待",
                         (unsigned)(uintptr_t)slot, kv, why);
                }
            }
        }

        if (GetTickCount() - t0 > timeoutMs) break;
        Sleep(100);
    }

    Logf("等待超时（%u ms）仍未拿到有效 Kernel —— 不安装，游戏不受影响", timeoutMs);
    return false;
}

bool Installed() { return g_installed != 0; }

void LogSummary() {
    LONG rc = g_redirectCount;
    LONG hl = g_highLeftCount;
    if (rc == g_lastReportedRedirect && hl == g_lastReportedHigh) return;
    g_lastReportedRedirect = rc;
    g_lastReportedHigh     = hl;

    uint64_t ac = 0, live = 0, peak = 0;
    uint32_t res = 0, com = 0;
    lowheap::GetStats(&ac, &live, &peak, &res, &com);

    // 计数是 32 位（见上方统计变量注释）：热路径上不值得为日志付 64 位
    // 原子的代价。这里按 int 打印即可；真到 21 亿次就该重启看日志了。
    Logf("统计: Alloc 调用 %d | 导向 %d 笔 / %d 字节 | 本就<2GB %d | "
         "高地址遗留 %d 笔 | 回退 %d | 池: 存活 %llu 峰值 %llu 提交 %u/%u",
         (int)g_hookCalls, (int)rc, (int)g_redirectBytes,
         (int)g_lowDirectCount, (int)hl, (int)g_fallbackCount,
         (unsigned long long)live, (unsigned long long)peak, com, res);
}

}  // namespace memhook
