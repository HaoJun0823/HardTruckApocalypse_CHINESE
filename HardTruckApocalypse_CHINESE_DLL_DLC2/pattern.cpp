// pattern.cpp —— 按特征码在模块内定位函数
//
// 为什么不用硬编码地址：本体是 hta.exe (6.6MB)，DLC1 是 Meridian113.exe (9.3MB)，
// DLC2 是 emarcade.exe (8.6MB)，三者字体代码同源但地址完全不同
// （例如度量函数 0x685990 / 0x690A10 / 0x82DD90）。
// 用特征码扫描让定位与具体版本解耦 —— 失配时是「拒绝安装」而不是「装错地方」。
#include "pch.h"
#include "plugin.h"

namespace pattern {

namespace {

// 解析 "8B 57 40 0F B6" / "8B ?? 40" 形式的模式串。
// 只需解析一次，结果缓存。
struct PatByte { uint8_t val; bool wild; };

int ParsePattern(const char* pat, PatByte* out, int maxOut) {
    int n = 0;
    const char* p = pat;
    while (*p && n < maxOut) {
        while (*p == ' ') ++p;
        if (!*p) break;

        if (p[0] == '?' && p[1] == '?') {
            out[n].wild = true; out[n].val = 0;
            p += 2; ++n; continue;
        }
        // 十六进制两位
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hex(p[0]);
        int lo = (p[1] && p[1] != ' ') ? hex(p[1]) : -1;
        if (hi < 0) { ++p; continue; }
        if (lo < 0) { out[n].wild = false; out[n].val = (uint8_t)hi; p += 1; ++n; continue; }
        out[n].wild = false; out[n].val = (uint8_t)((hi << 4) | lo);
        p += 2; ++n;
    }
    return n;
}

// 取模块在内存中的映像范围（含对齐的首尾）
bool ModuleRange(HMODULE mod, uintptr_t* base, size_t* size) {
    if (!mod) return false;
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *base = (uintptr_t)mod;
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

} // namespace

uintptr_t ScanRange(uintptr_t base, size_t size, const char* pat) {
    PatByte pb[256];
    int plen = ParsePattern(pat, pb, 256);
    if (plen <= 0) return 0;
    if (size < (size_t)plen) return 0;

    const uint8_t* p    = (const uint8_t*)base;
    const uint8_t* end  = p + size - plen;
    const uint8_t* first = nullptr;
    for (int i = 0; i < plen; ++i)
        if (!pb[i].wild) { first = p; break; }
    (void)first;

    for (const uint8_t* cur = p; cur <= end; ++cur) {
        int i = 0;
        for (; i < plen; ++i) {
            if (!pb[i].wild && cur[i] != pb[i].val) break;
        }
        if (i == plen) return (uintptr_t)cur;
    }
    return 0;
}

int CountModule(HMODULE mod, const char* pat) {
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleRange(mod, &base, &size)) return 0;

    PatByte pb[256];
    int plen = ParsePattern(pat, pb, 256);
    if (plen <= 0) return 0;

    // 只在可执行段内找
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    int count = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = base + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (ss < (size_t)plen) continue;

        const uint8_t* p   = (const uint8_t*)sb;
        const uint8_t* end = p + ss - plen;
        for (const uint8_t* cur = p; cur <= end; ++cur) {
            int i = 0;
            for (; i < plen; ++i)
                if (!pb[i].wild && cur[i] != pb[i].val) break;
            if (i == plen) ++count;
        }
    }
    return count;
}

uintptr_t ScanModule(HMODULE mod, const char* pat) {
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleRange(mod, &base, &size)) return 0;

    // 优先在可执行段内扫描（代码特征码不该出现在 .rdata/.data）
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = base + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        uintptr_t hit = ScanRange(sb, ss, pat);
        if (hit) return hit;
    }
    return 0;
}

// ★ 一次扫描同时拿到「首个命中」和「总命中数」★
//
//   为什么需要它（实测 2026-10-07）：
//     引入「多命中即拒绝」的保护时，ScanUnique 变成了
//         ScanModule() + CountModule()
//     两次全模块扫描。而 ScanRange 是逐字节朴素匹配（O(n·m)），
//     结果每个锚点要扫两遍 6.6MB —— 实测单次 ScanModule 就要 3.5~4.9 秒，
//     整个初始化从基线的 **4 秒** 涨到 **>19 秒还没跑完**。
//     用户在跑完之前手动关了游戏，看到的是「补丁装了一半」的中间状态：
//     P4/P4b 已改走我们的表、表却是空的 ⇒ GBK 字节被当 cp1251 解码
//     ⇒ 满屏西里尔乱码 + 度量未修所以叠字 + 后续没跑所以空白。
//
//   这不是崩溃，是性能回归暴露出的半成品状态。
//   —— 必须同时消掉重复扫描，否则保护机制本身就是 bug。
int ScanModuleCount(HMODULE mod, const char* pat, uintptr_t* outFirst) {
    if (outFirst) *outFirst = 0;
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleRange(mod, &base, &size)) return 0;

    PatByte pb[256];
    int plen = ParsePattern(pat, pb, 256);
    if (plen <= 0) return 0;

    // 选**最靠前**的非通配字节做跳跃锚点（首字节通常是 0x8B/0x83，
    // 命中率太低；中间那些更稀有）。这是纯性能优化，不改语义。
    int anchor = -1;
    for (int i = 0; i < plen; ++i) {
        if (pb[i].wild) continue;
        anchor = i; break;
    }
    // 首字节若过于常见（0x00/0xFF/0x8B），再往后找一个更稀有的
    if (anchor >= 0) {
        static const uint8_t kCommon[] = { 0x00, 0xFF, 0x8B, 0x89, 0x83, 0x74, 0x75 };
        bool common = false;
        for (uint8_t c : kCommon) if (pb[anchor].val == c) { common = true; break; }
        if (common) {
            for (int i = anchor + 1; i < plen; ++i) {
                if (pb[i].wild) continue;
                bool c2 = false;
                for (uint8_t c : kCommon) if (pb[i].val == c) { c2 = true; break; }
                if (!c2) { anchor = i; break; }
            }
        }
    }

    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int count = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = base + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (ss < (size_t)plen) continue;

        const uint8_t* p   = (const uint8_t*)sb;
        const uint8_t* end = p + ss - plen;
        const uint8_t* cur = p;
        if (anchor < 0) {
            for (; cur <= end; ++cur) {
                int i = 0;
                for (; i < plen; ++i)
                    if (!pb[i].wild && cur[i] != pb[i].val) break;
                if (i != plen) continue;
                if (!count && outFirst) *outFirst = (uintptr_t)cur;
                ++count;
            }
        } else {
            const uint8_t av = pb[anchor].val;
            for (; cur + anchor <= end; ++cur) {
                if (cur[anchor] != av) continue;      // ★ 一次跳跃过滤掉绝大多数位置
                int i = 0;
                for (; i < plen; ++i)
                    if (!pb[i].wild && cur[i] != pb[i].val) break;
                if (i != plen) continue;
                if (!count && outFirst) *outFirst = (uintptr_t)cur;
                ++count;
            }
        }
    }
    return count;
}

} // namespace pattern
