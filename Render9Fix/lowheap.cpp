// lowheap.cpp —— 2GB 以下的定长块小分配池
//
// 设计取舍：
//   * 单块预留（不被别的分配器占用），内部按需提交 —— 既不浪费物理内存，
//     也不需要维护多段区域的成员判断（判断归属变成一次范围比较）。
//   * 定长块 + 每档空闲链表：分配/释放 O(1)，无合并、无碎片管理复杂度。
//     代价是尺寸向上取整（最坏约 2 倍），但本池只接 dxrender9 的小对象
//     （默认 <= 64KB，实际主要是几百字节的 D3DX 结构），总量在个位数 MB。
//   * 尺寸档：前 128 档 16 字节步进（载荷 16..2048），后 128 档 512 字节步进
//     （2560..67584）。载荷都是 16 的倍数，块头 16 字节 ⇒ 用户指针 16 字节对齐。
#include "r9fix.h"

namespace lowheap {
namespace {

const uint32_t kMagic       = 0x52394658u;  // 'R9FX'
const int      kNumClasses  = 256;
const int      kStepClasses = 128;          // 前 128 档是 16 字节步进
const uint32_t kMaxPayload  = 67584;        // 最大档的载荷

#pragma pack(push, 1)
struct Head {
    uint32_t cls;
    uint32_t magic;
    uint64_t pad;
};
#pragma pack(pop)

const size_t kHeadSize = sizeof(Head);      // 16

CRITICAL_SECTION g_cs;

bool          g_ready     = false;
uint8_t*      g_base      = nullptr;
size_t        g_reserve   = 0;
volatile LONG g_committed = 0;              // 已提交字节数（单调增长，可无锁读）
size_t        g_bump      = 0;              // 分配游标（< g_committed）
void*         g_freeList[kNumClasses];

uint64_t g_allocCount = 0;
uint64_t g_freeCount  = 0;
uint64_t g_liveBytes  = 0;
uint64_t g_peakBytes  = 0;

uint32_t PayloadOf(int cls) {
    if (cls < kStepClasses) return 16u * (uint32_t)(cls + 1);
    return 2048u + 512u * (uint32_t)(cls - kStepClasses + 1);
}

uint32_t BlockTotalOf(int cls) {
    return PayloadOf(cls) + (uint32_t)kHeadSize;
}

// 尺寸 → 档号；超出最大档返回 -1
int ClassOf(size_t size) {
    if (size <= 16) return 0;
    if (size <= 2048) return (int)((size + 15) / 16) - 1;
    if (size <= kMaxPayload) return kStepClasses - 1 + (int)((size - 2048 + 511) / 512);
    return -1;
}

// 把已提交区扩到至少 want 字节（64KB 粒度）
bool EnsureCommitted(size_t want) {
    if (want <= (size_t)g_committed) return true;
    size_t newCommit = (want + 0xFFFFu) & ~(size_t)0xFFFFu;
    if (newCommit > g_reserve) newCommit = g_reserve;
    if (newCommit <= (size_t)g_committed) return false;

    size_t delta = newCommit - (size_t)g_committed;
    if (!VirtualAlloc(g_base + g_committed, delta, MEM_COMMIT, PAGE_READWRITE)) return false;
    g_committed = (LONG)newCommit;
    return true;
}

// 指针是否在「已提交」范围内（读块头之前必须先过这一关）
bool InCommitted(const void* p, size_t len) {
    if (!g_ready || !g_base) return false;
    uintptr_t lo = (uintptr_t)g_base;
    uintptr_t hi = lo + (size_t)g_committed;
    uintptr_t a  = (uintptr_t)p;
    return a >= lo && a + len <= hi;
}

}  // namespace

uintptr_t Base()  { return (uintptr_t)g_base; }
uintptr_t Limit() { return (uintptr_t)(g_base ? g_base + g_reserve : nullptr); }

bool Init(uint32_t preferredBase, uint32_t bytes) {
    if (g_ready) return true;
    if (bytes < 0x10000) bytes = 0x10000;

    InitializeCriticalSection(&g_cs);

    // 候选基址：从首选开始，失败依次往下试。全部要求 < 2GB。
    uint32_t cands[12];
    int nc = 0;
    cands[nc++] = preferredBase & ~0xFFFFu;
    const uint32_t fallback[] = {0x58000000u, 0x50000000u, 0x48000000u, 0x44000000u,
                                 0x40000000u, 0x38000000u, 0x30000000u, 0x28000000u,
                                 0x20000000u, 0x70000000u, 0x18000000u};
    for (int i = 0; i < (int)(sizeof(fallback) / sizeof(fallback[0])) && nc < 12; ++i) {
        if (fallback[i] == (preferredBase & ~0xFFFFu)) continue;
        cands[nc++] = fallback[i];
    }

    for (int i = 0; i < nc; ++i) {
        uint32_t b = cands[i];
        if (b >= 0x80000000u || b == 0) continue;
        void* p = VirtualAlloc((LPVOID)(uintptr_t)b, bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) continue;

        g_base      = (uint8_t*)p;
        g_reserve   = bytes;
        g_bump      = 0;
        g_committed = 0;
        for (int k = 0; k < kNumClasses; ++k) g_freeList[k] = nullptr;
        g_ready = true;

        Logf("低地址池: 基址=0x%08X 预留=%u 字节 (%.0f MB) —— 全部 < 2GB",
             (unsigned)(uintptr_t)p, bytes, bytes / 1048576.0);
        return true;
    }

    Logf("低地址池: 预留失败（候选基址全部被占）");
    return false;
}

void* Alloc(size_t size) {
    if (!g_ready) return nullptr;
    int cls = ClassOf(size);
    if (cls < 0) return nullptr;

    const size_t total = BlockTotalOf(cls);
    void* p = nullptr;

    EnterCriticalSection(&g_cs);
    p = g_freeList[cls];
    if (p) {
        g_freeList[cls] = *(void**)p;
    } else {
        if (!EnsureCommitted(g_bump + total)) {
            LeaveCriticalSection(&g_cs);
            return nullptr;  // 池满 —— 调用方回退到引擎分配器
        }
        p = g_base + g_bump;
        g_bump += total;
    }
    Head* h = (Head*)p;
    h->cls   = (uint32_t)cls;
    h->magic = kMagic;
    h->pad   = 0;

    ++g_allocCount;
    g_liveBytes += PayloadOf(cls);
    if (g_liveBytes > g_peakBytes) g_peakBytes = g_liveBytes;
    LeaveCriticalSection(&g_cs);

    return (uint8_t*)p + kHeadSize;
}

void Free(void* userPtr) {
    if (!userPtr || !g_ready) return;
    if (((uintptr_t)userPtr & 15u) != 0) {
        Logf("低地址池: Free 收到未对齐指针 %p —— 忽略（泄漏不破坏）", userPtr);
        return;
    }

    uint8_t* p = (uint8_t*)userPtr - kHeadSize;
    if (!InCommitted(p, kHeadSize)) {
        Logf("低地址池: Free 指针 %p 不在已提交区 —— 忽略", userPtr);
        return;
    }

    Head* h = (Head*)p;
    if (h->magic != kMagic) {
        Logf("低地址池: Free 头部魔数不符 @%p (magic=0x%08X cls=%u) —— 忽略（泄漏不破坏）",
             p, h->magic, h->cls);
        return;
    }
    int cls = (int)h->cls;
    if (cls < 0 || cls >= kNumClasses) {
        Logf("低地址池: Free 档号越界 %d @%p —— 忽略", cls, p);
        return;
    }
    h->magic = 0;  // 立刻失效，防止重复释放被当成有效块

    EnterCriticalSection(&g_cs);
    uint32_t payload = PayloadOf(cls);
    if (g_liveBytes >= payload) g_liveBytes -= payload;
    else g_liveBytes = 0;
    ++g_freeCount;
    *(void**)p = g_freeList[cls];
    g_freeList[cls] = p;
    LeaveCriticalSection(&g_cs);
}

void* Realloc(void* userPtr, size_t size) {
    if (!userPtr) return Alloc(size);
    if (size == 0) { Free(userPtr); return nullptr; }

    uint32_t oldCap = BlockSize(userPtr);
    if (oldCap == 0) return nullptr;     // 不是我们的块（调用方必须回退）
    if (size <= oldCap) return userPtr;  // 原地即可

    void* n = Alloc(size);
    if (!n) return nullptr;              // 池满（调用方回退）
    memcpy(n, userPtr, oldCap);
    Free(userPtr);
    return n;
}

bool Owns(const void* userPtr) {
    if (!g_ready || !userPtr) return false;
    uintptr_t a  = (uintptr_t)userPtr;
    // g_committed 单调增长，无锁读是安全的：任何存活的块一定早已在已提交区内。
    // 同时这也保证「引擎的指针绝不可能落在这个区间」—— 这块保留区是我们独占了。
    uintptr_t lo = (uintptr_t)g_base + kHeadSize;
    uintptr_t hi = (uintptr_t)g_base + (size_t)g_committed;
    return a >= lo && a < hi;
}

uint32_t BlockSize(const void* userPtr) {
    if (!Owns(userPtr)) return 0;
    const Head* h = (const Head*)((const uint8_t*)userPtr - kHeadSize);
    if (h->magic != kMagic) return 0;
    int cls = (int)h->cls;
    if (cls < 0 || cls >= kNumClasses) return 0;
    return PayloadOf(cls);
}

void GetStats(uint64_t* allocCount, uint64_t* liveBytes, uint64_t* peakBytes, uint32_t* reserveBytes,
              uint32_t* commitBytes) {
    if (allocCount)   *allocCount   = g_allocCount;
    if (liveBytes)    *liveBytes    = g_liveBytes;
    if (peakBytes)    *peakBytes    = g_peakBytes;
    if (reserveBytes) *reserveBytes = (uint32_t)g_reserve;
    if (commitBytes)  *commitBytes  = (uint32_t)g_committed;
}

bool SelfTest() {
    if (!g_ready) return false;

    const size_t sizes[] = {1, 16, 17, 100, 1024, 1025, 9686, 65536};
    const int    n       = (int)(sizeof(sizes) / sizeof(sizes[0]));
    void*        ptrs[8] = {0};
    bool         ok      = true;

    for (int i = 0; i < n; ++i) {
        ptrs[i] = Alloc(sizes[i]);
        if (!ptrs[i]) {
            Logf("低地址池自检: 尺寸 %u 分配失败", (unsigned)sizes[i]);
            ok = false;
            break;
        }
        if ((uintptr_t)ptrs[i] >= 0x80000000u) {
            Logf("低地址池自检: ★尺寸 %u 返回高地址 %p（不应发生）", (unsigned)sizes[i], ptrs[i]);
            ok = false;
        }
        if (((uintptr_t)ptrs[i] & 15u) != 0) {
            Logf("低地址池自检: 尺寸 %u 返回未对齐指针 %p", (unsigned)sizes[i], ptrs[i]);
            ok = false;
        }
        if (!Owns(ptrs[i])) {
            Logf("低地址池自检: 尺寸 %u 指针 %p 不被 Owns 认可", (unsigned)sizes[i], ptrs[i]);
            ok = false;
        }
        if (BlockSize(ptrs[i]) < sizes[i]) {
            Logf("低地址池自检: 尺寸 %u 报告容量 %u 不足", (unsigned)sizes[i], BlockSize(ptrs[i]));
            ok = false;
        }
        memset(ptrs[i], (int)(i + 1) & 0xFF, sizes[i]);
    }

    for (int i = 0; i < n; ++i) {
        if (ptrs[i]) Free(ptrs[i]);
    }

    // 复用检查（独立的一对分配，避免与上面的循环纠缠）
    void* a1 = Alloc(100);
    Free(a1);
    void* a2 = Alloc(100);
    bool  reuse = (a1 == a2);
    Free(a2);

    uint64_t ac = 0, live = 0, peak = 0;
    uint32_t res = 0, com = 0;
    GetStats(&ac, &live, &peak, &res, &com);
    Logf("低地址池自检: %s（分配 %llu 次, 释放复用 %s, 存活 %llu 字节, 峰值 %llu, 已提交 %u/%u 字节）",
         ok ? "通过" : "失败", (unsigned long long)ac, reuse ? "命中" : "未命中",
         (unsigned long long)live, (unsigned long long)peak, com, res);
    return ok;
}

}  // namespace lowheap
