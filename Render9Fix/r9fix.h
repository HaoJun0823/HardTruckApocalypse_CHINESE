// r9fix.h —— Render9Fix：把 dxrender9.dll 发起的「小对象」分配导向 2GB 以下的低地址
//
// ============================ 为什么需要它 ============================
//
// 已取证的事实链（2026-10-07 用 null_dbg 调试器 + minidump + exmachina.log 确认）：
//
//   1. hta.exe 打上 LAA 后，引擎内存池（MemoryManager 的 chunk）整体抬到 0x83xxxxxx
//      —— 即 >= 2GB 的地址区间。
//   2. 崩溃点固定为 dxrender9+0x416D6：effect 的 technique 计数为 0，之后走
//      「没有默认 technique 就用第一个」的警告路径，取 techArr[0]（NULL）解引用。
//   3. 现场取证显示 techCount=0 / techArr=0 / defaultIdx=-1，且 inDxRender9=1
//      —— 只有崩溃线程自己在 dxrender9 里，**没有并发方**。
//   4. exmachina.log 最后一行：
//        EffectImpl() error: effect = 'data/shaders/road.fx',
//        could not validate technique '<乱码>', skipping it...
//      那串「乱码」的原始 29 字节是 msvcr71.dll+0x218F 处**真实的机器码**
//      （_free 尾部 retn + 紧随的 push/call/pop/ret thunk 序列）—— 说明
//      techDesc 结构体根本没被写，Name 字段是**栈残留**的旧返回地址
//      （0x7C34218F）。也就是说 GetTechniqueDesc 失败了。
//   5. 对应源码（retruxx 1:1 还原）：
//        D3DXHANDLE h = m_effect->GetTechnique(i);
//        m_effect->GetTechniqueDesc(h, &techDesc);      // ← HRESULT 没检查
//        if (m_effect->ValidateTechnique(h) != D3D_OK)  // ← 无效句柄必然失败
//            LogMsg("...could not validate technique '%s'...", techDesc.Name);
//      ⇒ GetTechnique(i) 返回了无效句柄。
//   6. 单核 + LAA + 无调试器仍然崩；非 LAA 一切正常；4ms 调试事件延迟能「治好」
//      —— 延迟并没有改变任何单线程计算结果，它改变的是**各线程分配交错**
//      ⇒ 内存池布局变了 ⇒ road.fx 的 D3DX 对象有时落在 2GB 以下。
//      跨 dump 统计：出事的 EffectImpl 地址是 0x83BE8CF4 / 0x83BEE064 / 0x83BF228C
//      / 0x83C7FFE4 / 0x84C51C84（全部 >= 2GB），而有一次是 0x65AAF12C（< 2GB）。
//
//   结论：dxrender9 里静态链接的 D3DX（2006 年代码）在 >= 2GB 的地址上会出错
//         —— 典型的「指针被当成有符号 32 位整数参与运算」类老代码问题。
//         这解释了全部观察，且与汉化无关。
//
// ============================ 本插件怎么做 ============================
//
// 不修改任何可执行文件，只改 3 个**数据指针**：
//
//   1. 在 2GB 以下预留一段地址空间，自建一个定长块小分配池（lowheap）。
//   2. 找到引擎的 Kernel 对象（Kernel+0x18/0x1C/0x20 =
//      g_mar.AllocMem / ReallocMem / FreeMem，见 retruxx lib/include/core/kernel.h），
//      把这三个函数指针换成我们的。
//   3. 只拦截「调用方在 dxrender9.dll 内」且「大小 <= sizeMax」的分配，交给低地址池；
//      其余一律原样转交引擎分配器。
//      ⇒ dxrender9 的小对象（含 D3DX 的 effect 对象、technique 表、参数表）全部落回
//        2GB 以下，恢复 2006 年 D3DX 能正确处理的地址语义；
//        引擎其余部分（纹理、顶点缓冲等大块）照旧用满 4GB。
//
//   sizeMax 默认 64KB：data\shaders 下最大的 fx 只有 9686 字节，D3DX 的 effect
//   内部结构也都是几百字节到几 KB —— 64KB 足够覆盖，且低地址池占用极小。
//
// ============================ 安全设计 ============================
//
//   * 不 patch 任何代码；安装前先用「魔数探针」验证 Kernel 定位正确
//     （分配 64 字节，检查块头 0xDEADBEEF 与尾哨 0xFEEBDAED，再释放）。
//     探针失败就什么都不做，游戏不受影响。
//   * Free/Realloc 按**地址**分派：落在我们保留区内的归我们，其他一概交还原函数
//     —— 即使换指针的瞬间发生并发分配/释放也不会串味。
//   * 换指针顺序：先 Realloc/Free，最后 Alloc。这样「我们分配的块」绝不会被
//     原 FreeMem 看到（原 Free 读到我们的块头会判魔数不符并 __debugbreak）。
//   * 低地址池用尽时自动回退到引擎分配器 —— 游戏照常运行，只是不再导向。
//   * 池内每个块带 16 字节头（cls + 魔数）；Free 校验失败只泄漏、不破坏。
//   * 不安装任何代码钩子，不需要长度反汇编器，不需要可执行内存。
//
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
#error "Render9Fix 必须编译为 Win32 (x86)：游戏 hta.exe 是 32 位进程"
#endif

