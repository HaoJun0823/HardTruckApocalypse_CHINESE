// sigtest.cpp —— DLC1 离线校验工具（不启动游戏）
//
// 用途：把 hta.exe / Meridian113.exe 以 SEC_IMAGE 映射进本进程，调用与插件
//       **完全相同**的扫描代码，逐项断言 DLC1 插件用到的每一个锚点。
//
// 为什么要有这个：地址是写死的（用户确认的取舍），所以一旦特征码/地址写错，
// 后果是进游戏才崩。这个工具把它变成"构建期就能发现的错误"。
//
// 构建：tools\build_tools.bat
// 运行：sigtest.exe "I:\...\hta.exe" "I:\...\Meridian113.exe"
//
// 注意：比较一律在 **RVA 空间** 进行。SEC_IMAGE 映射的实际基址未必等于
//       ImageBase，绝对地址不可比。
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

namespace lde {
struct Insn { int len; int relOff; int relSize; };
bool Decode(const uint8_t* code, int maxLen, Insn* out);
}
namespace pattern {
uintptr_t ScanRange(uintptr_t base, size_t size, const char* pat);
uintptr_t ScanModule(HMODULE mod, const char* pat);
int CountModule(HMODULE mod, const char* pat);
}

// ═══════════════════════════════════════════════════════════════════════════
// 每个 exe 的全部期望值（RVA）
// ═══════════════════════════════════════════════════════════════════════════
struct Prof {
    const char* tag;
    // 字体引擎 / 渲染器
    uint32_t measure, draw, bake, loadXml, fontCtor, reserve, flush;
    // 路径 D 补丁点
    uint32_t p4anchor, p4b, p5, p7, p8m, p2, p3;
    // hook 与全局
    uint32_t gate, gfxEntry, appInit, engineAlloc;
    uint32_t uiCore, gfxGlobal;
    // 度量循环头 / 退出（由 P8 指令流推导，这里做独立期望值）
    uint32_t loopHead, loopExit;
    // pathd.cpp 里的 RVA_EngineCore —— 必须是 uiCore 全局的 **RVA**（VA - 0x400000）
    uint32_t engineCoreRva;
    // P5 的 4 条 rel32 跳入点
    uint32_t jump[4];
    // P2/P3 的特征码（113 的推进寄存器是 esi，本体是 edi）
    const char* sigP2;
    const char* sigP3;
    // GfxServer 入口特征码（两版序言不同）
    const char* sigGfxEntry;
    // 引擎分配器特征码（113 把全局写进签名以唯一化）
    const char* sigEngineAlloc;
};

static const Prof kProf[] = {
    {
        "hta.exe",
        0x685990, 0x685CA0, 0x8B9350, 0x8B80B0, 0x8B6740, 0x7B0110, 0x7AFC50,
        0x6865A2, 0x686A26, 0x686A52, 0x685B21, 0x685BC3, 0x68620D, 0x686221,
        0x5AA388, 0x6843F0, 0x5A9040, 0x589410,
        0xA0A88C, 0xA13CC0,
        0x685A80, 0x685C12,
        0x60A88C,                                  // RVA_EngineCore（本体：0xA0A88C - 0x400000）
        { 0x6864ED, 0x686507, 0x68658B, 0x68659C },
        "83 C7 01 EB ?? 0F 28 C8",
        "83 C7 01 EB ?? 8B 46 18 83 F8 03",
        "81 EC 8C 00 00 00 8B 84 24",
        "8B 44 24 04 50 52 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C2 04 00",
    },
    {
        "Meridian113.exe",
        0x690A10, 0x690D20, 0x7492D0, 0x747D40, 0x746350, 0x71F350, 0x71F0F0,
        0x691682, 0x691B06, 0x691B32, 0x690BA1, 0x690C43, 0x6912F2, 0x691306,
        0x6208C1, 0x688C50, 0x61E9C0, 0x5C0CE0,
        0xC7BA0C, 0xC9BA10,
        0x690B00, 0x690C92,
        0x87BA0C,                                  // RVA_EngineCore（113：0xC7BA0C - 0x400000）
        { 0x6915CD, 0x6915E7, 0x69166B, 0x69167C },
        "83 C6 01 EB ?? 0F 28 C8",
        "83 C6 01 EB ?? 8B BC 24",
        "81 EC 8C 00 00 00 53 55 56 57 8B BC 24",
        "8B 44 24 04 50 52 51 8B 0D 00 BA C7 00 E8 ?? ?? ?? ?? C2 04 00",
    },
};

