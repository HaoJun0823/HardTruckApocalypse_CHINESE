// memhook.cpp —— 定位 Kernel 对象并把 g_mar 的三个分配器函数指针换成我们的
//
// Kernel 布局（retruxx lib/include/core/kernel.h，已用 1:1 还原源码 + 运行时探针双重确认）：
//     +0x04  MemoryManager* m_memMan
//     +0x08  FileServer*
//     +0x0C  Timer*
//     +0x10  EngineConfig*
//     +0x14  ScriptServer*
//     +0x18  void* (__fastcall* AllocMem)  (unsigned int size, char const* src, int line)
//     +0x1C  void* (__fastcall* ReallocMem)(void* p, unsigned int size, char const*, int)
//     +0x20  void  (__fastcall* FreeMem)   (void* p, char const*, int)
//
// 怎样找到 Kernel？
//   不 patch 代码、不挂钩导出，而是**扫描模块可写段里存着的指针值**：
//   Kernel 对象必须同时满足
//     * [K+0x00] 是 vtable，落在 hta.exe 的只读数据段(.rdata)
//     * [K+0x18]/[K+0x1C]/[K+0x20] 是三个互不相同的、落在 hta.exe 可执行段的指针
//   这三条一起通过的概率极高、误判概率极低。再要求该值同时出现在
//   hta.exe 与 dxrender9.dll 的可写数据里（两边的 g_Kernel 副本），基本唯一确定。
//   最后用「魔数探针」实测一次分配/释放，确认定位无误才动手。
#include "r9fix.h"
#include <intrin.h>

