// host.cpp —— Render9Fix 端到端合成测试的宿主
//
// 目的：在**不启动游戏**的前提下，把 Render9Fix 的完整链路真跑一遍：
//   1. 宿主里放一个「假 Kernel 对象」，布局与 hta.exe 的真实 Kernel 完全一致
//      （vtable@0、g_mar.AllocMem@0x18 / ReallocMem@0x1C / FreeMem@0x20），
//      并把它的地址存进一个全局指针（模拟 m3d::g_Kernel）。
//   2. 载入 Render9Fix.asi（= Render9Fix.dll）→ 插件进入轮询。
//   3. 载入一个名叫 dxrender9.dll 的假驱动，让它把同一个 kernel 指针存进自己的数据
//      （模拟驱动 DLL 里的 g_kernel 副本）→ 插件应当因此完成定位与安装。
//   4. 让假驱动通过 kernel->g_mar.AllocMem 分配一批不同尺寸的块。
//      ⇒ 被插件导向的块必须落在低地址池里（0x60xxxxxx，< 2GB）；
//      ⇒ 超过 SizeMax 的块必须原样走假引擎分配器（malloc 出来的低地址，但不是池）。
//   5. 释放/重分配全部走一遍，确认没有崩、没有串味。
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// 假引擎分配器：块头 12 字节 + 数据 + 4 字节尾哨，用户指针 = block + 12
// （与 IDA 里确认的 MemoryManager::Malloc(0x748DC0) / Free(0x748EE0) 一致）
// ---------------------------------------------------------------------------
static const int kMagic   = (int)0xDEADBEEF;
static const int kTrailer = (int)0xFEEBDAED;

static void* g_engineBlocks[4096];
static int   g_engineBlockCount = 0;

static void* __fastcall EngineAlloc(unsigned size, const char* src, int line) {
    if (size == 0) size = 1;
    unsigned total = 12 + size + 4;
    unsigned char* block = (unsigned char*)malloc(total);
    if (!block) return NULL;
    *(int*)(block + 0)  = -1;         // nextBlock = -1（独立块）
    *(int*)(block + 4)  = (int)size;  // m_size
    *(int*)(block + 8)  = kMagic;     // m_magic
    *(int*)(block + 12 + size) = kTrailer;
    if (g_engineBlockCount < 4096) g_engineBlocks[g_engineBlockCount++] = block;
    (void)src; (void)line;
    return block + 12;
}

static void __fastcall EngineFree(void* p, const char* src, int line) {
    if (!p) return;
    unsigned char* block = (unsigned char*)p - 12;
    if (*(int*)(block + 8) != kMagic) {
        printf("  [假引擎] !! 魔数不符 @%p —— 真实引擎在这里会 __debugbreak (int3)\n", p);
        return;
    }
    if (*(int*)(block + 12 + *(int*)(block + 4)) != kTrailer) {
        printf("  [假引擎] !! 尾哨不符 @%p\n", p);
        return;
    }
    for (int i = 0; i < g_engineBlockCount; ++i) {
        if (g_engineBlocks[i] == block) { g_engineBlocks[i] = NULL; break; }
    }
    free(block);
    (void)src; (void)line;
}

static void* __fastcall EngineRealloc(void* p, unsigned size, const char* src, int line) {
    if (!p) return EngineAlloc(size, src, line);
    if (!size) { EngineFree(p, src, line); return NULL; }
    unsigned oldSize = (unsigned)*(int*)((unsigned char*)p - 8);
    void* n = EngineAlloc(size, src, line);
    if (n) {
        memcpy(n, p, oldSize < size ? oldSize : size);
        EngineFree(p, src, line);
    }
    return n;
}

