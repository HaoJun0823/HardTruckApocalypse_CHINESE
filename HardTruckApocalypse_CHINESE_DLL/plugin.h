// plugin.h —— Hard Truck Apocalypse / Rise of Clans 中文字体插件
//
// 目标：在不修改 hta.exe / Meridian113.exe 的前提下，让引擎显示 GBK 双字节中文。
//
// 引擎的硬约束（已逆向确认，见 README_DLL.md）：
//   * Font+0x40 是字形表 vector<void*>，**恰好 256 项**，索引 = 零扩展单字节 × 4
//   * 绘制 sub_685CA0 / 度量 sub_685990 逐字节推进（add esi,1 / add edi,1），无 DBCS 判定
//   * fonts.xml 的 <Symbol value=...> 在 strlen!=1 时被拒（"invalid symbol name"）
//   * 烘图 sub_8B9350 用 GDI，逐字节遍历全局 off_A05E38（0x20..0xFF）
//
// 因此必须：扩表 → 让加载器接受 2 字节 → 让遍历按前导字节跳步 → 让烘图处理双字节。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define PLUGIN_TAG "HTA_CHS"

// 编译期断言：本插件只支持 32 位目标进程
#if defined(_WIN64)
#error "本插件必须编译为 Win32 (x86)，游戏是 32 位进程"
#endif

// ===========================================================================
// 日志
// ===========================================================================
void LogOpen(HMODULE self);
void LogClose();
void Logf(const char* fmt, ...);

// ===========================================================================
// 轻量长度反汇编器（用于 trampoline 挂钩）
// ===========================================================================
namespace lde {

struct Insn {
    int len;      // 指令总长度
    int relOff;   // 相对位移在该指令内的字节偏移；-1 表示无
    int relSize;  // 1 或 4；0 表示无相对位移
};

// 解码一条 x86 指令。成功返回 true 并填充 out。
bool Decode(const uint8_t* code, int maxLen, Insn* out);

} // namespace lde

// ===========================================================================
// 特征码扫描
// ===========================================================================
namespace pattern {

// 在模块的可执行段内扫描。"8B 57 40 0F B6 ?? ??"，?? 为通配字节。
// 返回绝对地址；未命中返回 0。
uintptr_t ScanModule(HMODULE mod, const char* pat);

// 在指定内存范围内扫描（地址为绝对 VA）。
uintptr_t ScanRange(uintptr_t base, size_t size, const char* pat);

// 统计命中数量；addrs 可为空。
int CountModule(HMODULE mod, const char* pat);

} // namespace pattern

// ===========================================================================
// 内联挂钩（trampoline）
// ===========================================================================
namespace hook {

// 在 target 处安装跳转，原指令搬入 trampoline 并跳回。
// 成功后 *trampolineOut 可直接调用（等价于原函数）。
bool Install(const char* name, uintptr_t target, void* detour, void** trampolineOut);

// 撤销全部挂钩。
void UninstallAll();

} // namespace hook