namespace memhook {
namespace {

// ---------------------------------------------------------------------------
// 引擎分配器函数类型
// ---------------------------------------------------------------------------
typedef void*(__fastcall* AllocFn)(uint32_t size, const char* src, int line);
typedef void*(__fastcall* ReallocFn)(void* p, uint32_t size, const char* src, int line);
typedef void(__fastcall* FreeFn)(void* p, const char* src, int line);

AllocFn   g_alloc   = nullptr;
ReallocFn g_realloc = nullptr;
FreeFn    g_free    = nullptr;

uint32_t  g_kernelAddr = 0;
ModInfo   g_hta;
ModInfo   g_dx;

volatile LONG g_installed = 0;

// 引擎块头（12 字节）+ 4 字节尾哨，用于探针校验
const uint32_t kEngineMagic   = 0xDEADBEEFu;
const uint32_t kEngineTrailer = 0xFEEBDAEDu;

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
volatile LONG64 g_redirectCount = 0;
volatile LONG64 g_redirectBytes = 0;
volatile LONG64 g_fallbackCount = 0;   // 池满/无尺寸档而回退
volatile LONG64 g_highLeftCount = 0;   // dxrender9 发起、未导向、且落在 >=2GB 的笔数
volatile LONG64 g_highLeftBytes = 0;

volatile LONG64 g_lastReportedRedirect = -1;
volatile LONG64 g_lastReportedHigh     = -1;

// ---------------------------------------------------------------------------
// 候选值校验
// ---------------------------------------------------------------------------
// 拒因编码（用于诊断：为什么一个候选值没通过）
enum {
    R_TOO_SMALL = 0,
    R_UNALIGNED,
    R_UNREADABLE,
    R_VTABLE_NOT_IN_HTA,
    R_VTABLE_IN_CODE,
    R_FUNC_NOT_IN_CODE,
    R_FUNC_DUP,
    R_FAULT,
    R_REASON_COUNT
};

const char* ReasonName(int r) {
    switch (r) {
        case R_TOO_SMALL:        return "值太小";
        case R_UNALIGNED:        return "未对齐";
        case R_UNREADABLE:       return "对象不可读";
        case R_VTABLE_NOT_IN_HTA:return "vtable 不在 hta";
        case R_VTABLE_IN_CODE:   return "vtable 在代码段";
        case R_FUNC_NOT_IN_CODE: return "函数指针不在 hta 代码段";
        case R_FUNC_DUP:         return "三个函数地址重复";
        case R_FAULT:            return "读取异常";
        default:                 return "?";
    }
}

bool ValidateKernelInner(uint32_t v, char* why, size_t whyCap, int* reason) {
    #define FAIL(code, msg) do { if (why) _snprintf_s(why, whyCap, _TRUNCATE, msg); if (reason) *reason = code; return false; } while (0)

    if (v < 0x10000u)      FAIL(R_TOO_SMALL, "值太小");
    if (v & 3u)            FAIL(R_UNALIGNED, "未 4 字节对齐");
    if (!ReadableCached((const void*)(uintptr_t)v, 0x28)) FAIL(R_UNREADABLE, "Kernel 对象不可读");

    uint32_t vtable = *(const uint32_t*)(uintptr_t)v;
    // vtable 必须落在 hta 镜像里但不在代码段。
    // 正常情况它在 .rdata（只读数据）；这里不强制只读，避免极少数链接方式
    // 把 vtable 放进 .data 时直接失效 —— 后面还有「两边同时出现」+ 探针兜底。
    if (!InRange(vtable, g_hta.base, g_hta.size)) FAIL(R_VTABLE_NOT_IN_HTA, "vtable 不在 hta 镜像内");
    if (AddrInExec(g_hta, vtable))                FAIL(R_VTABLE_IN_CODE, "vtable 落在代码段");

    uint32_t a = *(const uint32_t*)(uintptr_t)(v + 0x18);
    uint32_t r = *(const uint32_t*)(uintptr_t)(v + 0x1C);
    uint32_t f = *(const uint32_t*)(uintptr_t)(v + 0x20);

    if (!AddrInExec(g_hta, a) || !AddrInExec(g_hta, r) || !AddrInExec(g_hta, f))
        FAIL(R_FUNC_NOT_IN_CODE, "三个函数指针不都在 hta 代码段");
    if (a == r || a == f || r == f) FAIL(R_FUNC_DUP, "三个函数地址有重复");

    #undef FAIL
    if (why) _snprintf_s(why, whyCap, _TRUNCATE, "ok");
    if (reason) *reason = -1;
    return true;
}

// 外层套 SEH：候选值的可读性判定来自启动时的索引快照，万一那页已经被释放，
// 读它会访问违例 —— 这里吞掉异常把它当成「不通过」，绝不让插件线程死掉。
bool ValidateKernel(uint32_t v, char* why, size_t whyCap, int* reason) {
    bool ok = false;
    __try {
        ok = ValidateKernelInner(v, why, whyCap, reason);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (why) _snprintf_s(why, whyCap, _TRUNCATE, "读取异常 0x%08X", GetExceptionCode());
        if (reason) *reason = R_FAULT;
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 扫描：把模块可写段里「看起来是 Kernel 指针」的值收集起来
// ---------------------------------------------------------------------------
struct Cand {
    uint32_t v;
    int      seenHta;    // 出现在 hta.exe 的可写数据里
    int      seenOther;  // 出现在驱动 DLL（dxrender9/sound/input）的可写数据里
};

void NoteCand(Cand* cands, int cap, int* count, uint32_t v, bool fromHta) {
    for (int i = 0; i < *count; ++i) {
        if (cands[i].v == v) {
            if (fromHta) cands[i].seenHta = 1;
            else         cands[i].seenOther = 1;
            return;
        }
    }
    if (*count >= cap) return;
    cands[*count].v         = v;
    cands[*count].seenHta   = fromHta ? 1 : 0;
    cands[*count].seenOther = fromHta ? 0 : 1;
    ++(*count);
}

// 遍历模块可写段的所有 dword，返回强校验通过的个数；reasons 里累计拒因
int ScanWritable(const ModInfo& m, Cand* cands, int cap, int* count, uint32_t* dwordsScanned,
                 int* reasons) {
    int valid = 0;
    *dwordsScanned = 0;
    if (!m.valid) return 0;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m.base;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)m.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    for (int s = 0; s < (int)nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        size_t vsize = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (vsize < 4) continue;
        const uint8_t* p = (const uint8_t*)(m.base + sec->VirtualAddress);

        // 按页扫：某一段里有未提交页时只跳过那一页，而不是整段放弃
        for (size_t off = 0; off + 4 <= vsize; off += 0x1000) {
            size_t chunk = vsize - off;
            if (chunk > 0x1000) chunk = 0x1000;
            if (!ReadableCached(p + off, chunk)) continue;

            const uint32_t* d = (const uint32_t*)(p + off);
            size_t nd = chunk / 4;
            for (size_t i = 0; i < nd; ++i) {
                uint32_t v = d[i];
                ++(*dwordsScanned);
                if (v < 0x10000u) { ++reasons[R_TOO_SMALL]; continue; }
                if (v & 3u)       { ++reasons[R_UNALIGNED]; continue; }
                int  reason = -1;
                char why[48];
                if (!ValidateKernel(v, why, sizeof(why), &reason)) {
                    if (reason >= 0 && reason < R_REASON_COUNT) ++reasons[reason];
                    continue;
                }
                ++valid;
                NoteCand(cands, cap, count, v, m.base == g_hta.base);
            }
        }
    }
    return valid;
}

// 把拒因直方图拼成一行（用于诊断「为什么一个都没通过」）
void FormatReasons(const int* reasons, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < R_REASON_COUNT; ++i) {
        if (!reasons[i]) continue;
        char one[64];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s=%d ", ReasonName(i), reasons[i]);
        strncat_s(out, cap, one, _TRUNCATE);
    }
    if (!out[0]) _snprintf_s(out, cap, _TRUNCATE, "(无)");
}

// ---------------------------------------------------------------------------
// 探针：用定位到的分配器实测一次「分配 64 字节 → 检查块头魔数/尾哨 → 释放」
//
// 这是动手改指针之前的最后一道闸。读偏移不写死（3 个 dword 里任意一个是
// 0xDEADBEEF、任意一个是 64、尾哨正确即认为配对无误）。
// 本函数内不能有任何需要析构的局部对象，否则 __try 编译不过。
// ---------------------------------------------------------------------------
bool ProbeAllocPair() {
    bool ok = false;
    __try {
        void* p = g_alloc(64, "R9FixProbe", 0);
        if (!p) {
            Logf("探针: 分配返回 NULL —— 放弃安装");
        } else {
            const uint8_t* b = (const uint8_t*)p;
            uint32_t h0 = *(const uint32_t*)(b - 12);
            uint32_t h1 = *(const uint32_t*)(b - 8);
            uint32_t h2 = *(const uint32_t*)(b - 4);
            uint32_t trailer = *(const uint32_t*)(b + 64);

            bool magicOk   = (h0 == kEngineMagic || h1 == kEngineMagic || h2 == kEngineMagic);
            bool sizeOk    = (h0 == 64 || h1 == 64 || h2 == 64);
            bool trailerOk = (trailer == kEngineTrailer);

            g_free(p, "R9FixProbe", 0);

            Logf("探针: 块头 = [0x%08X 0x%08X 0x%08X] 尾哨 = 0x%08X", h0, h1, h2, trailer);
            ok = magicOk && sizeOk && trailerOk;
            if (!ok) {
                Logf("探针: 校验失败 (magic=%d size=%d trailer=%d) —— 放弃安装",
                     (int)magicOk, (int)sizeOk, (int)trailerOk);
            } else {
                Logf("探针: 通过 —— 分配器配对确认");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("探针: 异常 0x%08X —— Kernel 定位错误，放弃安装", GetExceptionCode());
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 分派策略
// ---------------------------------------------------------------------------
bool IsDxCaller(const void* ra) {
    if (!g_dx.valid) return false;
    return InRange((uintptr_t)ra, g_dx.base, g_dx.size);
}

bool ShouldRedirect(const void* ra, uint32_t size) {
    if (!g_cfg.enabled) return false;
    if (size > g_cfg.sizeMax) return false;
    if (g_cfg.onlyDxRender9 && !IsDxCaller(ra)) return false;
    return true;
}

// 引擎块的实际容量（块头 +4 处的 m_size）
uint32_t EngineBlockSize(const void* p) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p;
    if (!IsReadable(b - 12, 16)) return 0;
    return *(const uint32_t*)(b - 8);
}

// ---------------------------------------------------------------------------
// 挂钩实体
// ---------------------------------------------------------------------------
void* __fastcall HookAlloc(uint32_t size, const char* src, int line) {
    const void* ra = _ReturnAddress();

    if (ShouldRedirect(ra, size)) {
        void* p = lowheap::Alloc(size);
        if (p) {
            LONG64 n = InterlockedIncrement64(&g_redirectCount);
            InterlockedExchangeAdd64(&g_redirectBytes, (LONG64)size);
            if (g_cfg.logAlloc || n <= 20) {
                char who[64];
                FormatCaller(ra, who, sizeof(who));
                Logf("[导向] caller=%s size=%u -> %p", who, size, p);
            }
            return p;
        }
        InterlockedIncrement64(&g_fallbackCount);
    }

    void* p = g_alloc(size, src, line);

    // 诊断：dxrender9 发起、我们没接（太大或池满）且仍落在 >=2GB 的分配
    if (p && (uintptr_t)p >= 0x80000000u) {
        bool interesting = g_cfg.onlyDxRender9 ? IsDxCaller(ra) : true;
        if (interesting) {
            LONG64 n = InterlockedIncrement64(&g_highLeftCount);
            InterlockedExchangeAdd64(&g_highLeftBytes, (LONG64)size);
            if (g_cfg.logAlloc || n <= 30) {
                char who[64];
                FormatCaller(ra, who, sizeof(who));
                Logf("[高地址遗留] caller=%s size=%u addr=%p", who, size, p);
            }
        }
    }
    return p;
}

void* __fastcall HookRealloc(void* p, uint32_t size, const char* src, int line) {
    const void* ra = _ReturnAddress();

    if (lowheap::Owns(p)) {
        void* n = lowheap::Realloc(p, size);
        if (n) return n;
        if (size == 0) return nullptr;
        // 池满：用引擎分配器兜底，把内容搬过去再释放我们的块
        void* e = g_realloc(nullptr, size, src, line);
        if (e) {
            uint32_t old = lowheap::BlockSize(p);
            memcpy(e, p, old < size ? old : size);
            lowheap::Free(p);
        }
        InterlockedIncrement64(&g_fallbackCount);
        return e;
    }

    if (p && ShouldRedirect(ra, size)) {
        void* n = lowheap::Alloc(size);
        if (n) {
            uint32_t old = EngineBlockSize(p);
            memcpy(n, p, old < size ? old : size);
            g_free(p, src, line);
            return n;
        }
        InterlockedIncrement64(&g_fallbackCount);
    }

    return g_realloc(p, size, src, line);
}

void __fastcall HookFree(void* p, const char* src, int line) {
    if (lowheap::Owns(p)) {
        lowheap::Free(p);
        return;
    }
    g_free(p, src, line);
}

// ---------------------------------------------------------------------------
// 已知布局快路径（静态逆向确认过，见下）
//
// 对 hta.exe 1.03 (Steam) 用 IDA 反编译 Kernel::Kernel 得到的确凿结果：
//     *a1        = &off_98F770            ; +0x00 vtable
//     dword_A0A88C = a1                   ; ← m3d::g_Kernel（全局指针，RVA 0x60A88C）
//     a1[1]      = new MemoryManager(0x90);  ; +0x04 m_memMan
//     a1[6] = sub_589410;                 ; +0x18 g_mar.AllocMem    (RVA 0x189410)
//     a1[7] = sub_589440;                 ; +0x1C g_mar.ReallocMem  (RVA 0x189440)
//     a1[8] = sub_589430;                 ; +0x20 g_mar.FreeMem     (RVA 0x189430)
// 三个包装分别转发到 MemoryManager::Malloc(0x748DC0) / Realloc(0x748FA0) / Free(0x748EE0)。
//
// 所以直接读那个全局槽位即可，无需扫描；校验不通过（例如资料片
// Meridian113.exe 的不同布局）就退回通用扫描。
// ---------------------------------------------------------------------------
const uint32_t kKnownKernelSlotRva = 0x60A88C;
const uint32_t kKnownAllocRva      = 0x189410;
const uint32_t kKnownReallocRva    = 0x189440;
const uint32_t kKnownFreeRva       = 0x189430;

// 成功返回 true 并写出 Kernel 地址
bool TryKnownLayout(uint32_t* outKernel) {
    uint32_t slot = (uint32_t)(g_hta.base + kKnownKernelSlotRva);
    if (!ReadableCached((const void*)(uintptr_t)slot, 4)) return false;

    uint32_t kv = 0;
    __try {
        kv = *(const uint32_t*)(uintptr_t)slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!kv) return false;

    char why[48];
    if (!ValidateKernel(kv, why, sizeof(why), nullptr)) {
        // 静默退回扫描（第一次时打一行，便于排查）
        static bool once = false;
        if (!once) { once = true; Logf("已知槽位 0x%08X 里的值 0x%08X 未通过校验(%s) —— 改用通用扫描", slot, kv, why); }
        return false;
    }

    uint32_t a = *(const uint32_t*)(uintptr_t)(kv + 0x18);
    uint32_t r = *(const uint32_t*)(uintptr_t)(kv + 0x1C);
    uint32_t f = *(const uint32_t*)(uintptr_t)(kv + 0x20);
    uint32_t ea = (uint32_t)(g_hta.base + kKnownAllocRva);
    uint32_t er = (uint32_t)(g_hta.base + kKnownReallocRva);
    uint32_t ef = (uint32_t)(g_hta.base + kKnownFreeRva);

    Logf("已知布局命中: g_Kernel 槽位=0x%08X(RVA 0x%X) → Kernel=0x%08X；"
         "Alloc/Realloc/Free = 0x%08X/0x%08X/0x%08X（期望 RVA 0x189410/0x189440/0x189430：%s）",
         slot, kKnownKernelSlotRva, kv, a, r, f,
         (a == ea && r == er && f == ef) ? "完全一致 ✓" : "不一致（仍按校验结果采用）");

    *outKernel = kv;
    return true;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
bool InstallAt(uint32_t kv) {
    // 目标结构必须可写
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
    g_alloc      = *(AllocFn*)(uintptr_t)(kv + 0x18);
    g_realloc    = *(ReallocFn*)(uintptr_t)(kv + 0x1C);
    g_free       = *(FreeFn*)(uintptr_t)(kv + 0x20);

    // 二次安装防护：如果这三个指针里已经有我们的钩子，说明本进程已经装过
    // （例如 asi 被加载了两份）。此时再装一次会让「原函数」指向我们自己 → 无限递归。
    if ((void*)g_alloc == (void*)&HookAlloc || (void*)g_realloc == (void*)&HookRealloc ||
        (void*)g_free == (void*)&HookFree) {
        Logf("安装: 目标结构里已经是本插件的钩子（说明已安装过或 asi 被加载了两份）—— 跳过");
        return false;
    }

    Logf("安装: Kernel=0x%08X  AllocMem=0x%08X  ReallocMem=0x%08X  FreeMem=0x%08X",
         kv, (unsigned)(uintptr_t)g_alloc, (unsigned)(uintptr_t)g_realloc, (unsigned)(uintptr_t)g_free);

    if (!ProbeAllocPair()) return false;

    // 顺序很重要：先 Realloc/Free，最后 Alloc。
    // 这样「我们分配的块」绝不会被原 FreeMem 看到（原 Free 读到我们的块头会
    // 判魔数不符并 __debugbreak）。反之，引擎的块被我们的 Free 看到是安全的
    // —— HookFree 会按地址判断后原样转交。
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + 0x1C), (PVOID)(uintptr_t)&HookRealloc);
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + 0x20), (PVOID)(uintptr_t)&HookFree);
    InterlockedExchangePointer((PVOID volatile*)(uintptr_t)(kv + 0x18), (PVOID)(uintptr_t)&HookAlloc);

    InterlockedExchange(&g_installed, 1);
    Logf("安装完成: 只导向 dxrender9%s 且 <= %u 字节的分配；其余全部原样走引擎分配器",
         g_cfg.onlyDxRender9 ? ".dll" : "／全部模块", g_cfg.sizeMax);
    return true;
}

}  // namespace

// ===========================================================================
// 对外：轮询等待并安装
// ===========================================================================
bool InstallWhenReady(uint32_t timeoutMs) {
    if (!g_cfg.enabled) {
        Logf("配置里 Enabled=0 —— 不安装");
        return false;
    }
    if (!GetModInfo(GetModuleHandleA(NULL), &g_hta)) {
        Logf("拿不到 hta.exe 模块信息 —— 放弃");
        return false;
    }
    Logf("宿主: base=0x%08X size=0x%X", (unsigned)g_hta.base, (unsigned)g_hta.size);

    BuildReadableMap();

    DWORD t0 = GetTickCount();
    bool  loggedFirst = false;
    int   lastTotal = -1;

    // 驱动 DLL 里也有 Kernel 指针的副本：先只用 dxrender9（最关键的），
    // 若干秒还没收敛就把 sound/input 也拉进来投票。
    const char* kExtraMods[] = {"sound.dll", "input_di8.dll"};
    ModInfo extra[2];
    bool    extraLoaded[2] = {false, false};

    for (;;) {
        if (!g_dx.valid) GetModInfoByName("dxrender9.dll", &g_dx);

        DWORD elapsed = GetTickCount() - t0;
        bool  wide    = elapsed > 3000;

        // 先试已知布局（静态确认过的确定位置）；失败再走通用扫描
        if (g_dx.valid) {
            uint32_t kv = 0;
            if (TryKnownLayout(&kv)) {
                if (InstallAt(kv)) return true;
                Logf("已知布局的 Kernel 安装失败 —— 不再重试");
                return false;
            }
        }

        Cand cands[64];
        int  count = 0;
        int  validHta = 0, validOther = 0;
        uint32_t dwordsHta = 0, dwordsOther = 0;
        int  reasons[R_REASON_COUNT];
        for (int i = 0; i < R_REASON_COUNT; ++i) reasons[i] = 0;

        if (g_dx.valid) {
            validHta = ScanWritable(g_hta, cands, 64, &count, &dwordsHta, reasons);
            validOther += ScanWritable(g_dx, cands, 64, &count, &dwordsOther, reasons);
        }
        if (wide) {
            for (int i = 0; i < 2; ++i) {
                if (!extraLoaded[i]) {
                    extraLoaded[i] = GetModInfoByName(kExtraMods[i], &extra[i]);
                    if (extraLoaded[i]) Logf("补扫: %s base=0x%08X", kExtraMods[i], (unsigned)extra[i].base);
                }
                if (extraLoaded[i]) {
                    uint32_t dw = 0;
                    validOther += ScanWritable(extra[i], cands, 64, &count, &dw, reasons);
                }
            }
        }

        if (count > 0 || (g_dx.valid && !loggedFirst)) {
            char detail[512];
            detail[0] = 0;
            size_t used = 0;
            for (int i = 0; i < count && i < 8; ++i) {
                char one[64];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "0x%08X[%s%s] ", cands[i].v,
                            cands[i].seenHta ? "hta" : "", cands[i].seenOther ? "+drv" : "");
                strncat_s(detail, sizeof(detail), one, _TRUNCATE);
                used += strlen(one);
                if (used > 400) break;
            }
            if (count != lastTotal) {
                Logf("扫描: hta 候选 %d / 驱动候选 %d / 合计 %d 个值%s；dwords hta=%u other=%u",
                     validHta, validOther, count, count ? "" : "（还没有 kernel 指针）",
                     dwordsHta, dwordsOther);
                if (count) {
                    Logf("  候选: %s", detail);
                } else {
                    char rs[512];
                    FormatReasons(reasons, rs, sizeof(rs));
                    Logf("  拒因: %s", rs);
                }
                lastTotal = count;
            }
            loggedFirst = true;
        }

        if (count > 0) {
            // 优先选「hta 与驱动 DLL 里都出现」的值；否则退而求其次取唯一候选
            int best = -1;
            int bothCount = 0, singleCount = 0, bestSingle = -1;
            for (int i = 0; i < count; ++i) {
                if (cands[i].seenHta && cands[i].seenOther) { ++bothCount; best = i; }
                else { ++singleCount; bestSingle = i; }
            }
            if (bothCount == 1) {
                Logf("选中 0x%08X（hta 与驱动 DLL 里同时出现）", cands[best].v);
                if (InstallAt(cands[best].v)) return true;
                Logf("安装失败（候选 0x%08X）—— 不再重试", cands[best].v);
                return false;
            }
            if (bothCount == 0 && singleCount == 1 && wide) {
                Logf("注意: 只有一个候选 0x%08X（没在两边同时出现），仍然尝试", cands[bestSingle].v);
                if (InstallAt(cands[bestSingle].v)) return true;
                return false;
            }
            // 其它情况（多个候选、或还没到宽扫阶段）继续等，下一轮可能就收敛
        }

        if (elapsed > timeoutMs) break;
        Sleep(100);
    }