// ---------------------------------------------------------------------------
// 假 Kernel：布局必须与 hta.exe 逐字节一致
//   继承一个多态基类以拿到 .rdata 里的 vtable（vptr@0）；
//   成员顺序 ⇒ memMan@4, p8@8, p12@0xC, p16@0x10, p20@0x14, alloc@0x18, realloc@0x1C, free@0x20
// ---------------------------------------------------------------------------
struct VTableSource {
    virtual ~VTableSource() {}
    virtual void v0() {}
    virtual void v1() {}
};

struct FakeKernel : VTableSource {
    void* memMan;        // +0x04
    void* p8;            // +0x08
    void* p12;           // +0x0C
    void* p16;           // +0x10
    void* p20;           // +0x14
    void* alloc;         // +0x18  ← g_mar.AllocMem
    void* reallocMem;    // +0x1C  ← g_mar.ReallocMem
    void* freeMem;       // +0x20  ← g_mar.FreeMem
};

static FakeKernel g_kernel;

// 模拟 m3d::g_Kernel：全局指针，插件靠扫描这个值来定位
static void* volatile g_kernelPtr = nullptr;

extern "C" __declspec(dllexport) void* HostKernel() { return &g_kernel; }

int main() {
    printf("=== Render9Fix 端到端合成测试 ===\n");
    printf("宿主 exe: %p\n", (void*)GetModuleHandleA(NULL));

    g_kernel.memMan     = nullptr;
    g_kernel.p8 = g_kernel.p12 = g_kernel.p16 = g_kernel.p20 = nullptr;
    g_kernel.alloc      = (void*)&EngineAlloc;
    g_kernel.reallocMem = (void*)&EngineRealloc;
    g_kernel.freeMem    = (void*)&EngineFree;
    g_kernelPtr         = &g_kernel;

    printf("假 Kernel      = %p (vtable=%p)\n", (void*)&g_kernel, *(void**)&g_kernel);
    printf("  期望偏移: memMan@%u alloc@%u realloc@%u free@%u\n",
           (unsigned)offsetof(FakeKernel, memMan), (unsigned)offsetof(FakeKernel, alloc),
           (unsigned)offsetof(FakeKernel, reallocMem), (unsigned)offsetof(FakeKernel, freeMem));

    // 1) 先载入插件（它开始轮询等待 dxrender9.dll）
    HMODULE hFix = LoadLibraryA("Render9Fix.dll");
    printf("LoadLibrary(Render9Fix.dll) = %p (err=%lu)\n", (void*)hFix, GetLastError());
    if (!hFix) { printf("插件载入失败，测试中止\n"); return 1; }

    // 2) 载入假驱动 dxrender9.dll，并让它记下 kernel 指针
    HMODULE hDx = LoadLibraryA("dxrender9.dll");
    printf("LoadLibrary(dxrender9.dll)  = %p (err=%lu)\n", (void*)hDx, GetLastError());
    if (!hDx) { printf("假驱动载入失败，测试中止\n"); return 1; }

    typedef void (*PrepareFn)(void*);
    typedef const char* (*RunTestFn)(void);
    PrepareFn prepare = (PrepareFn)(void*)GetProcAddress(hDx, "Prepare");
    RunTestFn runTest = (RunTestFn)(void*)GetProcAddress(hDx, "RunTest");
    if (!prepare || !runTest) { printf("找不到假驱动导出\n"); return 1; }

    prepare(&g_kernel);
    printf("已把 kernel 指针写进假驱动 -> 等插件完成定位与安装...\n");
    Sleep(3000);

    printf("\n--- 假驱动开始分配 ---\n");
    const char* report = runTest();
    printf("%s", report);

    printf("\n--- 宿主自己的统计 ---\n");
    printf("假引擎仍持有的块数（应等于超出 SizeMax 的分配数）: %d\n", g_engineBlockCount);
    for (int i = 0; i < g_engineBlockCount; ++i) {
        if (g_engineBlocks[i]) printf("  未释放的假引擎块: %p\n", g_engineBlocks[i]);
    }
    printf("\n测试结束。\n");
    return 0;
}
