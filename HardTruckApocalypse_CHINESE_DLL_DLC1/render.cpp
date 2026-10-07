// render.cpp —— 渲染器原语定位与封装
//
// 我们计划整体替换文本绘制 sub_685CA0（它不设置任何渲染器状态，由调用方
// sub_687AF0 在进入前配好 FVF/stride/顶点数组），改为自己发射 CJK 字形 quad。
// 为此需要这几个引擎原语。
//
// 已逆向确认的关键事实：
//   * dword_A0B55C = 引擎 App 单例；+0x2FC = IRenderer*（vtable 在 dxrender9.dll）
//   * dword_A13CC0 = 字体管理器单例；+0x4A8 = 当前 Font*；+0x510 = 当前绑定的图集页缓存
//   * sub_7B0110(this) 预留 4 个顶点并返回基址（满 4000 顶点时自动冲刷）
//   * sub_7AFC50(this) 冲刷顶点批次
//   * IRenderer vtable 槽：+0x3B4 建纹理、+0x3E8 准备、+0x3F0 上传、
//                          +0x3CC 绑定纹理页
//
// 顶点格式 28 字节：{x, y, z=0, rhw=0.5f, diffuse(ARGB), u, v}
// 顶点数组 this+0x58240，计数器 this+0x8AEC0，stride 28
#include "pch.h"
#include "plugin.h"