    Logf("等待超时（%u ms）仍未确定唯一的 Kernel 指针 —— 不安装，游戏不受影响", timeoutMs);
    return false;
}

bool Installed() { return g_installed != 0; }

bool FormatCaller(const void* addr, char* out, size_t cap) {
    uintptr_t a = (uintptr_t)addr;
    if (g_dx.valid && InRange(a, g_dx.base, g_dx.size)) {
        _snprintf_s(out, cap, _TRUNCATE, "dxrender9+0x%X", (unsigned)(a - g_dx.base));
        return true;
    }
    if (g_hta.valid && InRange(a, g_hta.base, g_hta.size)) {
        _snprintf_s(out, cap, _TRUNCATE, "hta+0x%X", (unsigned)(a - g_hta.base));
        return true;
    }
    _snprintf_s(out, cap, _TRUNCATE, "0x%08X", (unsigned)a);
    return false;
}

void LogSummary() {
    LONG64 rc = g_redirectCount;
    LONG64 hl = g_highLeftCount;
    if (rc == g_lastReportedRedirect && hl == g_lastReportedHigh) return;
    g_lastReportedRedirect = rc;
    g_lastReportedHigh     = hl;

    uint64_t ac = 0, live = 0, peak = 0;
    uint32_t res = 0, com = 0;
    lowheap::GetStats(&ac, &live, &peak, &res, &com);

    Logf("统计: 导向 %lld 笔 / %lld 字节 | 池: 存活 %llu 峰值 %llu 提交 %u/%u | "
         "高地址遗留 %lld 笔 / %lld 字节 | 回退 %lld",
         (long long)rc, (long long)g_redirectBytes,
         (unsigned long long)live, (unsigned long long)peak, com, res,
         (long long)hl, (long long)g_highLeftBytes, (long long)g_fallbackCount);
}

}  // namespace memhook