static int g_fail = 0;
static HMODULE g_mod = nullptr;

// ── 小工具 ─────────────────────────────────────────────────────────────
static uint8_t  rd8(uintptr_t a) { return *(uint8_t*)a; }
static uint32_t rd32(uintptr_t a) {
    return (uint32_t)rd8(a) | ((uint32_t)rd8(a + 1) << 8)
         | ((uint32_t)rd8(a + 2) << 16) | ((uint32_t)rd8(a + 3) << 24);
}
static int32_t rd32s(uintptr_t a) { return (int32_t)rd32(a); }
static uint32_t Rva(uintptr_t abs) { return abs ? (uint32_t)(abs - (uintptr_t)g_mod) : 0; }

// ★ 表里所有数字都是 IDA 里看到的**绝对 VA**（两个 exe 的 ImageBase 都是 0x400000），
//   而 Rva() 得到的是「相对**实际映射基址**」的偏移 —— 二者必须先换算再比。
//   （SEC_IMAGE 的实际映射基址不等于 ImageBase，所以绝对地址 нельзя 直接比。）
static const uint32_t kBase = 0x400000;
#define VA2RVA(va) ((uint32_t)((va) - kBase))

static uintptr_t A(uint32_t va) { return (uintptr_t)g_mod + (va - kBase); }

static const char* HexDump(uintptr_t a, int n) {
    static char buf[8][200];
    static int which = 0;
    char* p = buf[which]; which = (which + 1) & 7;
    char* q = p;
    for (int i = 0; i < n; ++i) q += sprintf_s(q, 8, "%02X ", rd8(a + i));
    q[-1] = 0;
    return p;
}

static bool BytesMatch(uintptr_t a, const char* expect) {
    const char* p = expect;
    size_t i = 0;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { p += 2; ++i; continue; }
        char t[3] = { p[0], p[1], 0 };
        uint8_t want = (uint8_t)strtoul(t, nullptr, 16);
        if (rd8(a + i) != want) return false;
        p += 2; ++i;
    }
    return true;
}

static void Chk(const char* name, bool ok, const char* detail) {
    if (!ok) ++g_fail;
    printf("  %s %-26s %s\n", ok ? "OK  " : "FAIL", name, detail ? detail : "");
}

// 断言「签名唯一命中且落在期望 RVA」
static uintptr_t ScanAt(HMODULE mod, const char* name, const char* sig, uint32_t expectVA) {
    char detail[256];
    int hits = pattern::CountModule(mod, sig);
    uintptr_t got = pattern::ScanModule(mod, sig);
    uint32_t rva = got ? (uint32_t)(got - (uintptr_t)mod) : 0;
    bool ok = (got != 0) && (rva == VA2RVA(expectVA));
    sprintf_s(detail, sizeof(detail), "签名命中 %d 处  RVA=0x%06X  期望=0x%06X",
              hits, (unsigned)rva, (unsigned)VA2RVA(expectVA));
    Chk(name, ok, detail);
    return got;
}

static void ScanModuleAt(uintptr_t abs, const char* name, const char* expect, uint32_t expectVA) {
    uintptr_t want = A(expectVA);
    char detail[256];
    bool ok = (abs == want) && BytesMatch(want, expect);
    sprintf_s(detail, sizeof(detail), "@0x%06X  字节 %s", (unsigned)VA2RVA(expectVA),
              ok ? "与预期一致" : HexDump(want, 12));
    Chk(name, ok, detail);
}

static HMODULE MapImageForTest(const char* path) {
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { printf("  CreateFile 失败 %lu\n", GetLastError()); return NULL; }
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
    CloseHandle(hf);
    if (!hm) { printf("  CreateFileMapping(SEC_IMAGE) 失败 %lu\n", GetLastError()); return NULL; }
    void* base = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hm);
    if (!base) { printf("  MapViewOfFile 失败 %lu\n", GetLastError()); return NULL; }
    return (HMODULE)base;
}