// ===========================================================================
// 字体引擎
// ===========================================================================
namespace font {

// 引擎里的字形结构（48 字节）
//
// 偏移已逐指令核实：
//   sub_685CA0 @0x6865A2-0x6865B5  索引：movzx ebp,bl; add ebp,ebp; add ebp,ebp; mov eax,[edx+ebp]
//                                  （注意是两次 add 而非 shl，用 shl 搜不到）
//   sub_685CA0 @0x6865C7           [glyph+0x24]/[glyph+0x28] 读入 xmm 作 UV/纹理对
//   sub_685CA0 @0x686A34           [glyph+0x2C] = advance
//   sub_66FE10                     a2[0]=*(g+0x24); a2[1]=*(g+0x28)
//   sub_66FEA0                     return *(float*)(*(this+16)+4*byte) + 0x2C
#pragma pack(push, 1)
struct Glyph {
    char  ch;        // +0x00  原始（单）字节
    float abcA;      // +0x04
    float width;     // +0x08
    float abcC;      // +0x0C
    int32_t page;    // +0x10  图集页号
    float u0;        // +0x14
    float v0;        // +0x18
    float u1;        // +0x1C
    float v1;        // +0x20
    uint32_t tex0;   // +0x24  纹理/UV 对（被 sub_66FE10 取出）
    uint32_t tex1;   // +0x28
    float advance;   // +0x2C  总步进 = abcA + width + abcC
};
#pragma pack(pop)

// Font 对象中我们关心的字段偏移
enum {
    FONT_OFF_HEIGHT      = 0x18,  // float 像素高
    FONT_OFF_ATLAS_VEC   = 0x30,  // vector<texId> 图集页表 _Myfirst
    FONT_OFF_GLYPH_VEC   = 0x40,  // vector<void*> 字形表 _Myfirst
};

// 引擎字形表初始容量（Font 构造里 push 0x100 决定）
enum { GLYPH_TABLE_SIZE_ORIG = 256 };

// 绘制路径里被劫持的字节常量（必须避开，否则会误触发转义）
enum {
    DRAW_ESC_AT   = 0x40,   // '@' 8 字节 hex 颜色转义（会吃掉后续 8 字节）
    DRAW_STYLE_HASH = 0x23, // '#'
    DRAW_STYLE_DOLLAR = 0x24, // '$'
    DRAW_STYLE_AMP = 0x26,  // '&'
    DRAW_STYLE_PIPE = 0x7C, // '|'
    DRAW_CTRL_LIMIT = 0x20, // < 0x20 一律被静默跳过（零宽、不查字形）
};

// 安装全部字体挂钩。返回成功定位/安装的数量。
int InstallHooks(HMODULE gameModule);

// 定位结果查询（未定位到的返回 0）
uintptr_t GetMeasure();
uintptr_t GetDraw();
uintptr_t GetBakeAtlas();
uintptr_t GetLoadXml();
uintptr_t GetFontCtor();
uintptr_t GetTableCapSite();

// 供挂钩内部使用：把 GBK 双字节解成索引
inline uint32_t MakeIndex(uint8_t lead, uint8_t trail) {
    return ((uint32_t)lead << 8) | trail;
}

// 是否是 GBK 前导字节（用 GetCPInfoExA 填充的表，回退到范围判断）
bool IsLeadByte(uint8_t b);

// 初始化前导字节表
void InitLeadByteTable();

} // namespace font

// ===========================================================================
// 文本转码挂钩
//
// 钩 String3d 赋值原语（sub_406F50），在拷贝完成后对目标缓冲做**就地**转码。
// 因为 GBK 双字节 -> [转义][槽位] 是等长的，所以不需要额外缓冲，
// 也不涉及长度/容量/所有权问题。
// ===========================================================================
namespace texthook {

// 安装（需 slotmap 已加载）。返回定位到的目标数。
int Install(HMODULE gameModule);

// 就地转码，返回替换的汉字数
int TranscodeInPlace(char* p, size_t len);

uintptr_t GetAssignTarget();

// 供外部调用：对一段可写字符串转码（用于运行时硬编码文本）
int TranscodeString(const char* sz);

} // namespace texthook

// ===========================================================================
// 字形槽位映射表
//
// 映射必须与离线烘字库时一致，否则字库里的字形与运行时算出的槽号对不上。
// 所以映射表由 fontgen 在烘图时一并产出，插件只加载与查询，不做运行时分配。
// ===========================================================================
namespace slotmap {

// 加载映射表（文本格式，见 slotmap.cpp 注释）
bool Load(const char* path);

// 查询：GBK 双字节 → 槽号
bool Lookup(uint16_t gbk, uint8_t* outSlot);

int  Count();
bool IsLoaded();

} // namespace slotmap

// ===========================================================================
// GBK → 引擎单字节序列 的转换层
//
// 引擎读一个字节就查 Font+0x40[字节]。汉字 2 字节会被拆成两个无关字形
// （这就是现在乱码的原因）。转换层把它转成 [转义][槽位] 两字节，
// 于是引擎**原有的度量与绘制循环一行都不用改**：
//   度量 sub_685990 @0x685A8B 与绘制 sub_685CA0 @0x6864E3 都是
//       cmp al, 20h / jnb ...     ; >=0x20 才查字形表
//   即 <0x20 的字节被静默跳过、零宽度、不查字形表。
// 所以转义字节取 0x1B，天然不可见，也不需要图集里有对应字形。
// ===========================================================================
namespace transcode {

// 把 GBK 串转到 dst（输出 [转义][槽位] 序列）。
// 返回 false 表示缓冲不足。无映射的汉字用 '?' 兜底。
bool GbkToSlots(const char* src, char* dst, size_t dstCap);

// 该字节能否安全用作槽号（避开绘制路径劫持的字节）
bool IsUsableSlot(uint8_t b);

// 判断一个字节串里是否含任何需要转码的 GBK 双字节序列。
// 用于绘制/度量的 ASCII 快速路径：全 ASCII 时直接走原函数，零回归风险。
bool NeedsTranscode(const char* s);

// 转义字节 / 每字号可用槽数
size_t EscapeByte();
int    SlotsPerFont();

// 自检：校验槽位区间与转义字节的合法性
bool SelfTest(char* errBuf, size_t errCap);

} // namespace transcode

