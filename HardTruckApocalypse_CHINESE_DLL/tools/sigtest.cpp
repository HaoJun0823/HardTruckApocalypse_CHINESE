// sigtest.cpp —— 特征码扫描与反汇编器的离线验证工具
//
// 用途：在不注入游戏的情况下，验证 pattern.cpp / ldisasm.cpp 的实现正确性。
// 做法：把 hta.exe / Meridian113.exe 以 SEC_IMAGE 映射进本进程（保留 PE 段布局
//       与 ImageBase 语义），然后对映像调用与插件完全相同的扫描代码。
//
// 构建：build_tools.bat
// 运行：sigtest.exe "I:\...\hta.exe" "I:\...\Meridian113.exe"
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

namespace lde {
struct Insn { int len; int relOff; int relSize; };
bool Decode(const uint8_t* code, int maxLen, Insn* out);
}
namespace pattern {
uintptr_t ScanRange(uintptr_t base, size_t size, const char* pat);
uintptr_t ScanModule(HMODULE mod, const char* pat);
int CountModule(HMODULE mod, const char* pat);
}

// 与 font_hooks.cpp / render.cpp 保持一致的特征码
static const char* kSigMeasure  = "8B 4C 24 1C 83 EC 2C 56 57 33 FF 3B CF 74";
static const char* kSigDraw     = "81 EC E0 00 00 00 55";
static const char* kSigBake     = "81 EC A4 00 00 00 53 55 56 57 8B F9 E8";
static const char* kSigLoadXml  = "81 EC F8 00 00 00 53 55 56 8B E9 E8";
static const char* kSigFontCtor = "0F 57 C0 33 C0 56 8B F1 89 46 04 8D 4E 08 89 0E 88 01 89 46 10 8D 4E 14";
// 渲染器原语（this 结构体偏移已通配，否则 ROC 上会 MISS）
static const char* kSigReserve  = "56 8B F1 8B 86 ?? ?? ?? ?? 83 C0 04 3D A0 0F 00 00 72 ?? E8 ?? ?? ?? ?? 8B 8E ?? ?? ?? ?? 8D 14 CD";
static const char* kSigFlush    = "51 53 8B D9 8B 83 ?? ?? ?? ?? 85 C0";

// 期望值（回归用；换游戏版本时这些会变，工具会报出来）
struct Expect { const char* tag; uint32_t measure, draw, bake, loadXml, fontCtor, reserve, flush; };
static const Expect kExpect[] = {
    { "hta.exe",         0x685990, 0x685CA0, 0x8B9350, 0x8B80B0, 0x8B6740, 0x7B0110, 0x7AFC50 },
    { "Meridian113.exe", 0x690A10, 0x690D20, 0x7492D0, 0x747D40, 0x746350, 0x71F350, 0x71F0F0 },
};

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

// 与 font_hooks.cpp 中 FindAllInRange 相同
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

static const char* BaseName(const char* p) {
    const char* s = p;
    for (const char* q = p; *q; ++q) if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}

static const Expect* FindExpect(const char* path) {
    const char* bn = BaseName(path);
    for (int i = 0; i < (int)(sizeof(kExpect)/sizeof(kExpect[0])); ++i)
        if (lstrcmpiA(bn, kExpect[i].tag) == 0) return &kExpect[i];
    return NULL;
}

