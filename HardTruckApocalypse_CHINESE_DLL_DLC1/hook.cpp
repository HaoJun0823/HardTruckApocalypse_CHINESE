// hook.cpp —— 内联挂钩（trampoline）
//
// 做法：在被挂钩函数开头写入 `E9 rel32` 跳到 detour，把被覆盖的完整指令
// 复制到一块可执行内存（trampoline）末尾再 `E9` 跳回原函数剩余部分。
// 含相对位移的指令（E8/E9/0F 8x/Jcc）需要按新旧地址差修正位移。
#include "pch.h"
#include "plugin.h"

namespace hook {

namespace {

#pragma pack(push, 1)
struct Entry {
    uintptr_t target;      // 原函数地址
    uintptr_t trampoline;  // 可执行跳板
    int       patchLen;    // 覆盖的原始字节数（>=5）
    char      name[64];
};
#pragma pack(pop)

Entry g_entries[32];
int   g_count = 0;

// 把一段内存改为可读可写可执行
bool MakeExecutable(void* addr, size_t size) {
    DWORD old = 0;
    return VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old) != FALSE;
}

bool RestoreProtect(void* addr, size_t size, DWORD prot) {
    DWORD old = 0;
    return VirtualProtect(addr, size, prot, &old) != FALSE;
}

// 复制指令到 trampoline，同时修正相对位移
// dstBase/dstCur 是 trampoline 里的对应位置，srcBase/srcCur 是原函数里的位置
bool CopyInsns(uintptr_t src, int need, uint8_t* dst, int* copiedOut) {
    int copied = 0;
    const uint8_t* p = (const uint8_t*)src;

    while (copied < need) {
        lde::Insn insn;
        if (!lde::Decode(p, 16, &insn) || insn.len <= 0) {
            Logf("hook: 反汇编失败 @0x%08X", (unsigned)(src + copied));
            return false;
        }
        memcpy(dst + copied, p, insn.len);

        if (insn.relOff >= 0 && insn.relSize > 0) {
            uint8_t* relField = dst + copied + insn.relOff;
            int32_t  oldRel   = 0;
            if (insn.relSize == 4) {
                memcpy(&oldRel, p + insn.relOff, 4);
            } else {
                int8_t r8 = 0;
                memcpy(&r8, p + insn.relOff, 1);
                oldRel = r8;
            }
            // 原目标绝对地址
            uintptr_t srcNext = src + copied + insn.len;
            uintptr_t dstNext = (uintptr_t)(dst + copied + insn.len);
            uintptr_t absTarget = srcNext + oldRel;
            int64_t   newRel    = (int64_t)absTarget - (int64_t)dstNext;

            if (insn.relSize == 4) {
                if (newRel > INT32_MAX || newRel < INT32_MIN) {
                    Logf("hook: rel32 溢出 @0x%08X", (unsigned)(src + copied));
                    return false;
                }
                int32_t nr = (int32_t)newRel;
                memcpy(relField, &nr, 4);
            } else {
                if (newRel > INT8_MAX || newRel < INT8_MIN) {
                    Logf("hook: rel8 溢出 @0x%08X", (unsigned)(src + copied));
                    return false;
                }
                int8_t nr = (int8_t)newRel;
                memcpy(relField, &nr, 1);
            }
        }

        copied += insn.len;
        p       += insn.len;
    }

    *copiedOut = copied;
    return true;
}

} // namespace

bool Install(const char* name, uintptr_t target, void* detour, void** trampolineOut) {
    if (!target || !detour) return false;
    if (g_count >= (int)(sizeof(g_entries) / sizeof(g_entries[0]))) return false;

    // 1) 量出至少要覆盖多少字节才能塞下 5 字节 jmp
    //    同时把每条指令的长度记下来打日志 —— 一旦反汇编器解错（例如把
    //    组指令 81 /5 imm32 当成 2 字节），回跳点就会落在指令中间并崩，
    //    而长度序列能让人一眼看出问题。
    uint8_t* dst = (uint8_t*)target;
    int need = 0;
    char bounds[160] = {0};
    while (need < 5) {
        lde::Insn insn;
        if (!lde::Decode(dst + need, 16, &insn) || insn.len <= 0) {
            Logf("hook[%s]: 序言反汇编失败 @0x%08X", name, (unsigned)target);
            return false;
        }
        char t[16];
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%d ", insn.len);
        strcat_s(bounds, sizeof(bounds), t);
        need += insn.len;
        if (need > 32) { Logf("hook[%s]: 序言过长", name); return false; }
    }
    Logf("hook[%s]: 序言指令长度序列 = [%s] 合计 %d 字节（首字节 %02X %02X %02X %02X %02X %02X）",
         name, bounds, need, dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);

    // 2) 分配跳板：need 字节 + 5 字节回跳
    size_t trampSize = (size_t)need + 5;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, trampSize,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!tramp) { Logf("hook[%s]: VirtualAlloc 失败", name); return false; }

    // 3) 复制被覆盖的指令
    int copied = 0;
    if (!CopyInsns(target, need, tramp, &copied)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    // 4) 追加回跳
    uintptr_t backTo = target + copied;
    tramp[copied + 0] = 0xE9;
    int32_t backRel = (int32_t)((int64_t)backTo - (int64_t)(uintptr_t)(tramp + copied + 5));
    memcpy(tramp + copied + 1, &backRel, 4);

    // 5) 写跳转到 detour
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, copied, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        Logf("hook[%s]: VirtualProtect 失败 (%lu)", name, GetLastError());
        return false;
    }
    dst[0] = 0xE9;
    int32_t jmpRel = (int32_t)((int64_t)(uintptr_t)detour - (int64_t)(target + 5));
    memcpy(dst + 1, &jmpRel, 4);
    // 其余被覆盖字节填 NOP，保证反汇编整洁
    for (int i = 5; i < copied; ++i) dst[i] = 0x90;
    VirtualProtect(dst, copied, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), dst, copied);

    Entry& e = g_entries[g_count++];
    e.target     = target;
    e.trampoline = (uintptr_t)tramp;
    e.patchLen   = copied;
    lstrcpynA(e.name, name, sizeof(e.name));

    if (trampolineOut) *trampolineOut = tramp;

    Logf("hook[%s]: 0x%08X -> detour 0x%08X, trampoline 0x%08X (覆盖 %d 字节)",
         name, (unsigned)target, (unsigned)(uintptr_t)detour, (unsigned)(uintptr_t)tramp, copied);
    return true;
}

void UninstallAll() {
    for (int i = g_count - 1; i >= 0; --i) {
        Entry& e = g_entries[i];
        uint8_t* dst = (uint8_t*)e.target;
        DWORD oldProt = 0;
        if (VirtualProtect(dst, e.patchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            // 从跳板把原始字节还原
            memcpy(dst, (const void*)e.trampoline, e.patchLen);
            VirtualProtect(dst, e.patchLen, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), dst, e.patchLen);
        }
        if (e.trampoline) VirtualFree((void*)e.trampoline, 0, MEM_RELEASE);
    }
    g_count = 0;
}

} // namespace hook