// ===========================================================================
// 路径 D：把字形索引拓宽到 16 位（支持 65536 个字形）
//
// 引擎字形表 Font+0x40 是 256 项、按单字节索引，扣掉 ASCII/标点后只剩
// 约 72 个槽位给汉字。完整汉化需要上千个，所以必须拓宽索引本身。
//
// 编码：ASCII 单字节 -> 索引 = 字节；GBK 双字节 -> 索引 = b1|(b2<<8)。
// 两组索引不重叠（ASCII ≤ 0xFF，汉字 ≥ 0x4081），前 256 项沿用现有字形。
//
// 汉字包文件存在才启用；不存在则自动回退到 transcode 的单字节槽位方案。
// 详见 PATH_D_DESIGN.md。
// ===========================================================================
namespace pathd {

// 初始化。返回 true 表示路径 D 已启用（此时**不要**启用 transcode 转码，
// 因为字符串必须保持原始 GBK 让引擎按 16 位查表）。
bool Init(HMODULE game, const char* pkgPath);

bool IsEnabled();

} // namespace pathd

// ===========================================================================
// 渲染器原语
//
// 已逆向确认（见 README.md）：
//   * dword_A0B55C          = 引擎 App 单例；+0x2FC = IRenderer*（vtable 在 dxrender9.dll）
//   * dword_A13CC0          = 字体管理器单例；+0x510 = 当前绑定图集页缓存（-1 = 无）
//   * sub_685CA0 **不设置**任何渲染器状态：FVF/stride/顶点数组都由调用方
//     sub_687AF0 在进入前建立。因此 detour 入口即继承一个配置正确的渲染器。
//
// 顶点格式（28 字节，D3DFVF_XYZRHW|DIFFUSE|TEX1）：
//   +0 float x   +4 float y   +8 float z=0   +12 float rhw=0.5f
//   +16 DWORD diffuse(ARGB)   +20 float u   +24 float v
//
// 顶点缓冲：计数器 this+0x8AEC0（顶点数），数组 this+0x58240，stride 28
// ===========================================================================
namespace render {

// 定位渲染器原语（用特征码）。返回找到的数量。
int Locate(HMODULE gameModule);

// 与 sub_7B0110 等价：预留 4 个顶点，返回顶点数组基址（满时会自动冲刷）
typedef void* (__thiscall *ReserveVertsFn)(void* self);
typedef void  (__thiscall *FlushFn)(void* self);

// 取引擎 App 单例 / 字体管理器单例（运行时指针）
void* AppSingleton();
void* FontManagerSingleton();

// IRenderer* （= App + 0x2FC）
void* Renderer();

// 当前 Font*（= 字体管理器 + 0x4A8）
void* CurrentFont();

// 让引擎的图集页缓存失效（我们自己绑页前必须调用）
void InvalidatePageCache();

// 顶点结构
#pragma pack(push, 1)
struct Vertex {
    float    x, y, z, rhw;
    uint32_t diffuse;
    float    u, v;
};
#pragma pack(pop)
enum { kVertexStride = 28, kVertexPerGlyph = 4 };

// 已定位的原语地址（0 表示未找到）
uintptr_t AddrReserveVerts();
uintptr_t AddrFlushBatch();

// 预留 4 个顶点并返回基址（满时引擎会自动冲刷）
void* ReserveVerts(void* textCtx);

// 冲刷顶点批次
void FlushBatch(void* textCtx);

// 发射一个字形 quad（4 顶点两三角）。textCtx 是原 sub_685CA0 的 this。
bool EmitGlyphQuad(void* textCtx, float x, float y, float w, float h,
                   uint32_t diffuse,
                   float u0, float v0, float u1, float v1);

// 绑定我们自己的图集页
void BindTexturePage(const int* texId);

// 建纹理并上传 BGRA 像素
bool CreateAndUploadTexture(const char* name, int w, int h,
                            const uint32_t* bgraPixels, int* outTexId);

} // namespace render
