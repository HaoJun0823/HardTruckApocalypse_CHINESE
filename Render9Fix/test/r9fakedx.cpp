// r9fakedx.cpp —— 端到端合成测试里的「假 dxrender9.dll」（输出名 /Fe:dxrender9.dll）
//
// 它模拟真实驱动 DLL 的两件事：
//   1. 自己的一份 kernel 指针副本（放在可写数据里）—— Render9Fix 靠
//      「同一个值同时出现在宿主与驱动 DLL 的可写数据里」来确认 Kernel 位置。
//   2. 通过 kernel->g_mar.AllocMem/ReallocMem/FreeMem（偏移 0x18/0x1C/0x20）
//      发起分配 —— 调用方在本模块内，因此应该被 Render9Fix 导向低地址。
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// 驱动自己的 kernel 指针副本（必须可写、必须在一个数据段里）
static void* volatile g_kernelCopy = nullptr;

typedef void* (__fastcall *AllocFn)(unsigned, const char*, int);
typedef void* (__fastcall *ReallocFn)(void*, unsigned, const char*, int);
typedef void  (__fastcall *FreeFn)(void*, const char*, int);

extern "C" __declspec(dllexport) void Prepare(void* kernel) {
    g_kernelCopy = kernel;
}

// 每次调用都从结构体里现读函数指针（真实驱动 DLL 就是这么做的）
static AllocFn   KAlloc()   { return *(AllocFn*)((unsigned char*)g_kernelCopy + 0x18); }
static ReallocFn KRealloc() { return *(ReallocFn*)((unsigned char*)g_kernelCopy + 0x1C); }
static FreeFn    KFree()    { return *(FreeFn*)((unsigned char*)g_kernelCopy + 0x20); }

static char g_report[8192];

static void Append(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(g_report);
    if (used < sizeof(g_report) - 2) {
        _vsnprintf_s(g_report + used, sizeof(g_report) - used, _TRUNCATE, fmt, ap);
    }
    va_end(ap);
}

extern "C" __declspec(dllexport) const char* RunTest() {
    g_report[0] = 0;
    if (!g_kernelCopy) { Append("假驱动没有 kernel 指针\n"); return g_report; }

    AllocFn   a = KAlloc();
    ReallocFn r = KRealloc();
    FreeFn    f = KFree();
    Append("Kernel=%p  Alloc=%p Realloc=%p Free=%p\n", g_kernelCopy, a, r, f);

    // 尺寸刻意覆盖：小对象（应被导向）、刚好在阈值内、超过阈值（应原样走引擎）
    const unsigned sizes[] = {16, 64, 220, 1024, 2200, 9686, 65536, 100000};
    const int      n       = (int)(sizeof(sizes) / sizeof(sizes[0]));
    void*          ptrs[16];

    Append("\n%-9s %-12s %-16s %s\n", "size", "ptr", "zone", "verdict");
    for (int i = 0; i < n; ++i) {
        unsigned sz = sizes[i];
        void* p = a(sz, nullptr, 0);
        ptrs[i] = p;
        if (!p) { Append("%-9u (NULL)\n", sz); continue; }

        memset(p, (int)(i + 1), sz);
        bool ok = true;
        unsigned char* b = (unsigned char*)p;
        for (unsigned k = 0; k < sz; ++k) {
            if (b[k] != (unsigned char)(i + 1)) { ok = false; break; }
        }

        unsigned addr = (unsigned)(uintptr_t)p;
        bool below2G = addr < 0x80000000u;
        bool inPool  = addr >= 0x60000000u && addr < 0x61000000u;   // 插件的低地址池
        const char* zone = inPool ? "pool(<2GB)" : (below2G ? "other-low" : "ABOVE-2GB!");
        const char* verdict;
        if (sz <= 65536) verdict = inPool ? "OK redirected" : "FAIL not-redirected";
        else             verdict = inPool ? "FAIL oversize-redirected" : "OK left-to-engine";

        Append("%-9u 0x%08X   %-16s %s%s\n", sz, addr, zone, verdict, ok ? "" : "  CONTENT-CORRUPTED");
    }

    Append("\n释放全部...\n");
    for (int i = 0; i < n; ++i) {
        if (ptrs[i]) f(ptrs[i], nullptr, 0);
    }
    Append("释放完成，没有崩溃\n");

    Append("\n重分配测试:\n");
    void* p1 = a(64, nullptr, 0);
    memset(p1, 0xAB, 64);
    void* p2 = r(p1, 4096, nullptr, 0);
    bool okCopy = true;
    for (int k = 0; k < 64; ++k) if (((unsigned char*)p2)[k] != 0xAB) { okCopy = false; break; }
    Append("  alloc(64)=0x%08X -> realloc(4096)=0x%08X  (content %s, %s)\n",
           (unsigned)(uintptr_t)p1, (unsigned)(uintptr_t)p2, okCopy ? "kept" : "LOST",
           ((unsigned)(uintptr_t)p2 >= 0x60000000u && (unsigned)(uintptr_t)p2 < 0x61000000u)
               ? "still-in-pool" : "moved-out-of-pool");

    void* p3 = r(p2, 32, nullptr, 0);
    Append("  realloc(4096 -> 32) = 0x%08X\n", (unsigned)(uintptr_t)p3);
    if (p3) f(p3, nullptr, 0);

    void* p4 = a(128, nullptr, 0);
    f(p4, nullptr, 0);
    Append("  再分配并释放一块: 0x%08X —— 完成\n", (unsigned)(uintptr_t)p4);

    Append("\n假驱动测试结束\n");
    return g_report;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