#define R9FIX_NAME "Render9Fix"

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

// 区间 / 段属性（用于校验候选 Kernel 对象）
bool InRange(uintptr_t a, uintptr_t base, size_t size);
bool AddrInExec  (const ModInfo& m, uintptr_t a);
bool AddrInRoData(const ModInfo& m, uintptr_t a);
bool AddrInWritable(const ModInfo& m, uintptr_t a);

// 可读性检查。用 VirtualQuery 实现，不靠异常。
// 扫描热路径请用 ReadableCached（先 BuildReadableMap）。
bool IsReadable(const void* p, size_t len);
void BuildReadableMap();
bool ReadableCached(const void* p, size_t len);

// ===========================================================================
// 配置（Render9Fix.ini，放 asi 同目录或游戏根目录）
// ===========================================================================
struct Config {
    bool     enabled;        // Enabled            默认 1
    uint32_t sizeMax;        // SizeMax            默认 65536
    bool     onlyDxRender9;  // OnlyDxRender9      默认 1
    uint32_t arenaMb;        // ArenaMB            默认 32
    uint32_t arenaBase;      // ArenaBase (hex)    默认 60000000
    bool     logAlloc;       // LogAlloc           默认 0（每笔导向都记，会很大）
    uint32_t waitSec;        // WaitSec            默认 60（等 kernel 出现）
};
extern Config g_cfg;
void LoadConfig(HMODULE self);

// ===========================================================================
// 低地址分配池
//
// 单块预留区 + 按需提交 + 定长块空闲链表。所有返回指针保证 < 2GB。
// 线程安全（内部一个 CRITICAL_SECTION）。
// ===========================================================================
namespace lowheap {

// 在 base 处预留 bytes 字节（会自动往下试多个候选基址）。成功返回 true。
bool Init(uint32_t preferredBase, uint32_t bytes);

void* Alloc(size_t size);
void* Realloc(void* userPtr, size_t size);
void  Free(void* userPtr);

// 指针是否落在本池的预留区内（无锁；用于分派）
bool Owns(const void* userPtr);

// 用户块的实际容量（载荷字节数）；不属于本池返回 0
uint32_t BlockSize(const void* userPtr);

void GetStats(uint64_t* allocCount, uint64_t* liveBytes, uint64_t* peakBytes, uint32_t* reserveBytes,
              uint32_t* commitBytes);

// 自检：分配若干尺寸、验证内容与地址、释放并复用。结果写入日志。
bool SelfTest();

uintptr_t Base();
uintptr_t Limit();

}  // namespace lowheap

// ===========================================================================
// 分配器挂钩
// ===========================================================================
namespace memhook {

// 轮询等待 dxrender9 与 Kernel 就绪，校验通过后安装挂钩。
bool InstallWhenReady(uint32_t timeoutMs);

bool Installed();

// 打印一行统计（只在有变化时输出）
void LogSummary();

// 供日志使用：把 dxrender9 内的地址转成 "dxrender9+0x1234"；不在其中返回 false
bool FormatCaller(const void* addr, char* out, size_t cap);

}  // namespace memhook
