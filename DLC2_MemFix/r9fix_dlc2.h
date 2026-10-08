// r9fix_dlc2.h —— DLC2_MemFix：把引擎分配器返回的「高地址小对象」导向 2GB 以下
//
// ★ 目标进程：emarcade.exe（HARD TRUCK APOCALYPSE ARCADE，Death Valley 资料片）
//   本文件与 DLC2_MemFix 的其它部分**只**服务 emarcade.exe。
//   DLC1_MemFix（Meridian113.exe）那份是同源但独立的工程，地址值不同。
//
// ============================ 为什么需要它 ============================
//
// ★ 本插件修的问题目前只有「工作假设」，还没有 emarcade 上的实机崩溃转储 ★
//
//   假设：emarcade.exe 打上 LAA（4GB 补丁）后，引擎内存池会整体抬到 >= 2GB。
//   而 emarcade 把渲染器（含 2006 年代的 D3DX）**静态链接进 exe**，那段老代码里
//   有「把指针当 32 位有符号整数参与运算」的地方，地址进 2GB 就会出错。
//
//   这个签名的来源是 DLC1（Meridian113.exe）那边**已取证**的崩溃：
//     Exception: 0xC0000005 at 0x006FA2EA,  ESI = 0  →  read [0]
//     引擎日志最后一行：
//       Kernel.cpp[0317] EffectImpl: failed to load fx file
//                        'data\shaders\road.fx' with: No D3DX error message.
//   本体 hta.exe 的 LAA 崩溃也是同一个签名（road.fx 的 technique 校验失败
//   → technique 表为空 → 取 techArr[0] 解引用），被 Render9Fix 证实并修好。
//
//   ★ emarcade 的现状：PE Characteristics = 0x10E，**没有 LAA 位**（113 是 0x12E）。
//     所以本插件在未打 4GB 补丁时，`addr < 0x80000000` 的判断恒不成立，等于空转、
//     游戏不受影响 —— 这是设计上的「无害空转」，不是故障。
//     打上 LAA 后本插件才开始起作用；届时请用日志里的「导向 / 高地址遗留」
//     两个计数来验证或推翻这个假设。
//
// ============================ 本插件怎么做 ============================
//
// emarcade 的 Kernel 对象布局（IDA 实证：sub_935BE0 = Kernel::Kernel，0x935BE0，
// 含 "0 == g_Kernel" 与 "W:\DeathValley\trunk\Core\Kernel.cpp" 字符串）：
//
//     +0x00  vtable      = off_B604B0
//     +0x08  MemoryManager
//     +0x30  AllocMem    = sub_932740   ← 换掉
//     +0x34  ReallocMem  = sub_932770   ← 换掉
//     +0x38  FreeMem     = sub_932760   ← 换掉
//
//   全局：dword_C066C4 = g_Kernel（RVA 0x8066C4），指向静态 Kernel 对象。
//
//   ⇒ 和 Render9Fix 一样：**不 patch 任何代码、不挂钩任何指令**，只替换这 3 个
//     数据指针。原函数指针先存下来，钩子里直接调用。
//
// ============================ 安全设计 ============================
//
//   * 安装前用「块头魔数探针」实测一次分配/释放（emarcade 的块布局：用户指针 = 块+16，
//     用户-4 = 0xDEADBEEF，用户+size = 0xFEEBDAED）。探针失败就什么都不做。
//   * 只导向「返回地址 >= 2GB 且 size 可放进低地址池」的分配；其余原样返回。
//   * Free/Realloc 按**地址**分派：落在我们保留区的归我们，其他交还原函数
//     —— 即使换指针的瞬间发生并发分配/释放也不会串味。
//   * 换指针顺序：先 Realloc/Free，最后 Alloc。
//   * 低地址池用尽时自动回退到原分配器（游戏照常运行，只是不再导向）。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

// 本插件只服务于 32 位目标进程
#if defined(_WIN64)
#error "DLC2_MemFix 必须编译为 Win32 (x86)：emarcade.exe 是 32 位进程"
#endif

#define DLC2MF_NAME "DLC2_MemFix"

// ===========================================================================
// 日志
// ===========================================================================
void LogOpen(HMODULE self);
void LogClose();
void Logf(const char* fmt, ...);

// ===========================================================================
// 模块 / 内存查询
// ===========================================================================
struct ModInfo {
    uintptr_t base;
    size_t    size;      // SizeOfImage
    bool      valid;
};

bool GetModInfo(HMODULE mod, ModInfo* out);
bool GetModInfoByName(const char* name, ModInfo* out);
bool InRange(uintptr_t a, uintptr_t base, size_t size);
bool AddrInExec  (const ModInfo& m, uintptr_t a);
bool AddrInRoData(const ModInfo& m, uintptr_t a);
bool AddrInWritable(const ModInfo& m, uintptr_t a);
bool IsReadable(const void* p, size_t len);
void BuildReadableMap();
bool ReadableCached(const void* p, size_t len);

// ===========================================================================
// 配置（DLC2_MemFix.ini，放 asi 同目录或游戏根目录）
// ===========================================================================
struct Config {
    bool     enabled;      // Enabled        默认 1
    uint32_t sizeMax;      // SizeMax        默认 65536（超过这个大小不导向）
    bool     onlyHighAddr; // OnlyHighAddr   默认 1（只导向 >=2GB 的返回）
    uint32_t arenaMb;      // ArenaMB        默认 32
    uint32_t arenaBase;    // ArenaBase(hex) 默认 60000000
    bool     logAlloc;     // LogAlloc       默认 0
    uint32_t waitSec;      // WaitSec        默认 120（等 Kernel 对象出现）
};
extern Config g_cfg;
void LoadConfig(HMODULE self);

// ===========================================================================
// 低地址分配池（与 Render9Fix/lowheap.cpp 同源）
// ===========================================================================
namespace lowheap {

bool     Init(uint32_t preferredBase, uint32_t bytes);
void*    Alloc(size_t size);
void*    Realloc(void* userPtr, size_t size);
void     Free(void* userPtr);
bool     Owns(const void* userPtr);
uint32_t BlockSize(const void* userPtr);
void GetStats(uint64_t* allocCount, uint64_t* liveBytes, uint64_t* peakBytes,
              uint32_t* reserveBytes, uint32_t* commitBytes);
bool SelfTest();
uintptr_t Base();
uintptr_t Limit();

}  // namespace lowheap

// ===========================================================================
// 分配器挂钩（替换 Kernel+0x30/0x34/0x38 三个函数指针）
// ===========================================================================
namespace memhook {

// 轮询等 Kernel 对象出现，校验通过后安装挂钩。
bool InstallWhenReady(uint32_t timeoutMs);

bool Installed();

// 打印一行统计（只在有变化时输出）
void LogSummary();

}  // namespace memhook