static int FindAll(HMODULE mod, const char* pat, uintptr_t* out, int maxOut) {
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = (uintptr_t)mod + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        uintptr_t cur = sb, end = sb + ss;
        while (cur < end && n < maxOut) {
            uintptr_t hit = pattern::ScanRange(cur, (size_t)(end - cur), pat);
            if (!hit) break;
            out[n++] = hit;
            cur = hit + 1;
        }
    }
    return n;
}

// 在可执行段里穷举所有 "E9 rel32" / "0F 8x rel32"，收集目标 == wantTarget 的跳转
static int FindJumpsTo(HMODULE mod, uint32_t wantTargetVA, uint32_t* outs, int maxOut) {
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = (uintptr_t)mod + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        for (uintptr_t p = sb; p + 6 < sb + ss && n < maxOut; ++p) {
            int len = 0;
            if (rd8(p) == 0xE9) len = 5;
            else if (rd8(p) == 0x0F && (rd8(p + 1) & 0xF0) == 0x80) len = 6;
            else continue;
            uintptr_t tgt = (uintptr_t)((int64_t)(p + len) + (int64_t)rd32s(p + len - 4));
            if (tgt == (uintptr_t)mod + VA2RVA(wantTargetVA)) {
                outs[n++] = (uint32_t)(p - (uintptr_t)mod);
            }
        }
    }
    return n;
}