namespace render {

namespace {

// 引擎全局
const uintptr_t kAppSingletonPtr   = 0xA0B55C;   // CApplication*
const uintptr_t kFontMgrPtr        = 0xA13CC0;   // 字体管理器*

// 偏移
const uint32_t  kOffRenderer       = 0x2FC;      // App + 0x2FC = IRenderer*
const uint32_t  kOffCurrentFont    = 0x4A8;      // 字体管理器 + 0x4A8 = Font*
const uint32_t  kOffPageCache      = 0x510;      // 字体管理器 + 0x510 = 当前页缓存

// IRenderer vtable 槽（字节偏移）
const uint32_t  kSlotCreateTexture = 0x3B4;
const uint32_t  kSlotBindPage      = 0x3CC;
const uint32_t  kSlotUpload        = 0x3F0;

uintptr_t g_reserve = 0;
uintptr_t g_flush   = 0;

// 这些全局在 EXE 的 .data 段，是「指针的地址」，需要解引用
inline void** AppSlot() { return (void**)kAppSingletonPtr; }
inline void** FontMgrSlot() { return (void**)kFontMgrPtr; }

} // namespace

// ---------------------------------------------------------------------------
// 定位
//
// 特征码把结构体偏移（顶点计数器 this+0x8AEC0、数组 this+0x58240）通配掉：
// 两个 exe 的 this 结构偏移不同（ROC 的顶点缓冲在别的偏移），硬编码偏移
// 会导致 ROC 上 MISS。通配后两边都唯一命中。
// ---------------------------------------------------------------------------
int Locate(HMODULE gameModule) {
    int n = 0;

    // sub_7B0110: push esi; mov esi,ecx; mov eax,[esi+OFF]; add eax,4; cmp eax,0FA0h
    g_reserve = pattern::ScanModule(gameModule,
        "56 8B F1 8B 86 ?? ?? ?? ?? 83 C0 04 3D A0 0F 00 00 72 ?? E8 ?? ?? ?? ?? 8B 8E ?? ?? ?? ?? 8D 14 CD");
    if (g_reserve) { ++n; Logf("render: 预留顶点 sub_7B0110 -> 0x%08X", (unsigned)g_reserve); }
    else           Logf("render: 预留顶点 未找到");

    // sub_7AFC50: push ecx; push ebx; mov ebx,ecx; mov eax,[ebx+OFF]; test eax,eax
    g_flush = pattern::ScanModule(gameModule,
        "51 53 8B D9 8B 83 ?? ?? ?? ?? 85 C0");
    if (g_flush) { ++n; Logf("render: 冲刷批次 sub_7AFC50 -> 0x%08X", (unsigned)g_flush); }
    else         Logf("render: 冲刷批次 未找到");

    return n;
}

uintptr_t AddrReserveVerts() { return g_reserve; }
uintptr_t AddrFlushBatch()   { return g_flush;   }

// ---------------------------------------------------------------------------
// 单例访问
// ---------------------------------------------------------------------------
void* AppSingleton() {
    void** slot = AppSlot();
    return slot ? *slot : nullptr;
}

void* FontManagerSingleton() {
    void** slot = FontMgrSlot();
    return slot ? *slot : nullptr;
}

void* Renderer() {
    void* app = AppSingleton();
    if (!app) return nullptr;
    return *(void**)((uint8_t*)app + kOffRenderer);
}

void* CurrentFont() {
    void* fm = FontManagerSingleton();
    if (!fm) return nullptr;
    return *(void**)((uint8_t*)fm + kOffCurrentFont);
}

void InvalidatePageCache() {
    void* fm = FontManagerSingleton();
    if (!fm) return;
    *(int32_t*)((uint8_t*)fm + kOffPageCache) = -1;
}

// ---------------------------------------------------------------------------
// 顶点预留（转发到引擎原语）
// ---------------------------------------------------------------------------
void* ReserveVerts(void* textCtx) {
    if (!g_reserve) return nullptr;
    ReserveVertsFn fn = (ReserveVertsFn)g_reserve;
    return fn(textCtx);
}

void FlushBatch(void* textCtx) {
    if (!g_flush) return;
    FlushFn fn = (FlushFn)g_flush;
    fn(textCtx);
}

// ---------------------------------------------------------------------------
// 写一个字形 quad（4 个顶点，两三角）
//
// textCtx : 文本上下文（原 sub_685CA0 的 this），顶点缓冲宿主
// x, y    : 屏幕坐标
// w, h    : 字形尺寸
// diffuse : ARGB 颜色
// u0,v0,u1,v1 : 图集 UV
// ---------------------------------------------------------------------------
bool EmitGlyphQuad(void* textCtx, float x, float y, float w, float h,
                   uint32_t diffuse,
                   float u0, float v0, float u1, float v1) {
    void* base = ReserveVerts(textCtx);
    if (!base) return false;

    Vertex* v = (Vertex*)base;
    // 左上
    v[0].x = x;     v[0].y = y;     v[0].z = 0.0f; v[0].rhw = 0.5f;
    v[0].diffuse = diffuse; v[0].u = u0; v[0].v = v0;
    // 右上
    v[1].x = x + w; v[1].y = y;     v[1].z = 0.0f; v[1].rhw = 0.5f;
    v[1].diffuse = diffuse; v[1].u = u1; v[1].v = v0;
    // 左下
    v[2].x = x;     v[2].y = y + h; v[2].z = 0.0f; v[2].rhw = 0.5f;
    v[2].diffuse = diffuse; v[2].u = u0; v[2].v = v1;
    // 右下
    v[3].x = x + w; v[3].y = y + h; v[3].z = 0.0f; v[3].rhw = 0.5f;
    v[3].diffuse = diffuse; v[3].u = u1; v[3].v = v1;
    return true;
}

// ---------------------------------------------------------------------------
// 绑定我们自己的图集页
//
// 引擎原逻辑（sub_685CA0 @0x686907-0x686942）：
//     ecx = IRenderer*
//     edx = [ecx]                      ; vftable
//     fstp qword [esp]  = -1.0         ; arg4/arg3
//     eax = &Font->pages[glyph->page]  ; arg2
//     push 0                           ; arg1
//     call [edx+3CCh]
// 我们照搬这个调用形状，但传入自己的 texId 指针。
// ---------------------------------------------------------------------------
typedef void (__thiscall *BindPageFn)(void* renderer, int stage, const int* texId, double param);

void BindTexturePage(const int* texId) {
    void* r = Renderer();
    if (!r) return;

    // 先让引擎的页缓存失效，强制它真的重新绑定
    InvalidatePageCache();

    void** vt = *(void***)r;
    BindPageFn fn = (BindPageFn)vt[kSlotBindPage / 4];

    // 参数顺序与引擎一致：stage=0, &texId, param=-1.0
    // x86 thiscall：this 在 ecx，其余从右向左入栈 → param, texId, stage
    // 注意 double 占 8 字节栈空间
    fn(r, 0, texId, -1.0);
}

// ---------------------------------------------------------------------------
// 建纹理 + 上传像素
// ---------------------------------------------------------------------------
typedef int  (__thiscall *CreateTextureFn)(void* renderer, int* outTexId,
                                           const char* name, int w, int h, int fmt);
typedef int  (__thiscall *UploadFn)(void* renderer, int* texId,
                                    int w, int h, const void* pixels, int bpp, int flags);

bool CreateAndUploadTexture(const char* name, int w, int h,
                            const uint32_t* bgraPixels, int* outTexId) {
    void* r = Renderer();
    if (!r || !outTexId) return false;
    void** vt = *(void***)r;

    CreateTextureFn create = (CreateTextureFn)vt[kSlotCreateTexture / 4];
    if (create(r, outTexId, name, w, h, 4) < 0) return false;

    UploadFn upload = (UploadFn)vt[kSlotUpload / 4];
    return upload(r, outTexId, w, h, bgraPixels, 4, 0) != 0;
}

} // namespace render
