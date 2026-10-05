// font_hooks.cpp —— 字体引擎定位与挂钩
//
// 已逆向确认的引擎结构（hta.exe v1.03 与 Meridian113.exe 同源，但地址与
// 部分寄存器分配不同）：
//
//   Font 对象：
//     +0x18 float 像素高
//     +0x30 vector<texId> 图集页表      (begin/end/cap)
//     +0x40 vector<void*> 字形表        (begin/end/cap)  ← 初始恰好 256 项
//
//   字形 (48 字节)：
//     +0  char  原始字节
//     +4  float abcA        +8  float width      +12 float abcC
//     +16 int   图集页号
//     +20 float u0          +24 float v0
//     +28 float u1          +32 float v1
//     +36 uint 纹理句柄对 (8 字节)
//     +44 float 总步进 = abcA + width + abcC
//
//   关键函数：
//     sub_685990  文本度量/排版  逐字节 `add edi,1`
//     sub_685CA0  文本绘制       逐字节 `add esi,1` / `add edi,1`
//     sub_8B9350  运行时 GDI 烘图（遍历全局 off_A05E38 = 0x20..0xFF）
//     sub_8B80B0  Font::CreateFromXmlNode（strlen(value)!=1 即拒绝）
//     sub_8B6740  Font 构造（此处 push 0x100 分配 256 项字形表）
#include "pch.h"
#include "plugin.h"