// 扫 "8B 0D <imm32>"（mov ecx,[imm32]），看其后 24 字节内是否有 call rel32 指向 gfxEntry
static uint32_t FindGlobalByCallTarget(HMODULE mod, uint32_t gfxEntryVA) {
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = (uintptr_t)mod + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        for (uintptr_t p = sb; p + 16 < sb + ss; ++p) {
            if (rd8(p) != 0x8B || rd8(p + 1) != 0x0D) continue;
            uint32_t slot = rd32(p + 2);
            for (uintptr_t q = p + 6; q < p + 26; ++q) {
                if (rd8(q) != 0xE8) continue;
                uintptr_t tgt = (uintptr_t)((int64_t)(q + 5) + (int64_t)rd32s(q + 1));
                if (tgt == (uintptr_t)mod + VA2RVA(gfxEntryVA)) return slot;
            }
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
static int TestOne(const char* path) {
    printf("======================================================================\n");
    printf("%s\n", path);
    g_mod = MapImageForTest(path);
    if (!g_mod) return 1;
    g_fail = 0;

    const Prof* P = nullptr;
    {
        const char* bn = path;
        for (const char* q = path; *q; ++q) if (*q == '\\' || *q == '/') bn = q + 1;
        for (int i = 0; i < 2; ++i) if (lstrcmpiA(bn, kProf[i].tag) == 0) P = &kProf[i];
    }
    if (!P) { printf("  [跳过] 不在已知目标列表里（只有 hta.exe / Meridian113.exe）\n"); UnmapViewOfFile(g_mod); return 0; }

    auto dos = (const IMAGE_DOS_HEADER*)g_mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)g_mod + dos->e_lfanew);
    printf("  ImageBase=0x%08X 实际映射=0x%08X SizeOfImage=0x%X\n",
           (unsigned)nt->OptionalHeader.ImageBase, (unsigned)(uintptr_t)g_mod,
           (unsigned)nt->OptionalHeader.SizeOfImage);

    // ── A. 字体引擎 ──────────────────────────────────────────────────
    puts(" [A] 字体引擎");
    ScanAt(g_mod, "measure 文本度量", "8B 4C 24 1C 83 EC 2C 56 57 33 FF 3B CF 74", P->measure);
    ScanAt(g_mod, "bake 运行时烘图", "81 EC A4 00 00 00 53 55 56 57 8B F9 E8", P->bake);
    ScanAt(g_mod, "loadXml XML加载", "81 EC F8 00 00 00 53 55 56 8B E9 E8", P->loadXml);
    ScanAt(g_mod, "fontCtor 字体构造", "0F 57 C0 33 C0 56 8B F1 89 46 04 8D 4E 08 89 0E 88 01 89 46 10 8D 4E 14", P->fontCtor);
    {
        uintptr_t cands[16];
        int nc = FindAll(g_mod, "81 EC E0 00 00 00 55", cands, 16);
        uintptr_t got = 0;
        if (nc > 0) {
            uintptr_t m = A(P->measure);
            for (int i = 0; i < nc; ++i) if (cands[i] > m && (!got || cands[i] < got)) got = cands[i];
            if (!got) got = cands[0];
        }
        char d[160];
        sprintf_s(d, sizeof(d), "命中 %d 处（取 measure 之后最近者）RVA=0x%06X 期望=0x%06X",
                  nc, (unsigned)Rva(got), (unsigned)VA2RVA(P->draw));
        Chk("draw 文本绘制", got && Rva(got) == VA2RVA(P->draw), d);
    }

    // ── B. 渲染器原语（替绘路径备用）──────────────────────────────────
    puts(" [B] 渲染器原语");
    ScanAt(g_mod, "reserve 预留顶点", "56 8B F1 8B 86 ?? ?? ?? ?? 83 C0 04 3D A0 0F 00 00 72 ?? E8 ?? ?? ?? ?? 8B 8E ?? ?? ?? ?? 8D 14 CD", P->reserve);
    ScanAt(g_mod, "flush   冲刷批次", "51 53 8B D9 8B 83 ?? ?? ?? ?? 85 C0", P->flush);

    // ── C. 路径 D 补丁点 ─────────────────────────────────────────────
    puts(" [C] 路径 D 补丁点（签名必须唯一命中）");
    ScanAt(g_mod, "P4 主查表(锚点+4)", "8B 7C 24 34 8B 57 40 0F B6 EB 03 ED 03 ED 8B 04 2A", P->p4anchor);
    ScanAt(g_mod, "P4b 第二处查表", "8B 44 24 34 8B 48 40 8B 04 29 85 C0 74 07 F3 0F 10 40 2C", P->p4b);
    ScanAt(g_mod, "P5 主遍历", "0F 57 C0 83 C6 01 83 C7 01 3B 74 24", P->p5);
    ScanAt(g_mod, "P7 度量宽度", "8B 74 24 4C 56 E8 ?? ?? ?? ?? D8 44 24 14", P->p7);
    ScanAt(g_mod, "P8 度量推进(锚点+14)", "F3 0F 10 44 24 14 8B 4C 24 10 8B 74 24 20 83 C7 01", P->p8m);
    ScanAt(g_mod, "P2 预扫遍历A", P->sigP2, P->p2);
    ScanAt(g_mod, "P3 预扫遍历B", P->sigP3, P->p3);

    // ── D. 补丁窗口的原始字节（写死地址必须逐字节对）──────────────────
    puts(" [D] 补丁窗口原始字节");
    ScanModuleAt(A(P->p4anchor) + 4, "P4  18 字节",
                 "8B 57 40 0F B6 EB 03 ED 03 ED 8B 04 2A 85 C0 8D 0C 2A C6", P->p4anchor + 4);
    ScanModuleAt(A(P->p4b), "P4b 10 字节", "8B 44 24 34 8B 48 40 8B 04 29", P->p4b);
    ScanModuleAt(A(P->p5), "P5  9 字节", "0F 57 C0 83 C6 01 83 C7 01 3B", P->p5);
    ScanModuleAt(A(P->p7), "P7  10 字节", "8B 74 24 4C 56 E8", P->p7);
    ScanModuleAt(A(P->p7) + 10, "P7 落点 fadd", "D8 44 24 14", P->p7 + 10);
    ScanModuleAt(A(P->p8m) + 14, "P8  13 字节", "83 C7 01 3B 7C 24 1C 0F 8C", P->p8m + 14);
    // P2/P3 的推进寄存器按 exe 不同（本体 edi / 113 esi），
    // 字节期望值直接取各自特征码的前 11 个字符（"83 C6 01 EB" / "83 C7 01 EB"）。
    char p2b[16] = {0}, p3b[16] = {0};
    memcpy(p2b, P->sigP2, 11);
    memcpy(p3b, P->sigP3, 11);
    ScanModuleAt(A(P->p2), "P2  3 字节+回跳", p2b, P->p2);
    ScanModuleAt(A(P->p3), "P3  3 字节+回跳", p3b, P->p3);

    // ── E. 度量循环头 / 退出（从 P8 指令流推导，与期望值独立比对）────
    puts(" [E] 度量循环头 / 退出（推导）");
    {
        uintptr_t b = A(P->p8m) + 14;
        uintptr_t loopHead = (uintptr_t)((int64_t)(b + 13) + (int64_t)rd32s(b + 9));
        int8_t    exitRel  = (int8_t)rd8(b + 14);
        uintptr_t loopExit = (uintptr_t)((int64_t)(b + 15) + exitRel);
        char d[200];
        sprintf_s(d, sizeof(d), "jl 目标 RVA=0x%06X 期望=0x%06X", (unsigned)Rva(loopHead), (unsigned)VA2RVA(P->loopHead));
        Chk("循环头", Rva(loopHead) == VA2RVA(P->loopHead), d);
        sprintf_s(d, sizeof(d), "jmp short 目标 RVA=0x%06X 期望=0x%06X", (unsigned)Rva(loopExit), (unsigned)VA2RVA(P->loopExit));
        Chk("循环退出", Rva(loopExit) == VA2RVA(P->loopExit), d);
    }

    // ── F. P5 的 4 条 rel32 跳入点 ───────────────────────────────────
    puts(" [F] P5 的 rel32 跳入点（目标 = P5+3）");
    {
        uint32_t got[32];
        int n = FindJumpsTo(g_mod, P->p5 + 3, got, 32);
        char d[256];
        sprintf_s(d, sizeof(d), "找到 %d 条:", n);
        for (int i = 0; i < n; ++i) sprintf_s(d + strlen(d), 64, " 0x%06X", (unsigned)got[i]);
        Chk("跳入点数量 == 4", n == 4, d);
        for (int k = 0; k < 4; ++k) {
            bool found = false;
            for (int i = 0; i < n; ++i) if (got[i] == VA2RVA(P->jump[k])) found = true;
            sprintf_s(d, sizeof(d), "期望 0x%06X", (unsigned)VA2RVA(P->jump[k]));
            Chk("跳入点命中", found, d);
        }
        // 打全，便于人工核对
        printf("       实际跳入点:");
        for (int i = 0; i < n; ++i) printf(" 0x%06X", (unsigned)got[i]);
        printf("\n");
    }

    // ── G. hook 点与全局变量 ─────────────────────────────────────────
    puts(" [G] hook 点与全局变量");
    ScanAt(g_mod, "冻结门 mov eax,1", "B8 01 00 00 00 5B 81 C4 F0 00 00 00 C2 14 00", P->gate);
    ScanAt(g_mod, "GfxServer 入口", P->sigGfxEntry, P->gfxEntry);
    {
        uintptr_t got = pattern::ScanModule(g_mod, "A1 ?? ?? ?? ?? 81 EC F0 00 00 00 53 56 8B F1");
        char d[200];
        uint32_t rva = Rva(got), slot = got ? rd32(got + 1) : 0;
        sprintf_s(d, sizeof(d), "RVA=0x%06X 期望=0x%06X  读的全局=0x%06X 期望=0x%06X",
                  (unsigned)rva, (unsigned)VA2RVA(P->appInit), (unsigned)slot, (unsigned)P->uiCore);
        Chk("Application::init", got && rva == VA2RVA(P->appInit) && slot == P->uiCore, d);
    }
    {
        uintptr_t got = pattern::ScanModule(g_mod, P->sigEngineAlloc);
        char d[200];
        sprintf_s(d, sizeof(d), "RVA=0x%06X 期望=0x%06X  读的全局=0x%06X",
                  (unsigned)Rva(got), (unsigned)VA2RVA(P->engineAlloc), (unsigned)(got ? rd32(got + 9) : 0));
        Chk("引擎分配器", got && Rva(got) == VA2RVA(P->engineAlloc), d);
    }
    {
        uint32_t slot = FindGlobalByCallTarget(g_mod, P->gfxEntry);
        char d[160];
        sprintf_s(d, sizeof(d), "结构反查得到 0x%06X 期望=0x%06X", (unsigned)slot, (unsigned)P->gfxGlobal);
        Chk("GfxServer 全局", slot == P->gfxGlobal, d);
    }
    {
        // uiCore 全局与「引擎分配器读的全局」必须相差 0xC（结构不变式）
        uintptr_t alloc = pattern::ScanModule(g_mod, P->sigEngineAlloc);
        uint32_t allocSlot = alloc ? rd32(alloc + 9) : 0;
        char d[160];
        sprintf_s(d, sizeof(d), "分配器全局 0x%06X + 0xC == uiCore 0x%06X",
                  (unsigned)allocSlot, (unsigned)P->uiCore);
        Chk("结构不变式 +0xC", allocSlot && allocSlot + 0xC == P->uiCore, d);
    }

    {
        // ★ RVA_EngineCore 必须是 uiCore 全局的 RVA（VA − 0x400000）★
        //   这里栽过一次：写成了绝对 VA，于是 *(base+VA) 读到模块外 -> 启动即 0xC0000005。
        char d[160];
        sprintf_s(d, sizeof(d), "pathd.cpp 里写 0x%06X，应为 uiCore(0x%06X) − 0x400000 = 0x%06X",
                  (unsigned)P->engineCoreRva, (unsigned)P->uiCore, (unsigned)VA2RVA(P->uiCore));
        Chk("RVA_EngineCore", P->engineCoreRva == VA2RVA(P->uiCore), d);
    }

    // ── H. 反汇编器自检 ──────────────────────────────────────────────
    puts(" [H] ldisasm 长度自检");
    {
        struct Case { const char* bytes; int len; const char* note; };
        static const Case kCases[] = {
            { "\x81\xEC\xF8\x00\x00\x00", 6, "sub esp,0F8h" },
            { "\x81\xEC\x8C\x00\x00\x00", 6, "sub esp,8Ch"   },
            { "\x68\x00\x01\x00\x00",     5, "push 100h"     },
            { "\x8B\x7C\x24\x34",         4, "mov edi,[esp+34h]" },
            { "\x8D\x0C\x2A",             3, "lea ecx,[edx+ebp]" },
            { "\x0F\xB6\xEB",             3, "movzx ebp,bl"   },
            { "\x0F\x84\xB3\x04\x00\x00", 6, "jz rel32"       },
            { "\x0F\x8C\xA2\xFE\xFF\xFF", 6, "jl rel32"       },
            { "\xEB\xB9",                 2, "jmp rel8"       },
            { "\x83\xC6\x01",             3, "add esi,1"      },
            { "\xF3\x0F\x10\x44\x24\x14", 6, "movss xmm0,[esp+14h]" },
            { "\xD8\x44\x24\x14",         4, "fadd [esp+14h]" },
            { "\x0F\x57\xC0",             3, "xorps xmm0,xmm0" },
            { "\x88\x5C\x24\x40",         4, "mov [esp+40h],bl" },
        };
        int bad = 0;
        for (int k = 0; k < (int)(sizeof(kCases) / sizeof(kCases[0])); ++k) {
            lde::Insn in;
            bool ok = lde::Decode((const uint8_t*)kCases[k].bytes, 16, &in) && in.len == kCases[k].len;
            if (!ok) { ++bad; printf("  FAIL %-24s 期望 %d 实际 %d\n", kCases[k].note, kCases[k].len, in.len); }
        }
        char d[80];
        sprintf_s(d, sizeof(d), "%d 条全部正确", (int)(sizeof(kCases) / sizeof(kCases[0])));
        Chk("指令长度", bad == 0, d);
        g_fail += bad;
    }

    UnmapViewOfFile(g_mod);
    g_mod = nullptr;
    return g_fail;
}

int main(int argc, char** argv) {
    printf("DLC1 (hta_chs_dlc1) 锚点离线校验工具\n\n");
    if (argc < 2) { printf("用法: sigtest.exe <exe1> [exe2 ...]\n"); return 2; }
    int fail = 0;
    for (int i = 1; i < argc; ++i) fail += TestOne(argv[i]);
    printf("\n======================================\n");
    printf("结果: %s (%d 项未通过)\n", fail ? "有失败" : "全部通过", fail);
    return fail ? 1 : 0;
}