static int TestOne(const char* path) {
    printf("======================================================================\n");
    printf("%s\n", path);
    HMODULE mod = MapImageForTest(path);
    if (!mod) return 1;
    const Expect* ex = FindExpect(path);

    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    printf("  ImageBase=0x%08X (实际映射 0x%08X)  SizeOfImage=0x%X\n",
           (unsigned)nt->OptionalHeader.ImageBase, (unsigned)(uintptr_t)mod,
           (unsigned)nt->OptionalHeader.SizeOfImage);

    int fail = 0;
    struct Row { const char* name; uintptr_t got; int hits; uint32_t exp; };
    Row rows[8]; int nr = 0;

    const char* names4[] = { "measure 文本度量", "bake 运行时烘图", "loadXml XML加载", "fontCtor 字体构造" };
    const char* pats4[]  = { kSigMeasure, kSigBake, kSigLoadXml, kSigFontCtor };
    uint32_t    exps4[]  = { ex?ex->measure:0, ex?ex->bake:0, ex?ex->loadXml:0, ex?ex->fontCtor:0 };
    uintptr_t got4[4] = {0,0,0,0};
    for (int i = 0; i < 4; ++i) {
        got4[i] = pattern::ScanModule(mod, pats4[i]);
        rows[nr].name = names4[i]; rows[nr].got = got4[i];
        rows[nr].hits = pattern::CountModule(mod, pats4[i]); rows[nr].exp = exps4[i];
        ++nr;
    }

    // draw 消歧
    {
        uintptr_t cands[16];
        int nc = FindAll(mod, kSigDraw, cands, 16);
        uintptr_t best = 0;
        if (nc > 0) {
            if (got4[0]) {
                for (int i = 0; i < nc; ++i) {
                    if (cands[i] <= got4[0]) continue;
                    if (!best || cands[i] < best) best = cands[i];
                }
            }
            if (!best) best = cands[0];
        }
        rows[nr].name = "draw 文本绘制(消歧)"; rows[nr].got = best; rows[nr].hits = nc;
        rows[nr].exp = ex ? ex->draw : 0;
        ++nr;
    }

    for (int i = 0; i < nr; ++i) {
        // 比较必须在 RVA 空间进行：SEC_IMAGE 映射的实际基址未必等于
        // PreferredBase（ASLR/占用都会改变它），绝对地址不可比，RVA 才是稳定量。
        uintptr_t gotRva = rows[i].got ? (rows[i].got - (uintptr_t)mod) : 0;
        bool ok = rows[i].got != 0;
        if (ex && rows[i].exp) {
            uint32_t expRva = rows[i].exp - 0x400000;   // 期望值以 preferred base 给出
            ok = (rows[i].got != 0) && (gotRva == expRva);
        }
        if (!ok) ++fail;
        printf("  %s %-24s hits=%-2d RVA=0x%06X", ok ? "OK  " : "FAIL",
               rows[i].name, rows[i].hits, (unsigned)gotRva);
        if (ex && rows[i].exp) printf("  expect RVA=0x%06X", (unsigned)(rows[i].exp - 0x400000));
        printf("\n");
    }

    // 字形表容量指令
    if (got4[3]) {
        uintptr_t cap = pattern::ScanRange(got4[3], 0x100, "68 00 01 00 00");
        if (!cap) ++fail;
        printf("  %s %-24s got=0x%08X (fontCtor+0x%X)\n",
               cap ? "OK  " : "FAIL", "字形表容量 push 0x100",
               (unsigned)cap, (unsigned)(cap ? cap - got4[3] : 0));
    } else ++fail;

    // 渲染器原语（替绘路径需要）
    {
        struct RP { const char* name; const char* pat; uint32_t exp; };
        RP rp[2] = {
            { "reserve 预留顶点", kSigReserve, ex ? ex->reserve : 0 },
            { "flush   冲刷批次", kSigFlush,   ex ? ex->flush   : 0 },
        };
        for (int i = 0; i < 2; ++i) {
            uintptr_t g = pattern::ScanModule(mod, rp[i].pat);
            int h = pattern::CountModule(mod, rp[i].pat);
            bool ok = g != 0;
            if (ex && rp[i].exp) ok = (g != 0) && ((g - (uintptr_t)mod) == (rp[i].exp - 0x400000));
            if (!ok) ++fail;
            printf("  %s %-24s hits=%-2d RVA=0x%06X", ok ? "OK  " : "FAIL",
                   rp[i].name, h, (unsigned)(g ? g - (uintptr_t)mod : 0));
            if (ex && rp[i].exp) printf("  expect RVA=0x%06X", (unsigned)(rp[i].exp - 0x400000));
            printf("\n");
        }
    }

    // 反汇编器自检
    if (got4[0]) {
        const uint8_t* p = (const uint8_t*)got4[0];
        int total = 0; bool ok = true;
        for (int k = 0; k < 8; ++k) {
            lde::Insn in;
            if (!lde::Decode(p + total, 16, &in) || in.len <= 0) { ok = false; break; }
            total += in.len;
        }
        if (!ok) ++fail;
        printf("  %s %-24s 8 条指令共 %d 字节\n", ok ? "OK  " : "FAIL", "ldisasm 自检", total);
    }

    UnmapViewOfFile(mod);
    return fail;
}

int main(int argc, char** argv) {
    printf("hta_chs 特征码扫描验证工具 (tools/sigtest)\n\n");
    if (argc < 2) { printf("用法: sigtest.exe <exe1> [exe2 ...]\n"); return 2; }
    int fail = 0;
    for (int i = 1; i < argc; ++i) fail += TestOne(argv[i]);
    printf("\n======================================\n");
    printf("结果: %s (%d 项未达预期)\n", fail ? "有失败" : "全部通过", fail);
    return fail ? 1 : 0;
}