namespace font {

namespace {

// ---------------------------------------------------------------------------
// 前导字节表
// ---------------------------------------------------------------------------
bool g_leadByte[256] = { false };
bool g_leadInit = false;

// ---------------------------------------------------------------------------
// 扫描到的目标
// ---------------------------------------------------------------------------
struct Targets {
    uintptr_t measure;    // 文本度量
    uintptr_t draw;       // 文本绘制
    uintptr_t bakeAtlas;  // 运行时 GDI 烘图
    uintptr_t loadXml;    // Font::CreateFromXmlNode
    uintptr_t fontCtor;   // Font 构造
};

Targets g_t = { 0, 0, 0, 0, 0 };

// Font 构造里 push 0x100 的地址（字形表容量）
uintptr_t g_fontTableCapSite = 0;

} // namespace

// ---------------------------------------------------------------------------
// 前导字节
// ---------------------------------------------------------------------------
bool IsLeadByte(uint8_t b) {
    if (g_leadInit) return g_leadByte[b];
    return b >= 0x81 && b <= 0xFE;   // 未初始化时的保守回退
}

void InitLeadByteTable() {
    if (g_leadInit) return;
    memset(g_leadByte, 0, sizeof(g_leadByte));

    CPINFOEXA cp;
    ZeroMemory(&cp, sizeof(cp));
    UINT cpId = 936;                  // GBK
    if (!GetCPInfoExA(cpId, 0, &cp)) {
        cpId = CP_ACP;
        if (!GetCPInfoExA(cpId, 0, &cp)) {
            Logf("font: GetCPInfoExA 失败，使用范围回退");
            g_leadInit = true;
            return;
        }
    }
    for (int i = 0; i < MAX_LEADBYTES && cp.LeadByte[i]; i += 2) {
        uint8_t lo = cp.LeadByte[i], hi = cp.LeadByte[i + 1];
        for (int b = lo; b <= hi && b <= 255; ++b) g_leadByte[b] = true;
    }
    g_leadInit = true;

    int n = 0;
    for (int i = 0; i < 256; ++i) if (g_leadByte[i]) ++n;
    Logf("font: 前导字节表已建立 (cp=%u, %d 个前导字节)", cpId, n);
}

// ---------------------------------------------------------------------------
// 特征码
//
// 全部已用 tools/sigtest.exe 对 hta.exe 与 Meridian113.exe 验证。
//
// 两个二进制的差异：draw 的寄存器分配不同（HTA 把 arg0 存到 ebp，
// ROC 存到 [esp+40h]），没有公共的长序言。因此 draw 先做宽松匹配，
// 再取“位于 measure 之后、距离最近”的那个命中来消歧。
// ---------------------------------------------------------------------------
namespace {

const char* kSigMeasure  = "8B 4C 24 1C 83 EC 2C 56 57 33 FF 3B CF 74";
const char* kSigDraw     = "81 EC E0 00 00 00 55";
const char* kSigBake     = "81 EC A4 00 00 00 53 55 56 57 8B F9 E8";
const char* kSigLoadXml  = "81 EC F8 00 00 00 53 55 56 8B E9 E8";
const char* kSigFontCtor = "0F 57 C0 33 C0 56 8B F1 89 46 04 8D 4E 08 89 0E 88 01 89 46 10 8D 4E 14";

// 穷举某特征码在模块可执行段内的全部命中
int FindAllInRange(HMODULE mod, const char* pat, uintptr_t* out, int maxOut) {
    auto dos = (const IMAGE_DOS_HEADER*)mod;
    auto nt  = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t sb = (uintptr_t)mod + sec->VirtualAddress;
        size_t    ss = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        uintptr_t cur = sb;
        uintptr_t end = sb + ss;
        while (cur < end && n < maxOut) {
            uintptr_t hit = pattern::ScanRange(cur, (size_t)(end - cur), pat);
            if (!hit) break;
            out[n++] = hit;
            cur = hit + 1;
        }
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// 查询接口
// ---------------------------------------------------------------------------
uintptr_t GetMeasure()      { return g_t.measure;   }
uintptr_t GetDraw()         { return g_t.draw;      }
uintptr_t GetBakeAtlas()    { return g_t.bakeAtlas; }
uintptr_t GetLoadXml()      { return g_t.loadXml;   }
uintptr_t GetFontCtor()     { return g_t.fontCtor;  }
uintptr_t GetTableCapSite() { return g_fontTableCapSite; }

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
int InstallHooks(HMODULE gameModule) {
    InitLeadByteTable();

    int found = 0;

    struct One { const char* name; const char* pat; uintptr_t* dst; };
    One simple[] = {
        { "measure  文本度量",   kSigMeasure,  &g_t.measure   },
        { "bake     运行时烘图",  kSigBake,     &g_t.bakeAtlas },
        { "loadXml  XML加载",    kSigLoadXml,  &g_t.loadXml   },
        { "fontCtor 字体构造",   kSigFontCtor, &g_t.fontCtor  },
    };
    for (int i = 0; i < (int)(sizeof(simple) / sizeof(simple[0])); ++i) {
        int hits = pattern::CountModule(gameModule, simple[i].pat);
        uintptr_t addr = pattern::ScanModule(gameModule, simple[i].pat);
        *simple[i].dst = addr;
        if (addr) {
            ++found;
            Logf("font: %-18s -> 0x%08X  (命中 %d 处)", simple[i].name, (unsigned)addr, hits);
            if (hits != 1) Logf("font:   警告: 命中数不为 1");
        } else {
            Logf("font: %-18s -> 未找到", simple[i].name);
        }
    }

    // draw 消歧
    {
        uintptr_t cands[16];
        int nc = FindAllInRange(gameModule, kSigDraw, cands, 16);
        g_t.draw = 0;
        if (nc > 0) {
            if (g_t.measure) {
                uintptr_t best = 0;
                for (int i = 0; i < nc; ++i) {
                    if (cands[i] <= g_t.measure) continue;
                    if (!best || cands[i] < best) best = cands[i];
                }
                g_t.draw = best;
            }
            if (!g_t.draw) g_t.draw = cands[0];
            if (g_t.draw) ++found;
            Logf("font: %-18s -> 0x%08X  (命中 %d 处，取 measure 之后最近者)",
                 "draw     文本绘制", (unsigned)g_t.draw, nc);
        } else {
            Logf("font: draw     文本绘制    -> 未找到");
        }
    }

    // 字形表容量指令：fontCtor 内的 `68 00 01 00 00`（push 0x100）
    if (g_t.fontCtor) {
        uintptr_t hit = pattern::ScanRange(g_t.fontCtor, 0x100, "68 00 01 00 00");
        g_fontTableCapSite = hit;
        Logf("font: 字形表容量指令 -> 0x%08X (fontCtor+0x%X)",
             (unsigned)hit, hit ? (unsigned)(hit - g_t.fontCtor) : 0);
        if (!hit) Logf("font:   警告: 未找到 push 0x100，扩容无法进行");
    }

    Logf("font: 共定位 %d/5 个目标", found);
    return found;
}

} // namespace font
