// pathd.cpp —— 路径 D：让单字节渲染引擎显示 GBK 双字节汉字
//
// ═══════════════════════════════════════════════════════════════════════════
// 为什么必须这么做
// ═══════════════════════════════════════════════════════════════════════════
// 引擎字形表 Font+0x40 是 256 项、按**单字节**索引：
//     0x6865A9  0F B6 EB    movzx ebp, bl      ; ★ 只读 1 字节
//     0x6865AC  03 ED       add ebp, ebp
//     0x6865AE  03 ED       add ebp, ebp       ; ×4
//     0x6865B0  8B 04 2A    mov eax, [edx+ebp]
// 扣掉 ASCII/标点/西里尔，能腾给汉字的只有约 72 个 —— 完整汉化需要上千个。
//
// ═══════════════════════════════════════════════════════════════════════════
// ★★★ 索引字节序（历史 bug 的根源，改动前务必先读这段）★★★
// ═══════════════════════════════════════════════════════════════════════════
//     ASCII/拉丁  单字节 c          -> 索引 = c              (0x0000..0x00FF)
//     汉字        GBK 双字节 b1 b2  -> 索引 = (b1<<8)|b2      (0x8140..0xFEFE)
//
//   注意是 **(b1<<8)|b2**，也就是 **GBK 原值本身**，不是 b1|(b2<<8)。
//   两者差一次字节序颠倒，FillCjk 和 P4 必须与之一致，否则整屏错字。
//
//   幸运的巧合：b1 天然落在 0x81..0xFE，**永远不会**是 0x23/0x24/0x26/0x40/0x7C
//   这些被绘制路径劫持的转义字节，所以双字节序列不会被误判为转义。
//
// ═══════════════════════════════════════════════════════════════════════════
// 实际采用的架构（⚠ 与早期设计文档不同，别照那份改）
// ═══════════════════════════════════════════════════════════════════════════
//   早期设想是「原地把 movzx 改成读 16 位」（P1/P4 十字节内联编码），
//   实际落地的是 **call 注入 + naked 助手**，原因见下面三条硬约束：
//     1. 补丁点只有 9~18 字节，放不下完整的双字节判定；
//     2. 需要按 Font* 选表（见「每字体独立表」），而 Font* 只在栈上；
//     3. 引擎要求「除 eax/ecx/ebp 外寄存器原样保留」，C++ 助手做不到。
//
// ═══════════════════════════════════════════════════════════════════════════
// 每字体独立汉字表（修复「所有字号互相覆写」）
// ═══════════════════════════════════════════════════════════════════════════
//   旧实现所有字号共用一张 64K 表，而 SetupThread 是按字号依次填充的，
//   后填的字号会覆盖先填的 —— 每个汉字都拿到**别的字号**的字形，
//   于是 pxW/pxH/advance 全错。
//   改为 g_cjkSlots[]（最多 16 槽，每槽 64K 项）懒分配，一字体一表。
//
// ═══════════════════════════════════════════════════════════════════════════
// 补丁点（IDA 逐字节核实，hta.exe；运行时不改 exe 文件）
// ═══════════════════════════════════════════════════════════════════════════
//   sub_685CA0 文本绘制（0x685CA0..0x686B07）：
//     P4  0x6865A6  18 字节  主查表   原 8B 57 40 0F B6 EB 03 ED 03 ED 8B 04 2A 85 C0 8D 0C 2A
//     P4b 0x686A26  10 字节  第二处查表（原 8B 44 24 34 8B 48 40 8B 04 29）
//     P5  0x686A52   9 字节  主遍历   原 0F 57 C0 83 C6 01 83 C7 01
//   另有 4 处跳转目标需同步重定位 0x686A55 -> 0x686A52：
//     0x6864ED / 0x686507 / 0x68658B / 0x68659C
//   sub_685CA0 预扫循环（P2/P3）：
//     0x68620D / 0x686221  各 5 字节，call 条件推进助手
//   sub_685990 文本度量：
//     P7 0x0044ED34  ★ 故意不补丁 ★（3 字节放不下条件判断，硬补会破坏 ASCII）
//
// ═══════════════════════════════════════════════════════════════════════════
// ★★★ 引擎契约（P4/P4b/P5 全部适用，违反即崩）★★★
// ═══════════════════════════════════════════════════════════════════════════
//   · 除 eax / ecx / ebp 外，**所有寄存器必须原样保留**。
//     bl 只是 ebx 的低字节 —— 任何 C++ 函数都可能改写它。
//   · **绝不在这些助手里用 call 调 C++**：实测一次 call 就让全部文字消失。
//   · ebp 出口 = 索引*4；ecx = 槽地址；成功时 edx = 字体表。
//   · 栈偏移一律读 ModRM/SIB 的 disp 字节；IDA 的 stack_frame 表不可信。
//     经验公式：call-entered helper 实际位移 = 引擎偏移 + 4 + 4*pushes。
// ═══════════════════════════════════════════════════════════════════════════

#include "pch.h"
#include "plugin.h"
#include <vector>
#include <tlhelp32.h>

// ⚠ 这两个指针必须放在**全局作用域**：下面的 naked asm 里用
//   jmp dword ptr [PathD_PrepassTopA]  引用它们，
//   若放在匿名命名空间里，汇编器会当成未定义标号（C2094）。
extern "C" void* PathD_PrepassTopA = nullptr;   // P2 预扫分支的循环头
extern "C" void* PathD_PrepassTopB = nullptr;   // P3 预扫分支的循环头

// ★ 路径 D 的汉字字形表：**完全由我们自己持有**，65536 项。
//   为什么不用字体自己的表：字体表只有 256 项，而 16 位索引会越过它读到
//   未初始化内存（实测取到 0x1C，随后解引用崩溃）。而且字体何时构造、
//   表何时扩容都不在我们控制内（首帧绘制就早于字体创建）。
// ★★★ 汉字表在下面 g_cjkSlots[] 里按字体懒分配。
//   这里原本还有一张全局共享的 g_cjkTable，已删除：SetupThread 按字号依次
//   填充，后填的字号会覆盖先填的，所有汉字都拿到错误尺寸的字形。

// ★★★ 就绪标志（⚠ 已不参与渲染判据，仅供诊断）★★★
//
//   历史：这里原本是「全部装完才置 1」的总闸门，P4 查它来决定
//   返不返回字形。代价是「进入界面要等 8 秒才出中文」。
//   现在改成**每槽独立 ready**（见 CjkSlot），第一个字号填完就能显示。
//   本变量只在装配线程收尾时置 1，用于日志区分「装配中/已完成」。
//
//   ★ 原始设计意图（保留供追溯，防有人再踩回去）★
//   补丁是**立即生效**的（引擎一进绘制循环就用上了），而装配要等
//   SetupThread 完成，两者天然有竞态。转储 hta.exe0041 实证：
//       第1次  0x686A2F  EAX=1352328C   ← P4 返回了这个指针
//       第2次  0x663C50A5                ← 引擎解引用它时炸了
//   EAX 是个"看起来像堆指针"的值，但那一格还没初始化完。
//   ⇒ **未就绪一律返回 NULL**，让引擎 `test eax,eax / jz` 安全跳过
//     整个字形（那一帧不显示），绝不能返回半成品指针。
extern "C" volatile LONG g_cjkReady = 0;

// ★★★ Font* 自注册表：绘制时自动发现字体，不再靠枚举全局变量 ★★★
//
//   为什么放弃「找字体管理器全局变量」（实测都失败）：
//     dword_A0A88C  引擎核心 UI 对象（运行时值 0xA0A890），+1188 处 count 恒为 0
//     dword_A13CC0  被 sub_41BF90 / sub_432650 等几十个**非 UI** 函数引用
//     两个都不是 FontManager。
//
//   ★ 可靠来源：绘制循环本身 ★
//     0x6865A2  mov edi, [esp+0F0h+var_BC]   ← edi = Font*
//     0x6865A6  mov edx, [edi+40h]            ← 我们覆盖的起点
//   每个要绘制的字符都会经过这里，edi 就是**活的** Font*。
//   把它们登记下来，等价的字体集合自动到手 —— 不依赖任何全局变量。
#define MAX_SEEN_FONTS 64
struct SeenFont { void* p; volatile LONG slot; };
// ★ 定义必须用 extern "C" + 存储类，否则 P4 的裸汇编按 C++ 名修饰找不到符号
extern "C" { SeenFont g_seenFonts[MAX_SEEN_FONTS]; volatile LONG g_seenCount; }

// ═══════════════════════════════════════════════════════════════════════════
// 每字体独立的汉字表（修复缺陷 #4：10 个字号互相覆写同一张 64K 表）
//
//   ★★★ 根因 ★★★
//     旧实现只有**一张** g_cjkTable（65536 项 = 256KB），而 SetupThread
//     是按字号**依次**填充的：
//         [12.000] 填充 2207 个汉字字形 …
//         [10.000] 填充 …
//         …
//         [15.625] 填充 …      ← 最后写入，全表归它
//     于是 12 号字体的排版上下文里，每个汉字拿到的都是 15.625 号的
//     字形指针 —— pxW/pxH/advance 全部偏大，字符被撑宽、基线被压低。
//     这是「字形尺寸错乱」的真凶，与 P4b 无关。
//
//   ★ 代价 ★
//     MAX_CJK_TABLES × 256KB。取 16 槽 = 4MB；实际只有约 10 个字号
//     真正用得上，其余留空不分配（懒分配），实测占用约 2.5MB。
//     游戏地址空间 2GB，4MB 可忽略。
//
//   ★ 为什么不用 map/vector ★
//     P4 是裸汇编助手，要按 Font* O(1)~O(16) 找到表。线性扫 16 项
//     只是几条指令，且**绝不分配内存**，绝不会在绘制途中失败。
#define MAX_CJK_TABLES 16
struct CjkSlot {
    void*     font;      // 该槽归属的 Font*
    uint32_t* table;     // 65536 项；未分配时为 0
    volatile LONG ready; // 1 = 该表已填完，可放心查询
};
extern "C" {
    CjkSlot     g_cjkSlots[MAX_CJK_TABLES];
    volatile LONG g_cjkSlotCount;
}

// P4 的查表助手（替换 0x6865A9 处 10 字节）。
// 入口：esi=串下标  eax=串基址  edx=[Font+0x40](字体自带表)  bl=当前字符
// 出口：eax=字形指针  ecx=存放该指针的槽地址（后续代码只读不写）
//       ebp=索引*4
// 除 eax/ecx/ebp 外全部寄存器必须原样保留。
extern "C" void __cdecl PathD_GlyphLookup();
extern "C" void __cdecl PathD_GlyphLookup2();
// P4 的裸汇编要 call 它取本字体的汉字表。**返回类型必须与定义一致**
// （早先这里声明成 void，定义成 uint32_t*，MSVC 直接拒绝并把后面
//  所有符号的解析带偏，报出一堆「kFontOffPages 未声明」之类的假错误）。
// ★ 声明与定义的 `__cdecl` 写法必须一致，否则 MSVC 报 C2143/C2447 ★
//   而 P4 的裸汇编按 C 名修饰找符号。x86 下 __cdecl 是**默认**调用约定，
//   所以两边都**不写** __cdecl 最省事、也最不容易写错。
//   （试过 extern "C" __cdecl 与 __cdecl extern "C" 两种写法，
//     MSVC 14.51 两种都拒绝，只有省略 __cdecl 才通过。）
extern "C" uint32_t* PathD_CjkTableFor(void* font);
extern "C" uint32_t* g_lastFontTable = nullptr;   // 助手1 记下的字体表基址

// ★★★ P4 → P4b 的字形传递（带字符校验）★★★
//   P4（每轮迭代的取字处）把查到的字形存进 g_curGlyph，同时把**当前字符**
//   bl 存进 g_curChar；P4b 取 g_curGlyph，但**必须**先确认 g_curChar == bl。
//
//   ★ 为什么必须有字符校验（纯缓存会画错字）★
//     0x686A13（喂 P4b 的 `cmp bl,20h`）的唯一前驱是 0x686A11 —— 那是
//     0x6869F0 那个 4 轮 flush 循环的循环尾，由 0x686728/0x6868C1 进入，
//     **整条路径都没经过 P4**，缓存是陈的，会画出别的字符。
//
//   ★ 为什么**不能**让 P4b 自己解引用 var_BC（自查找方案已否决）★
//     实测崩溃（hta.exe0079）：flush 路径上 var_BC 不是有效 Font*，
//     `mov ecx,[edi+40h]` 读 0x419FBEC0 直接 AV。
//     EDI=0x419FBE80 / EAX=0x50 —— 三种区间判据（0x10000 / 堆区 / -1）
//     全都拦不住它。⇒ P4b 只能**读缓存**，不得碰 var_BC / var_9C。
//
//   ★ 校验失败返回 0 的代价 ★
//     引擎 0x686A32 `jz 0x686A3B` -> 推进量记 0，flush 那几轮不推进，
//     画面上最多少几个字符的宽度；不崩、不画错字。
extern "C" uint32_t g_curGlyph = 0;        // P4 查到的字形指针
extern "C" int32_t  g_curChar  = -1;       // 该字形对应的字符（bl），-1 = 无效
// ★ A 方案的加强版：flush 路径**根本不会**把 g_p4Hit 清 0（它不经过 P4），
//   所以单靠这个标志挡不住陈旧值。真正可靠的是 g_curChar 校验，
//   它在 P4 失败和 flush 两条路上都保证 g_curChar != bl。
//   这里保留它是为了「P4 成功但字形随后失效」时能立刻发现。
extern "C" int32_t  g_p4Hit    = 0;        // 本轮 P4 是否成功

// 注：曾经为定位崩溃加过的一批诊断（g_dbgLookups / LogFontProbe /
//     LogLookupDbg / LogLookup2Dbg）已全部移除 —— 它们完成了使命，
//     而且 LogLookupDbg 里 `*(unsigned*)slot` 在 slot 无效时会自己崩，
//     反而干扰定位。现在的策略是：崩溃一律看转储 + 补丁落地自检。

namespace pathd {

namespace {

// ─────────────────────────────────────────────────────────────────────────
// 状态
// ─────────────────────────────────────────────────────────────────────────
bool     g_enabled   = false;   // 路径 D 是否启用（包文件存在才启用）
uintptr_t g_modBase  = 0;

// 取字函数（用于 P8/P9 等长改宽度）
uintptr_t g_advGet   = 0;       // sub_66FEA0
uintptr_t g_uvGet    = 0;       // sub_66FE10

// 引擎自带的 vector::resize(this, newSize, value)  __thiscall
typedef unsigned int (__thiscall *VecResizeFn)(void* self, unsigned int n, int value);
VecResizeFn g_vecResize = nullptr;
bool        g_vecResizeOk = false;   // 自检通过才敢用

// ★★★ 引擎堆分配器 —— 修复退出崩溃与堆破坏的关键 ★★★
//
//   背景：AppendPages 原来用 VirtualAlloc 给字体的**页表**(Font+0x30)分配
//   新缓冲，然后整体替换 vector 的 _Myfirst/_Mylast/_Myend。
//   引擎 ~Font 析构时会用自己的 free 去释放 _Myfirst ——
//   而 VirtualAlloc 出来的内存**绝对不能**被 free，于是崩在
//       0x748EF2  cmp dword ptr [edx-4], 0xDEADBEEF     (0xC0000005)
//   这就是 hta.exe0081 / 0092 两个转储里同一个地址的由来。
//
//   引擎自己的堆（IDA 实证）：
//     sub_748DC0 = alloc  块头写 0xDEADBEEF，返回 ptr+12 处的用户指针
//     sub_748EE0 = free   校验 [ptr-4] == 0xDEADBEEF
//     sub_748FA0 = realloc（先 alloc 再拷再 free，证明两者配对）
//     sub_589410 = alloc 的公开包装，__fastcall(ecx=size, edx, stack)，
//                  内部取 dword_A0A880 作为管理器 this
//
//   用引擎的 alloc 分配页表，free 就能正确配对，堆破坏消失。
//
//   ★ 签名说明（逐字节核实 0x589410）★
//       8B 44 24 04   mov eax,[esp+arg_0]
//       50            push eax        ← 第 3 参数
//       52            push edx        ← 第 2 参数
//       51            push ecx        ← 第 1 参数（= size）
//       8B 0D ?? ?? ?? ??  mov ecx, dword_A0A880
//       E8 ?? ?? ?? ??     call sub_748DC0
//       C2 04 00      retn 4          ← 被调用方清栈 4 字节
//     => __fastcall 带 1 个栈参数；ecx 是 size，edx 与栈参数在小块
//        （<1024）路径下不使用，传 0 即可。
typedef void* (__fastcall *EngineAllocFn)(uint32_t size, void* a2, int a3);
static EngineAllocFn g_engineAlloc = nullptr;
// 0x589410 的特征码（a0a880 与 call 目标用 ?? 通配）
#define SIG_ENGINE_ALLOC "8B 44 24 04 50 52 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C2 04 00"
bool        g_skipPatch  = false;    // HTA_CHS_NO_PATCH=1  时跳过补丁（二分定位用）
bool        g_skipExpand = false;    // HTA_CHS_NO_EXPAND=1 时跳过扩表（二分定位用）
bool        g_skipP5     = false;    // HTA_CHS_NO_P5=1   跳过 P5 主遍历助手
bool        g_skipP4b    = false;    // HTA_CHS_NO_P4B=1  跳过 P4b 第二处查表助手
bool        g_skipScan   = false;    // HTA_CHS_NO_SCAN=1 跳过堆扫描（对照实验用）
bool        g_hookFont   = false;    // 是否安装 Font::CreateFromXmlNode 钩子
                                       // ★ 默认关：实测该钩子破坏 esi 导致崩溃，
                                       //   而且它从未成功找到过 CJK 图集页。

// 字体管理 / 取字体
// ★ 字体管理器 = dword_A0A88C 的**内容**（见 FontArrayOf 上方的实证链）。
//   IDA 地址 0xA0A88C，本 exe 无 ASLR / ImageBase 固定 0x400000，
//   但仍然在运行时按模块基址 + RVA 计算，不写死绝对地址。
#define RVA_EngineCore   0x00060A88Cu        // dword_A0A88C
static void* g_fontMgr   = nullptr;          // 初始化时 = *(base + RVA_EngineCore)
uintptr_t g_getFontByH  = 0;    // sub_679700(manager, int index) -> Font*
uintptr_t g_getCurFont  = 0;    // sub_679710(manager) -> Font*
typedef void* (__thiscall *GetFontByHFn)(void* mgr, int index);
typedef void* (__thiscall *GetCurFontFn)(void* mgr);

// ★★ 字体数组的最终定位（IDA 全链实证，别再猜）★★
//
//   证据链：
//     1. sub_6843F0（ui_Srv.cpp 的 SetSchema）加载字体：
//            0x684E44  mov edx, [esp+var_5C]
//            0x684E48  mov ecx, [edx+4A4h]   ← ★ FontManager = *(obj+0x4A4) ★
//            0x684E4E  call sub_8BA480
//        而 sub_6843F0 全程以 dword_A0A88C 为「obj」（vtable+32/36 都是它）。
//     2. sub_8BA480 -> sub_8B9B60（LoadFonts），遍历每个 Item：
//            v11 = sub_8B6740(...)          // new Font
//            sub_8B80B0(v11, ...)            // CreateFromXmlNode
//            sub_8B66F0(this, &v23)         // ★ 插进容器 ★
//     3. sub_8B66F0 的容器布局（IDA 实证）：
//            this+4 = begin / this+8 = end / this+12 = cap
//            `*(_DWORD*)((this+8) - 4)` 取插入位置，写入后 end+=4
//        → 元素是 4 字节的 Font*
//
//   ★ 所以正确算法是：
//       FontMgr   = *(uint32_t*)((uint8_t*)dword_A0A88C + 0x4A4)
//       vec       = (Vec3*)((uint8_t*)FontMgr + 4)
//       第 i 个   = vec->begin[i]
//
//   之前两次失败的原因：
//     · dword_A13CC0 —— 被 sub_41BF90/sub_432650 等几十个非 UI 函数引用
//     · dword_A0A88C 直接当 FontMgr，offset 也没对（应为 +0x4A4 再 +4）
struct Vec3 { uint32_t* begin, *end, *cap; };
static Vec3* GetFontVector(void* uiCore) {
    if (!uiCore) return nullptr;
    void* mgr = *(void**)((uint8_t*)uiCore + 0x4A4);
    if (!mgr) return nullptr;
    return (Vec3*)((uint8_t*)mgr + 4);         // 容器本体：begin/end/cap
}

// 已创建的 Font 列表（由 sub_8B80B0 入口钩子收集）
struct FontRec { void* font; float height; };
std::vector<FontRec> g_fonts;
CRITICAL_SECTION     g_cs;

// 助手（naked asm 实现，见文件末尾）
extern "C" void __cdecl PathD_DrawAdvance();      // P5 用
extern "C" void __cdecl PathD_PrepassAdvanceA();  // P2 用（字形存在分支）
extern "C" void __cdecl PathD_PrepassAdvanceB();  // P3 用（字形为空分支）
extern "C" void __cdecl PathD_MeasureAdvance();   // P7 用

// ─────────────────────────────────────────────────────────────────────────
// 包文件（由 fontgen/build_cjk.py 产出）
//
//   Header 48 字节：
//     u32 magic='HCJK'; u32 version; u32 sizeCount; u32 glyphCount;
//     u32 pageW, pageH;               // 所有图集页统一尺寸
//     u32 reserved[7];
//   Glyph codes: glyphCount × u16     // GBK 双字节码，升序
//   每个字号一条记录：
//     f32 height;                     // 对应原 fonts.xml 里该字号的 height
//     u32 pageCount;                  // 该字号用了几页
//     u32 cols;                       // 每行几格
//     u32 cellsPerPage;               // 每页几格
//     u32 cellW, cellH;               // 单元像素尺寸（≈em）
//     u32 reserved;
//     u32 cellIndex[glyphCount];      // 该字号的格位（页号/行列由此推导）
// ─────────────────────────────────────────────────────────────────────────
const uint32_t kMagic = 0x4B4A4348;   // "HCJK"

struct Package {
    uint32_t sizeCount = 0, glyphCount = 0;
    uint32_t pageW = 0, pageH = 0;
    std::vector<uint16_t> codes;      // GBK 码（存的是 (b1<<8)|b2）
    struct SizeRec {
        float height = 0;
        uint32_t pageCount = 0, cols = 0, cellsPerPage = 0, cellW = 0, cellH = 0;
        std::vector<uint32_t> cell;
    };
    std::vector<SizeRec> sizes;
    bool loaded = false;
};
Package g_pkg;

// ─────────────────────────────────────────────────────────────────────────
// 小工具
// ─────────────────────────────────────────────────────────────────────────
inline uint8_t rd8(uintptr_t a) { return *(uint8_t*)a; }

// 读 4 字节并按**有符号**解释（跳转 rel32 用）。
// 注意：必须自己拼字节，不能直接 *(int32_t*)a —— hta.exe 的代码段在
// 0x400000，未对齐读虽然 x86 能转，但 MSVC 在 /O2 下允许对齐假设，
// 拆成 4 个字节读更保险。
inline int32_t rd32s(uintptr_t a) {
    uint32_t v = (uint32_t)rd8(a)
               | ((uint32_t)rd8(a + 1) << 8)
               | ((uint32_t)rd8(a + 2) << 16)
               | ((uint32_t)rd8(a + 3) << 24);
    return (int32_t)v;
}

bool BytesMatch(uintptr_t a, const uint8_t* pat, const char* mask, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (mask[i] == '?') continue;
        if (rd8(a + i) != pat[i]) return false;
    }
    return true;
}

std::string HexDump(uintptr_t a, size_t n) {
    char buf[256] = {0}; size_t p = 0;
    for (size_t i = 0; i < n && p + 4 < sizeof(buf); ++i)
        p += _snprintf_s(buf + p, sizeof(buf) - p, _TRUNCATE, "%02X ", rd8(a + i));
    return std::string(buf);
}

// 改内存保护并写字节
bool WriteBytes(uintptr_t addr, const uint8_t* data, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        Logf("pathd: VirtualProtect 失败 0x%08X (%lu)", (unsigned)addr, GetLastError());
        return false;
    }
    memcpy((void*)addr, data, n);
    VirtualProtect((LPVOID)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)addr, n);
    return true;
}

// 计算 rel32 并写入 E8 (call)
bool WriteCall(uintptr_t at, void* target, const char* what) {
    int64_t rel = (int64_t)target - (int64_t)(at + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: %s 目标 0x%p 距离过远，无法 E8 相对调用", what, target);
        return false;
    }
    uint8_t b[5];
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)rel;
    if (!WriteBytes(at, b, 5)) return false;
    Logf("pathd:   %s @0x%08X -> call 0x%p (rel=%d)", what, (unsigned)at, target, (int)rel);
    return true;
}

// ── 补丁写入：冻结全部线程后一次性写入，消除"半更新状态"窗口 ────────────
//
// ★ 为什么必须这样做（hta.exe0077 / 0078 转储实证）★
//   之前每个补丁点分两次写：先 WriteCall 写 5 字节 E8 rel32，再单独
//   WriteBytes 补 NOP。两次调用之间是两次独立的 VirtualProtect+memcpy，
//   中间有 20~40ms 的窗口。而引擎此时**已经在渲染主菜单**，渲染线程正好
//   落进这个窗口，执行"新 call + 旧尾部字节"这个半更新状态：
//
//     0078 P4b @0x686A26  转储实测: E8 75 F7 8D 70 | 48 40 8B 04 29
//          call 返回后落到 0x686A2B，旧字节解码 = dec eax / inc eax /
//          mov eax,[ecx+ebp]，ecx=0、ebp=0x2F0 → 读 0x2F0（= 崩溃现场）
//     0077 P5  @0x686A52  转储实测: E8 19 F6 8D 70 | 01 83 C7 01
//          call 返回后落到 0x686A57，旧字节解码 = add [ebx+743B01C7],eax，
//          ebx=039C3E40 → 写 0x77D74007（= 崩溃现场）
//
//   两次转储的 CreationTime 都落在写入的那一刻（0078 转储 35.332 vs
//   写入日志 35.330）。跳过 P5 后崩溃立刻转移到 P4b ⇒ 每个点各自触发
//   一次，是**确定性**竞争，不是随机破坏。
//
//   修法（Detours 的标准做法）：
//     1) 先 VirtualProtect 改页属性（线程还在跑，不影响）
//     2) Toolhelp32 快照本进程全部线程 -> SuspendThread
//     3) GetThreadContext 校验 EIP **不在** [at, at+n) 内
//        —— 落在区间内说明正处在半更新处，解冻重试
//     4) 通过则单次 memcpy 整块写入 -> 恢复页属性 -> 全部 ResumeThread
//   这样任何线程都不可能观察到中间态。
//
//   ★ 冻结区间内**绝对不能调用 Logf** ★ 若被冻结的线程正持有日志的
//   CRT/文件锁，我们再写日志就会死锁。日志一律放在冻结之前或之后。
bool WriteBlockSafe(uintptr_t at, const uint8_t* buf, size_t n, const char* what) {
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)at, n, PAGE_EXECUTE_READWRITE, &old)) {
        Logf("pathd: [%s] VirtualProtect 失败 0x%08X (%lu)", what, (unsigned)at, GetLastError());
        return false;
    }

    const DWORD self = GetCurrentThreadId();

    // ── 线程句柄只快照一次，重试时复用 ──────────────────────────────
    //   原写法每次重试都 CreateToolhelp32Snapshot + OpenThread 全部线程。
    //   游戏有几十个线程，40 次重试 = 上千次系统调用，是"补丁安装耗时
    //   11 秒"的主因之一。线程集合在运行期基本不变，缓存即可。
    DWORD tids[512]; int tidN = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                if (te.th32ThreadID == self) continue;
                if (tidN >= 512) break;
                tids[tidN++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    HANDLE held[512];
    int heldN = 0;
    const DWORD t0 = GetTickCount();
    const DWORD kTotalBudgetMs = 3000;      // ★ 总超时 3 秒，绝不无限等 ★
    int attempt = 0;
    bool wrote = false;
    uintptr_t blockerEip = 0;

    for (attempt = 0; attempt < 40; ++attempt) {
        heldN = 0;
        for (int i = 0; i < tidN; ++i) {
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, tids[i]);
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            held[heldN++] = th;
        }

        // ── 校验：只拒绝"指令跨边界"的情况 ──────────────────────────
        //   ★ 这是本次最重要的修正 ★
        //   旧判据把 [at-15, at+n) 全算"忙"。但 P4/P4b/P5 补丁的正是
        //   绘制函数的**热点**，渲染线程大部分时间 EIP 就在这段里 ——
        //   于是每次校验都 busy，40 次重试全白费，补丁安装卡十几秒
        //   甚至永远写不进去（实测日志停在"原字节"再无输出）。
        //
        //   ★ 真正危险的只有一种 ★
        //     EIP ∈ [at-15, at)：该线程正停在一条**跨越 at 边界**的指令
        //     上，补丁后从 EIP 重新解码会得到完全不同的指令 —— 必崩。
        //
        //   ★ 而 EIP ∈ [at, at+n) 是安全的 ★
        //     线程已挂起，写入期间不会执行；Resume 后它从 at 处的
        //     `E8 call 助手` 开始跑，等价于"它自己调用了助手"，
        //     助手会正确返回到 at+n —— 语义正确，不是半更新状态。
        const uintptr_t loGuard = (at >= 15) ? (at - 15) : 0;
        bool busy = false;
        blockerEip = 0;
        for (int i = 0; i < heldN && !busy; ++i) {
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(held[i], &c)) continue;
            uintptr_t eip = (uintptr_t)c.Eip;
            if (eip >= loGuard && eip < at) { busy = true; blockerEip = eip; }
        }

        if (!busy) {
            memcpy((void*)at, buf, n);
            FlushInstructionCache(GetCurrentProcess(), (LPVOID)at, n);
            wrote = true;
            break;
        }

        for (int i = 0; i < heldN; ++i) { ResumeThread(held[i]); CloseHandle(held[i]); }
        heldN = 0;
        if (GetTickCount() - t0 > kTotalBudgetMs) break;
        Sleep(1);
    }

    // ★ 统一出口：无论如何都必须恢复所有线程，否则游戏直接卡死 ★
    for (int i = 0; i < heldN; ++i) { ResumeThread(held[i]); CloseHandle(held[i]); }
    VirtualProtect((LPVOID)at, n, old, &old);

    if (wrote) {
        if (attempt) Logf("pathd:   %s 冻结重试 %d 次后写入成功", what, attempt);
        return true;
    }
    // 走到这里说明没写成。绝不静默失败 —— 否则像之前那样日志无输出、
    // 表面"没崩"实际补丁全没装上，汉字永远不显示。
    Logf("pathd: [%s] ★写入失败★ 重试 %d 次/超时 %lums，阻塞 EIP=0x%08X",
         what, attempt, (unsigned long)(GetTickCount() - t0), (unsigned)blockerEip);
    Logf("pathd:       补丁未生效，该路径的汉字不会显示（游戏不会崩）");
    return false;
}

// 把 "call 助手 + NOP 填充" 组装成单个缓冲区后**一次**写入
bool WriteCallBlock(uintptr_t at, size_t total, void* target, const char* what) {
    int64_t rel = (int64_t)target - (int64_t)(at + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: %s 目标 0x%p 距离过远，无法 E8 相对调用", what, target);
        return false;
    }
    if (total < 5)   { Logf("pathd: %s 覆盖 %u 字节 < 5，放不下 call", what, (unsigned)total); return false; }
    if (total > 64)  { Logf("pathd: %s 覆盖 %u 字节超缓冲", what, (unsigned)total); return false; }
    uint8_t b[64];
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)rel;
    for (size_t i = 5; i < total; ++i) b[i] = 0x90;
    if (!WriteBlockSafe(at, b, total, what)) return false;
    Logf("pathd:   %s @0x%08X -> call 0x%p (rel=%d, 共 %u 字节一次写入)",
         what, (unsigned)at, target, (int)rel, (unsigned)total);
    return true;
}

// 按特征码在模块里找唯一命中；通配 '?'
uintptr_t ScanUnique(const char* sig, const char* what, bool required = true) {
    uintptr_t hit = pattern::ScanModule((HMODULE)g_modBase, sig);
    if (!hit) {
        if (required) Logf("pathd: [MISS] %s  特征码 %s", what, sig);
        else          Logf("pathd: [可选缺失] %s", what);
        return 0;
    }
    // 报告命中数（帮助判断是否需要消歧）
    int n = pattern::CountModule((HMODULE)g_modBase, sig);
    Logf("pathd: %-22s -> 0x%08X  (%d 处命中)", what, (unsigned)hit, n);
    return hit;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1) 载入包文件
// ═══════════════════════════════════════════════════════════════════════════
static bool LoadPackage(const char* path) {
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        Logf("pathd: 未找到包文件 %s （回退到路径 C 单字节槽位方案）", path);
        return false;
    }
    DWORD sz = GetFileSize(hf, NULL);
    if (sz == INVALID_FILE_SIZE || sz < 48 || sz > 256u * 1024 * 1024) {
        CloseHandle(hf);
        Logf("pathd: 包文件大小异常 %lu", sz);
        return false;
    }
    std::vector<uint8_t> buf(sz);
    DWORD got = 0;
    BOOL ok = ReadFile(hf, buf.data(), sz, &got, NULL);
    CloseHandle(hf);
    if (!ok || got != sz) { Logf("pathd: 读取包文件失败"); return false; }

    const uint8_t* p = buf.data();
    uint32_t magic = *(const uint32_t*)p;
    if (magic != kMagic) {
        Logf("pathd: 包文件 magic 不符 (0x%08X != 0x%08X)", magic, kMagic);
        return false;
    }
    uint32_t ver = *(const uint32_t*)(p + 4);
    g_pkg.sizeCount    = *(const uint32_t*)(p + 8);
    g_pkg.glyphCount   = *(const uint32_t*)(p + 12);
    g_pkg.pageW        = *(const uint32_t*)(p + 16);
    g_pkg.pageH        = *(const uint32_t*)(p + 20);

    Logf("pathd: 包文件 version=%u  字号数=%u  字形数=%u  图集页 %ux%u",
         ver, g_pkg.sizeCount, g_pkg.glyphCount, g_pkg.pageW, g_pkg.pageH);

    if (!g_pkg.sizeCount || !g_pkg.glyphCount || !g_pkg.pageW || !g_pkg.pageH) {
        Logf("pathd: 包文件字段非法");
        return false;
    }

    size_t off = 48;
    if (off + g_pkg.glyphCount * 2 > sz) { Logf("pathd: 码表越界"); return false; }
    g_pkg.codes.resize(g_pkg.glyphCount);
    memcpy(g_pkg.codes.data(), p + off, g_pkg.glyphCount * 2);
    off += g_pkg.glyphCount * 2;

    g_pkg.sizes.resize(g_pkg.sizeCount);
    for (uint32_t i = 0; i < g_pkg.sizeCount; ++i) {
        if (off + 32 > sz) { Logf("pathd: 字号记录越界"); return false; }
        auto& s = g_pkg.sizes[i];
        s.height       = *(const float*)(p + off); off += 4;
        s.pageCount    = *(const uint32_t*)(p + off); off += 4;
        s.cols         = *(const uint32_t*)(p + off); off += 4;
        s.cellsPerPage = *(const uint32_t*)(p + off); off += 4;
        s.cellW        = *(const uint32_t*)(p + off); off += 4;
        s.cellH        = *(const uint32_t*)(p + off); off += 4;
        off += 4;   // reserved
        off += 4;   // reserved
        size_t need = (size_t)g_pkg.glyphCount * 4;
        if (off + need > sz) { Logf("pathd: 字号 %u 格位表越界", i); return false; }
        s.cell.resize(g_pkg.glyphCount);
        memcpy(s.cell.data(), p + off, need);
        off += need;
        Logf("pathd:   字号 %7.3f  页数 %u  每页 %u 格  每行 %u 格  单元 %ux%u",
             s.height, s.pageCount, s.cellsPerPage, s.cols, s.cellW, s.cellH);
    }
    g_pkg.loaded = true;
    Logf("pathd: 包文件载入成功");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 2) 字形结构（48 字节）
//
//   布局三重核实：sub_8B80B0 的逐字段初始化 + sub_8B6280 的 finalize
//   + fonts.xml 的实测数据。
//
//   ★ 决定性证据（fonts.xml 实测）★
//     value='!'  abc="1.000 2.000 2.000"  tcs="0.000 0.016 0.000 0.031 0.078"
//     绘制 0x686689 起读 +0x10..+0x20 五个连续 float，其中 +0x10 单独当页号用
//     （0x6868C7 test edx,edx / jl，0x6868DE cmp edx,页数 / jge）。
//     只有「+0x10 = tcs[0] = 页号」这一种排法能让 u0..v1 落在 0..1，
//     且统计全文件 tcs[0] 恒为 0（单页字体）——证明它是页号不是纹理坐标。
//
//   +0x00 u8  ch          字符码（引擎只拿它做字典校验 sub_8B3D80）
//   +0x04 f32 abc[0]      0x8B8AE7
//   +0x08 f32 abc[1]      0x8B8AF4
//   +0x0C f32 abc[2]      0x8B8B01
//   +0x10 i32 page        ★tcs[0]★  图集页号 → Font+0x30[page]
//   +0x14 f32 u0          tcs[1]
//   +0x18 f32 v0          tcs[2]
//   +0x1C f32 u1          tcs[3]
//   +0x20 f32 v1          tcs[4]
//   +0x24 f32 pxW         finalize 写：0x8B62D8 *(_DWORD*)(*v7+36)
//   +0x28 f32 pxH         finalize 写：0x8B62E0 v9[1]
//   +0x2C f32 advance     finalize 写：0x8B6337 = abc0+abc1+abc2
//
//   ★ finalize（sub_8B6280）只遍历 off_A05E38 这个**单字节**字符集，
//     所以它永远不会碰我们的汉字 —— +0x24/+0x28/+0x2C 必须自己填。
// ═══════════════════════════════════════════════════════════════════════════
static const int kGlyphSize = 48;

// 分配一个 glyph
// ★ 原先这里试图复用引擎分配器（g_engineAlloc / DetectEngineAlloc），
//   但特征码匹配到的是数据段而非代码，且结果无人使用，已删除。
//   改为自建池：Predictable、可控、不依赖对引擎内部的猜测。
static void* AllocGlyph() {
    // 原来用 *(mgr+0x18) 当分配器 —— 实测那个地址里是 0x11ABECB4（数据段），
    // 不是代码指针，说明特征码匹配到了别处。改为自建池，不再猜引擎内部结构。
    //
    // ★ 改成按需增长 ★（hta.exe 没有 LARGE_ADDRESS_AWARE，地址空间只有 2GB，
    //   转储实测崩溃时只剩 145MB 可用 —— 一次性预留 8MB 虽然不多，
    //   但配合 g_cjkTable(256KB) + 10 个字号的页表(VirtualAlloc) 会加剧压力。
    //   实际用量：300 字 × 48 字节 = 14KB 起步，按需翻倍足够。
    static uint8_t* pool = nullptr;
    static size_t   used  = 0;
    static size_t   cap   = 0;
    // ★★★ 2207 字形 × 10 字号 = 22070 个 × 48 字节 = 1,059,360 字节 ★★★
    //   起步就直接给足 2MB，让**任何一次分配都不需要搬迁**。
    const size_t kInit = 2u * 1024 * 1024;      // ≈ 43000 个字形，有余量
    if (used + kGlyphSize > cap) {
        size_t ncap = cap ? cap * 2 : kInit;
        uint8_t* np = (uint8_t*)VirtualAlloc(NULL, ncap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!np) { Logf("pathd: 字形池扩容失败(%u 字节)", (unsigned)ncap); return nullptr; }
        // ★★★ 旧池**绝不释放** ★★★
        //   g_cjkTable 里存的是**每个字形的原始地址**，不是偏移。
        //   一旦 VirtualFree 旧池，先前填好的所有字形指针立刻悬空 ——
        //   引擎 0x6865C7 `movss xmm1,[eax+24h]`（读字形宽高）读到的
        //   就是已释放页，直接 0xC0000005。
        //   实测崩溃 hta.exe0080：EAX=0x68948340，故障地址 0x68948364 = EAX+0x24，
        //   而 0x68948340 正是被 MEM_RELEASE 掉的旧池地址。
        //   泄漏上限 ~2MB（装满 2207×10 只需 ~1.06MB），换取指针永久有效。
        if (pool) memcpy(np, pool, used);      // 只搬数据，**不 VirtualFree**
        Logf("pathd: 字形池 %uKB @0x%08X（自建；旧池故意不释放，字形指针须永久有效）",
             (unsigned)(ncap / 1024), (unsigned)(uintptr_t)np);
        pool = np; cap = ncap;
    }
    void* p = pool + used;
    used += kGlyphSize;
    return p;
}


static void FillGlyph(void* g, uint16_t gbk, int page,
                      float u0, float v0, float u1, float v1,
                      float pxW, float pxH, float advance) {
    uint8_t* b = (uint8_t*)g;
    memset(b, 0, kGlyphSize);
    b[0] = (uint8_t)(gbk & 0xFF);          // 引擎只拿它做字典校验
    *(float*)(b + 0x04) = 0.0f;            // abc[0]：方块字无左伸
    *(float*)(b + 0x08) = pxW;             // abc[1]：字身宽 = 单元宽
    *(float*)(b + 0x0C) = 0.0f;            // abc[2]：无右伸
    *(int32_t*)(b + 0x10) = page;          // ★ 图集页号 ★
    *(float*)(b + 0x14) = u0;
    *(float*)(b + 0x18) = v0;
    *(float*)(b + 0x1C) = u1;
    *(float*)(b + 0x20) = v1;
    *(float*)(b + 0x24) = pxW;             // finalize 本该写，这里自己算
    *(float*)(b + 0x28) = pxH;
    *(float*)(b + 0x2C) = advance;         // = abc0+abc1+abc2
}

// ═══════════════════════════════════════════════════════════════════════════
// 3) 表扩展 / 页挂载 / 汉字填充
// ═══════════════════════════════════════════════════════════════════════════
static const int    kTableEntries = 0x10000;   // 65536
static const size_t kFontOffHeight = 0x18;     // Font+0x18 f32 height
static const size_t kFontOffPages  = 0x30;     // Font+0x30 vector<texId>
static const size_t kFontOffTable  = 0x40;     // Font+0x40 vector<glyph*>

// ── CJK 图集页的 height 编码（必须与 fontgen/build_cjk.py 保持一致）──
//     height = kCjkBase + sizeIndex * kCjkStep + pageIndex
//   ★ 步长必须 >= 单字号最大页数 ★
//     2330 字时 18.750 号要 19 页，旧步长 10 会让 si=0/pi=10 与 si=1/pi=0
//     都编码成 910，两个字号抢同一张图集 → 图集错乱。
//     步长 50 > 19 有余量；基准 500 使 si=9/pi=19 -> 969 < 1000，
//     仍落在实测可正常加载的区间（避开 >=1000 是否被接受的未知风险）。
static const float  kCjkBase = 500.0f;
static const int    kCjkStep = 50;
static const float  kCjkMax  = 999.0f;         // 页字体 height 上界

// ── 每字体汉字表的登记与查询 ──────────────────────────────────────────
//
// ★ 查得到已有槽就返回，槽满则返回 -1（调用方放弃填表，汉字就不显示，
//   但**绝不崩**——这是降级，不是失败）。
static LONG CjkSlotFor(void* font) {
    if (!font) return -1;
    for (LONG i = 0; i < g_cjkSlotCount && i < MAX_CJK_TABLES; ++i) {
        if (g_cjkSlots[i].font == font) return i;
    }
    return -1;
}

// 取得（必要时分配）该字体的表。**只在后台装配线程里调用**，
// 绘制线程只读，绝不会在渲染途中触发 VirtualAlloc。
static uint32_t* CjkTableEnsure(void* font) {
    LONG i = CjkSlotFor(font);
    if (i < 0) {
        // 占一个空槽（先占位，防止并发重复登记）
        if (g_cjkSlotCount >= MAX_CJK_TABLES) return nullptr;
        i = g_cjkSlotCount++;
        g_cjkSlots[i].font  = font;
        g_cjkSlots[i].table = nullptr;
        g_cjkSlots[i].ready = 0;
    }
    if (!g_cjkSlots[i].table) {
        size_t bytes = (size_t)kTableEntries * 4;
        void* buf = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) {
            Logf("pathd: [警告] 汉字表分配失败(%u 字节) for font %p", (unsigned)bytes, font);
            return nullptr;
        }
        memset(buf, 0, bytes);
        g_cjkSlots[i].table = (uint32_t*)buf;
    }
    return g_cjkSlots[i].table;
}

// 绘制线程用的只读查询：**绝不分配**，找不到就返回 0（引擎安全跳过）。
// 被 P4 的裸汇编直接 call，所以必须是 cdecl、无副作用、可重入。
extern "C" uint32_t* PathD_CjkTableFor(void* font) {
    LONG i = CjkSlotFor(font);
    if (i < 0) return nullptr;
    return g_cjkSlots[i].table;
}

// 记录哪些 Font 已经处理过，避免重复
static std::vector<void*>    g_doneFonts;

static float FontHeight(void* font) { return *(float*)((uint8_t*)font + kFontOffHeight); }

// 把一个 Font 的字形表扩到 65536
// ─────────────────────────────────────────────────────────────────────────
// fontCtor 建表调用的包装
//
//   引擎在 fontCtor 里这样建字形表：
//       lea ecx, [esi+3Ch]        ; ★ this = Font+0x3C（不是 +0x40！）
//       push 0                    ; 填充值
//       push 10000h               ; 容量（被 P0 补丁从 100h 改来）
//       call 0x8B6660             ; 建表 / 扩容
//
//   它按 __thiscall 调用（ecx=this，参数在栈上，被调用方清栈），所以下面用
//   **完全相同的约定**声明并调用它 —— 不再有栈错乱风险（之前就是因为把
//   调用约定猜成别的，ret 弹到垃圾地址，EIP 跑到 0）。
//
//   为什么需要包装：扩容出来的新格子**不初始化**（引擎只写它实际用到的
//   下标），于是 16 位索引在 >=0x100 处读到未初始化内存。实测读到 0x1C，
//   随后 `movss xmm1,[eax+24h]` 解引用崩溃（0xC0000005 @0x6865C7，EAX=0x1C）。
//
//   ⚠ 清零前**必须**校验真实容量，否则会把相邻堆砸烂（第 4 轮就是这么崩的）。
// ─────────────────────────────────────────────────────────────────────────
// MSVC 不允许自由函数用 __thiscall，标准做法是用 __fastcall 加一个占位参数
// 接收 EDX 来模拟：ecx=self, edx=占位, 其余参数在栈上，且由被调用方清栈 ——
// 与引擎那条 __thiscall 调用完全一致。
typedef unsigned int (__fastcall *CtorVecFn)(void* self, void* dummy, unsigned n, int value);
static CtorVecFn g_ctorVecFn = nullptr;      // 原 0x8B6660

static unsigned int __fastcall CtorTableSetup(void* self, void* dummy, unsigned n, int value) {
    unsigned int r = 0;
    if (g_ctorVecFn) r = g_ctorVecFn(self, dummy, n, value);

    // self = Font+0x3C；引擎的向量约定：[self+4]=起点, [self+8]=终点
    uint32_t base = ((uint32_t*)self)[1];    // Font+0x40
    uint32_t end  = ((uint32_t*)self)[2];    // Font+0x44
    uint32_t cnt  = (end > base) ? (end - base) / 4 : 0;

    if (base && cnt >= (uint32_t)kTableEntries && cnt < 2000000u) {
        memset((void*)base, 0, (size_t)kTableEntries * 4);
        Logf("pathd: 建表包装：容量 %u 项，已清零 %u 字节 (base=0x%08X)",
             cnt, (unsigned)((size_t)kTableEntries * 4), base);
    } else {
        Logf("pathd: 建表包装：容量只有 %u 项（期望 >= %d），**不清零**以免砸堆",
             cnt, kTableEntries);
    }
    return r;
}

static bool ExpandTable(void* font, float h) {
    // ★ 已停用：改为自持 64K 表 + 查表助手后，不再需要动字体的表。
    if (true) { (void)font; (void)h; return true; }
    uint8_t* f = (uint8_t*)font;
    uint32_t* vec = (uint32_t*)(f + kFontOffTable);
    uint32_t sizeBefore = (vec[1] && vec[0]) ? (vec[1] - vec[0]) / 4 : 0;
    uint32_t capBefore  = (vec[2] && vec[0]) ? (vec[2] - vec[0]) / 4 : 0;
    if (sizeBefore >= (uint32_t)kTableEntries) return true;

    // 先试引擎自己的扩容函数（如果它在就行，能保证用同一个分配器，析构安全）
    uint32_t capAfter = capBefore;   // 不调用引擎函数，容量不会自己变

    if (capAfter < (uint32_t)kTableEntries) {
        // ⚠⚠ 引擎的扩容函数没起作用（实测：256->65536 时 _Myfirst 纹丝不动，
        //     说明它并没有真正 reserve）。**绝对不能**在这种情况下假设后面
        //     有 256KB 可用 —— 之前就是直接 memset 256KB 把相邻堆砸烂，
        //     导致某个函数指针被清零、游戏跳到 NULL 崩溃（0xC0000005 @0）。
        //   改用自己分配的一大块内存，并整体替换 vector 的三个指针。
        Logf("pathd: [%7.3f] 引擎扩容无效，改用自分配表（256KB）", h);
        const size_t bytes = (size_t)kTableEntries * 4;
        void* buf = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) { Logf("pathd: [%7.3f] VirtualAlloc 失败", h); return false; }
        memset(buf, 0, bytes);
        // 保留原 256 项内容（Latin/西里尔字形指针）
        if (vec[0] && sizeBefore) {
            uint32_t keep = sizeBefore < 256 ? sizeBefore : 256;
            memcpy(buf, (void*)vec[0], keep * 4);
        }
        // ⚠ 注意：Font 析构时引擎会用自己的分配器 free 这个指针 —— 可能崩在退出时。
        //   记录原指针以便排查。
        Logf("pathd: [%7.3f] 原表指针 0x%08X 将被替换（析构时可能不安全，仅影响退出）",
             h, vec[0]);
        vec[0] = (uint32_t)buf;                    // _Myfirst
        vec[1] = (uint32_t)buf + (uint32_t)bytes;  // _Mylast
        vec[2] = (uint32_t)buf + (uint32_t)bytes;  // _Myend
    } else {
        // 引擎真的扩了容量：把 size 推到位并清零（reserve 不初始化新元素）
        const size_t bytes = (size_t)kTableEntries * 4;
        memset((void*)vec[0], 0, bytes);
        vec[1] = vec[0] + (uint32_t)bytes;
        vec[2] = vec[0] + (uint32_t)bytes;
    }

    uint32_t after = (vec[1] - vec[0]) / 4;
    Logf("pathd: [%7.3f] 字形表 %u -> %u 项  _Myfirst=0x%08X  _Myend=0x%08X",
         h, sizeBefore, after, vec[0], vec[2]);
    return after == (uint32_t)kTableEntries;
}

// 把 CJK 图集页挂到这个 Font 的页表（Font+0x30）末尾。
// ⚠ 本函数内不能有需要对象展开的 C++ 对象 —— 它会被包在 __try 里（否则 C2712）。
//
// ★★★ 必须用引擎自己的堆分配器，绝不能用 VirtualAlloc ★★★
//   原因：我们把 vec[0](_Myfirst) 整体替换掉了。引擎 ~Font 析构时会
//   用自己的 free 释放这个指针；VirtualAlloc 出来的内存被 free 会直接
//   崩在魔数校验上（0x748EF2 cmp [edx-4],0xDEADBEEF），
//   实测转储 hta.exe0081 / 0092 都是同一个地址。
//   改用 sub_589410（引擎 alloc 的包装）后，free 能正确配对。
//
//   ★ 为什么不再回退 VirtualAlloc ★
//     之前"分配失败就用 VirtualAlloc"的兜底，正是堆破坏的来源。
//     现在拿不到引擎分配器就**直接失败**——该字号没有汉字，
//     也比砸烂整个堆、让游戏随机崩溃要好得多。
static bool AppendPages(void* font, float h, const uint32_t* pages, int npages,
                        int* outBasePage) {
    if (!g_engineAlloc) {
        Logf("pathd: [%7.3f] 无引擎分配器，跳过挂页（该字号无汉字，但不崩）", h);
        return false;
    }
    uint8_t* f = (uint8_t*)font;
    uint32_t* vec = (uint32_t*)(f + kFontOffPages);
    uint32_t cnt = (vec[1] && vec[0]) ? (vec[1] - vec[0]) / 4 : 0;
    uint32_t newCnt = cnt + (uint32_t)npages;
    size_t bytes = (size_t)newCnt * 4;

    void* buf = g_engineAlloc((uint32_t)bytes, nullptr, 0);
    if (!buf) {
        Logf("pathd: [%7.3f] 引擎分配页表失败(%u 页, %u 字节)", h, newCnt, (unsigned)bytes);
        return false;
    }
    memset(buf, 0, bytes);
    if (vec[0] && cnt) memcpy(buf, (void*)vec[0], cnt * 4);
    for (int i = 0; i < npages; ++i)
        ((uint32_t*)buf)[cnt + i] = pages[i];

    *outBasePage = (int)cnt;
    Logf("pathd: [%7.3f] 页表 %u -> %u 页，CJK 页从索引 %u 起（引擎堆 0x%08X）",
         h, cnt, newCnt, cnt, (unsigned)(uintptr_t)buf);
    vec[0] = (uint32_t)buf;
    vec[1] = (uint32_t)buf + (uint32_t)bytes;
    vec[2] = (uint32_t)buf + (uint32_t)bytes;
    return true;
}

// 处理单个字号：扩表 + 挂页 + 填字。
// ⚠ 必须保持「无 C++ 对象」，因为它被 __try 包着（否则 C2712）。
static int FillCjk(void* font, float h, int basePage);   // 前置声明

static int ProcessFont(void* font, float h, const uint32_t* pages, int npages) {
    if (!ExpandTable(font, h)) return 0;
    int base = -1;
    if (!AppendPages(font, h, pages, npages, &base)) return 0;
    return FillCjk(font, h, base);
}

// 带异常保护的单字号处理。单独成函数是为了让含 C++ 局部对象的调用方避开 C2712。
static int SafeProcessFont(void* font, float h, const uint32_t* pages, int npages) {
    __try {
        return ProcessFont(font, h, pages, npages);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("pathd: [%7.3f] 处理异常 0x%08X（跳过该字号）", h, (unsigned)GetExceptionCode());
        return 0;
    }
}

// 为这个 Font 填充汉字字形
static int FillCjk(void* font, float h, int basePage) {
    // ★ 字形一律写进**我们自己的** 64K 表：字体表只有 256 项，
    //   往 idx>=256 写就是越界写堆（会砸坏相邻内存）。
    // ★★★ 每个字体写进**自己**的表（修复缺陷 #4）★★★
    //   旧实现所有字号共用一张 g_cjkTable，后填的字号把先填的全覆写掉 ——
    //   12 号字体的排版里拿到的是 15.625 号的字形，尺寸全错。
    uint32_t* table = CjkTableEnsure(font);
    if (!table) { Logf("pathd: [%7.3f] 本字体汉字表不可用，跳过填充", h); return 0; }
    LONG slot = CjkSlotFor(font);
    g_cjkSlots[slot].ready = 0;              // 正在填，先别让绘制线程读半成品

    // 找匹配的字号记录
    const Package::SizeRec* rec = nullptr;
    for (auto& s : g_pkg.sizes)
        if (fabsf(s.height - h) < 0.01f) { rec = &s; break; }
    if (!rec) { Logf("pathd: [%7.3f] 包文件里没有这个字号，跳过汉字填充", h); return 0; }

    const float PW = (float)g_pkg.pageW, PH = (float)g_pkg.pageH;
    const uint32_t cols = rec->cols, perPage = rec->cellsPerPage;
    const uint32_t cw = rec->cellW, chh = rec->cellH;
    float adv = (float)cw;               // 汉字等宽推进 = 单元宽

    int n = 0;
    for (uint32_t i = 0; i < g_pkg.glyphCount; ++i) {
        uint32_t cell = rec->cell[i];
        uint32_t page = cell / perPage;
        uint32_t pos  = cell % perPage;
        uint32_t col  = pos % cols;
        uint32_t row  = pos / cols;
        if ((int)page >= (int)rec->pageCount) continue;

        uint16_t gbk = g_pkg.codes[i];
        // ★★★ 索引必须与 P4 逐字节一致：(b1<<8)|b2，也就是 gbk 原值 ★★★
        //
        //   P4 的实际字节码（0x686A26 分支同款取码序列）：
        //       movzx ebp, bl            ; ebp = b1（前导字节）
        //       shl   ebp, 8             ; ebp = b1 << 8
        //       movzx ecx, byte[..+esi]  ; ecx = b2（后继字节）
        //       or    ebp, ecx           ; ebp = (b1<<8) | b2
        //   所以查表索引就是 gbk 原值，**绝不能交换字节**。
        //
        //   ★ 历史 bug（乱码根因之一）★
        //     这里曾写成 `(gbk>>8) | ((gbk&0xFF)<<8)` = b1|(b2<<8)，
        //     与 P4 恰好互为字节交换：
        //         '中' = 0xD6D0 → 这里写入 idx 0xD0D6
        //                       → P4 却查 idx 0xD6D0 → 永远取到 0
        //     于是每个汉字都"没有字形"，引擎直接跳过不画。
        //     包文件里 300 个码本就是这个索引空间，无需任何变换。
        uint32_t idx = (uint32_t)gbk;

        float u0 = (col * cw) / PW,          v0 = (row * chh) / PH;
        float u1 = ((col + 1) * cw) / PW,    v1 = ((row + 1) * chh) / PH;
        float pxW = (float)cw, pxH = (float)chh;

        void* g = AllocGlyph();
        if (!g) continue;
        FillGlyph(g, gbk, basePage + (int)page, u0, v0, u1, v1, pxW, pxH, adv);
        table[idx] = (uint32_t)g;      // ★ 写进**本字体**的表（见函数头说明）★
        ++n;
    }
    g_cjkSlots[slot].ready = 1;              // ★ 填完才开闸 ★
    Logf("pathd: [%7.3f] 填充 %d 个汉字字形（单元 %ux%u，每页 %u 格，%u 页，页基址 %d，槽 %d）",
         h, n, cw, chh, perPage, rec->pageCount, basePage, (int)slot);
    return n;
}


// ═══════════════════════════════════════════════════════════════════════════
// 5) 后台线程：等字体管理器就绪 -> 枚举字体 -> 收集 CJK 页 -> 挂页+填字
// ═══════════════════════════════════════════════════════════════════════════

// 读一个 Font 的字号（带异常保护）。单独成函数是为了让调用方避开 C2712
// （含 C++ 局部对象的函数里不能用 __try）。
static bool SafeFontHeight(void* fp, float* out) {
    __try {
        *out = FontHeight(fp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 同上：读一个 Font 已挂载的页数组（begin/end/cap）。
static int SafePageCount(void* fp, uint32_t** outVec) {
    __try {
        uint32_t* vec = (uint32_t*)((uint8_t*)fp + kFontOffPages);
        *outVec = vec;
        return (vec[0] && vec[1]) ? (int)((vec[1] - vec[0]) / 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outVec = nullptr;
        return 0;
    }
}

static std::vector<FontRec> EnumerateFonts(uint32_t** arr, int count) {
    std::vector<FontRec> out;
    for (int i = 0; i < count; ++i) {
        void* fp = (void*)arr[0][i];
        if (!fp) continue;
        float h = 0.0f;
        if (!SafeFontHeight(fp, &h)) continue;
        FontRec rec; rec.font = fp; rec.height = h;
        out.push_back(rec);
    }
    return out;
}

// ══════════════════════════════════════════════════════════════════════════
// 堆扫描：在绘制期登记的 Font* 附近找 CJK 图集页字体
//
//   ★ 为什么必须扫描堆 ★
//     FontManager 对象只活在 sub_8B9B60(LoadFonts) 的栈帧里
//     （IDA 0x8B9F2B  mov ecx,[esp+90h+var_8]），取不到；
//     而 sub_6843F0 里 `mov ecx,[edx+4A4h]` 那个 edx 并不是
//     dword_A0A88C —— 实测 dword_A0A88C+0x4A4 == 0，两条路都断了。
//
//   ★ 为什么扫描是安全的（误命中几乎不可能）★
//     只认 height ∈ [900,999] 这个引擎从不使用的区间，
//     且 name/pages/glyphTable 四项都要同时通过结构校验。
//
//   ★ 扫描范围 ★
//     不用全进程扫：CJK 字体和真实 Font 是同一批 new 出来的，
//     都在同一个堆区域。以已登记字体指针为中心上下各 64MB 即可。
//     96 个已知 Font * 16 条 fonts.xml Item，FontManager 里是同一批。
// ══════════════════════════════════════════════════════════════════════════
static bool LooksLikeCjkFont(uintptr_t a, float* outH, uint32_t* outTexId) {
    bool ok = false;
    float h = 0.0f; uint32_t tex = 0;
    __try {
        const char* nm = *(const char**)(a + 0x00);
        if (!nm || ((uintptr_t)nm & 3)) return false;
        int L = 0; while (L < 64 && nm[L] >= 0x20 && nm[L] < 0x7F) ++L;
        if (L == 0 || L >= 64 || nm[L] != 0) return false;     // 名字必须是短 ASCII
        h = *(float*)(a + 0x18);
        // ★ 放宽：CJK 页字体 height ∈ [900,999]（我们编码的），真实字号 ∈ [4,40]。
        //   之前只认 [900,999]，导致堆扫描只能捞到 CJK 页字体，真正的字号字体
        //   只能靠「绘制期登记」—— 而 45 秒内游戏只画到 2 种字号，其余 8 个字号
        //   永远装不上汉字（日志实证：「2 个字号已装入汉字」）。
        //   现在两类都收，一次堆扫描把 10 个字号全拿到。
        // ★ CJK 页字体的 height ∈ [kCjkBase, 999]（由 build_cjk.py 编码），
        //   真实字号 ∈ [4,40]。两段都收，一次堆扫描把 10 个字号全拿到。
        //   ⚠ 基准从 900 降到 500 后，这里必须同步跟着降到 500 ——
        //     否则 500~899 的页字体会被这个结构校验直接拒掉。
        if (!(h >= 4.0f && h <= 40.0f) && !(h >= kCjkBase && h <= 999.0f)) return false;
        uint32_t* pb = *(uint32_t**)(a + kFontOffPages + 0);
        uint32_t* pe = *(uint32_t**)(a + kFontOffPages + 4);
        if (!pb || !pe || pe < pb || (pe - pb) > 256) return false;
        tex = pb[0];
        void* tbl = *(void**)(a + kFontOffTable);
        if (!tbl || ((uintptr_t)tbl & 3)) return false;
        uint32_t* t0 = (uint32_t*)tbl;
        uint32_t* t1 = *(uint32_t**)(a + kFontOffTable + 4);
        if (!t1 || t1 < t0 || (t1 - t0) > 0x200000) return false;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (ok) { *outH = h; *outTexId = tex; }
    return ok;
}

static void CollectAllFonts(std::vector<FontRec>& out) {
    // ★ HTA_CHS_NO_SCAN=1 时跳过堆扫描，只用绘制期登记 ★
    //   堆扫描会在 ±2MB 范围内每 4 字节跑一次 LooksLikeCjkFont
    //   （内含 __try），20 秒超时。这本身不该吃内存，但它拖长了
    //   装配线程、推迟了闸门开启，需要能单独关掉做对照实验。
    {
        char nv[8] = {0};
        if (GetEnvironmentVariableA("HTA_CHS_NO_SCAN", nv, sizeof(nv)) > 0) {
            Logf("pathd: [调试] HTA_CHS_NO_SCAN 已设 —— 跳过堆扫描，只用绘制期登记");
        }
    }
    auto add = [&](void* fp) {
        for (auto& r : out) if (r.font == fp) return;
        float h = 0.0f;
        if (!SafeFontHeight(fp, &h)) return;
        FontRec rec; rec.font = fp; rec.height = h;
        out.push_back(rec);
    };
    // —— A) 绘制期登记到的真实字体 ——
    LONG sn = g_seenCount;
    if (sn > MAX_SEEN_FONTS) sn = MAX_SEEN_FONTS;
    std::vector<uintptr_t> anchors;
    for (LONG i = 0; i < sn; ++i) {
        add(g_seenFonts[i].p);
        if (g_seenFonts[i].p) anchors.push_back((uintptr_t)g_seenFonts[i].p);
    }
    Logf("pathd: 绘制期登记 %ld 个字体，其中 %u 个通过结构校验",
         g_seenCount, (unsigned)out.size());
    if (anchors.empty()) return;

    // —— B) 在锚点附近扫出 CJK 页字体 ——
    //   ★ 范围必须窄 ★：64MB×每 8 字节 = 800 万次 __try，实测会把
    //   装配线程卡死好几分钟（日志停在"补丁安装"，一个 Font 都没打出）。
    //   所有 Font 是同一批连续 new 出来的，锚点 ±2MB 足够覆盖。
    if (g_skipScan) {
        Logf("pathd: [调试] 跳过堆扫描 —— 只靠绘制期登记，字号可能不全");
        return;
    }
    const uintptr_t kSpan = 2u << 20;
    uintptr_t lo = 0, hi = 0;
    for (uintptr_t a : anchors) {
        uintptr_t s = (a > kSpan) ? a - kSpan : 0x10000;
        uintptr_t e = a + kSpan;
        if (lo == 0 || s < lo) lo = s;
        if (e > hi) hi = e;
    }

    int found = 0;
    uintptr_t a = lo;
    DWORD tick0 = GetTickCount();
    Logf("pathd: 堆扫描开始 0x%08X..0x%08X（%.1f MB，锚点 %u 个）",
         (unsigned)lo, (unsigned)hi, (hi - lo) / 1048576.0, (unsigned)anchors.size());
    while (a < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t end  = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect != PAGE_NOACCESS
            && mbi.Protect != PAGE_GUARD) {
            // ★★★ 快速预筛（在进 __try 之前）★★★（实测省 3.5 秒）
            //   堆扫描慢的根因是 __try 的 SEH 帧进入/退出开销，
            //   而 ±2MB 每 4 字节一次 = 约 100 万次调用。
            //   height 只是一次普通读，且本区域已由 VirtualQuery 确认
            //   MEM_COMMIT 且非 PAGE_NOACCESS/GUARD，读 p+0x18 是安全的。
            //   一次比较就能筛掉 99.9% 的候选地址。
            //
            // ⚠ 这个筛必须在**内层 for 循环体内**：放在 while 体外时
            //   continue 会作用于 while，而那里还没有 p —— 一个字节都扫不到。
            for (uintptr_t p = (base + 3) & ~3ull; p + 0x50 <= end; p += 4) {
                float hh = *(float*)(p + kFontOffHeight);
                if (!(hh >= 4.0f && hh <= 40.0f) && !(hh >= kCjkBase && hh <= 999.0f))
                    continue;
                float h = 0.0f; uint32_t tex = 0;
                if (!LooksLikeCjkFont(p, &h, &tex)) continue;
                bool dup = false;
                for (auto& r : out) if ((uintptr_t)r.font == p) { dup = true; break; }
                if (dup) continue;
                FontRec rec; rec.font = (void*)p; rec.height = h;
                out.push_back(rec);
                ++found;
            }
        }
        if (end <= base) break;
        a = end;
        // 保险：最多扫 20 秒，超时就用已经找到的
        if ((GetTickCount() - tick0) > 20000) {
            Logf("pathd: 堆扫描超时（20 秒）中断，已找到 %d 个", found);
            break;
        }
    }
    Logf("pathd: 堆扫描（%p..%p）额外找到 %d 个 CJK 页字体，合计 %u 个",
         (void*)lo, (void*)hi, found, (unsigned)out.size());
}

static DWORD WINAPI SetupThread(LPVOID) {
    Logf("pathd: 后台线程启动，等待字体就绪…");
    // 两个来源都要：
    //   A) FontManager 的 vector —— 拿 CJK 图集页字体（height>=900，
    //      它们永远不会被绘制，所以绘制期登记拿不到）
    //   B) 绘制期登记           —— 兜底
    // 等绘制登记稳定 2 秒后一次性收集。
    std::vector<FontRec> fonts;
    int lastCount = -1, stable = 0;
    // ★ 等「绘制期登记的字体数连续稳定」就够，不必死等 2 秒 ★
    //   实测：进入界面要等 8 秒才出中文，这里就占了 2 秒。
    //   fonts.xml 在启动阶段一次性加载完，实测 0.5 秒内计数就稳定了。
    //   万一没稳定就走 900 次（90 秒）上限，且漏掉的字号只是没有汉字，
    //   引擎会安全跳过 —— 不会崩。
    for (int i = 0; i < 900; ++i) {               // 最多等 90 秒
        Sleep(100);
        LONG cur = g_seenCount;
        if (cur > 0 && cur == lastCount) { if (++stable >= 5) break; }    // 稳定 0.5s
        else stable = 0;
        lastCount = (int)cur;
    }
    CollectAllFonts(fonts);
    Logf("pathd: 合计收集到 %u 个 Font", (unsigned)fonts.size());
    if (fonts.empty()) {
        Logf("pathd: [失败] 一个字体都没收集到");
        return 0;
    }
    {   // 逐个打印，方便对照引擎实际用到的字号
        char buf[64];
        for (size_t i = 0; i < fonts.size() && i < 40; ++i) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f", (double)fonts[i].height);
            Logf("pathd:   Font[%u] %p  height=%s", (unsigned)i, fonts[i].font, buf);
        }
    }

    // ── 图集页来源 ────────────────────────────────────────────────────
    // CJK 页被做成**额外的 fonts.xml Item**（height 用 kCjkBase 起编码），
    // 引擎会正常加载它们的纹理；我们按 height 解码出「字号序号 + 页序号」，
    // 再把 texId 归到对应字号名下。
    //
    //   cjkHeight = kCjkBase + sizeIndex * kCjkStep + pageIndex
    //
    // 这样做的好处：**完全不碰渲染器接口**（不用猜 createTexture /
    // uploadPixels 的签名和像素格式），复用引擎自己的纹理加载。
    std::vector<std::vector<uint32_t> > pagesBySize;   // sizeIndex -> [texId]
    int pageFonts = 0;
    for (auto& fr : fonts) {
        fr.height = FontHeight(fr.font);   // ★ 重新读：钩子入口时还没解析
        if (fr.height < kCjkBase || fr.height > kCjkMax) continue;
        ++pageFonts;
        int enc = (int)(fr.height - kCjkBase + 0.5f);
        int si = enc / kCjkStep, pi = enc % kCjkStep;
        uint8_t* f = (uint8_t*)fr.font;
        uint32_t* vec = (uint32_t*)(f + kFontOffPages);
        uint32_t cnt = (vec[1] && vec[0]) ? (vec[1] - vec[0]) / 4 : 0;
        uint32_t id = cnt ? ((uint32_t*)vec[0])[0] : 0;
        if (si >= (int)pagesBySize.size()) pagesBySize.resize(si + 1);
        if (pi >= (int)pagesBySize[si].size()) pagesBySize[si].resize(pi + 1, 0);
        pagesBySize[si][pi] = id;
        Logf("pathd: CJK 页字体 height=%.3f -> 字号序号 %d 页 %d  texId=%u (该字体共 %u 页)",
             fr.height, si, pi, id, cnt);
    }
    if (pagesBySize.empty()) {
        Logf("pathd: [失败] 没有找到任何 CJK 图集页");
        Logf("pathd:        需要在 fonts.xml 里加 height>=900 的 Item 指向 CJK 图集");
        Logf("pathd:        字形表已扩到 65536，汉字全为 NULL（会被安全跳过，不崩）");
        return 0;
    }
    {
        int tot = 0;
        for (size_t i = 0; i < pagesBySize.size(); ++i) {
            Logf("pathd: 字号序号 %u 共有 %u 个 CJK 页", (unsigned)i, (unsigned)pagesBySize[i].size());
            tot += (int)pagesBySize[i].size();
        }
        Logf("pathd: 合计 %d 个 CJK 图集页（来自 %d 个页字体）", tot, pageFonts);
    }

    // ── 给每个真实字号扩表 + 挂它自己那套页 + 填字 ────────────────────
    int ok = 0;
    for (auto& fr : fonts) {
        fr.height = FontHeight(fr.font);   // ★ 重新读
        if (fr.height >= kCjkBase) continue;
        {
            // 该字号在包文件里的序号
            int si = -1;
            for (size_t k = 0; k < g_pkg.sizes.size(); ++k)
                if (fabsf(g_pkg.sizes[k].height - fr.height) < 0.01f) { si = (int)k; break; }
            if (si < 0) { Logf("pathd: [%7.3f] 包文件里没有这个字号，跳过", fr.height); continue; }
            if (si >= (int)pagesBySize.size() || pagesBySize[si].empty()) {
                Logf("pathd: [%7.3f] 字号序号 %d 没有对应的 CJK 页，跳过", fr.height, si);
                continue;
            }

            const std::vector<uint32_t>& pv = pagesBySize[si];
            int n = SafeProcessFont(fr.font, fr.height, pv.data(), (int)pv.size());
            if (n > 0) { ++ok; g_doneFonts.push_back(fr.font); }
        }
    }
    Logf("pathd: === 路径 D 准备完成：%d 个字号已装入汉字 ===", ok);
    Logf("pathd: 汉字现在用 16 位索引查表（GBK 原样，不做转码）");

    // ★★★ 装配线程整体结束标记（**不再参与渲染判据**）★★★
    //   P4 现在查的是每个槽自己的 ready（分批开闸），
    //   所以第 1 个字号填完就能显示，不必等这里。
    //   这个标志只用于日志/诊断。
    // ★ HTA_CHS_NO_CJK=1 时不开闸（二分定位用）★
    {
        char v[8] = {0};
        if (GetEnvironmentVariableA("HTA_CHS_NO_CJK", v, sizeof(v)) > 0) {
            Logf("pathd: [调试] HTA_CHS_NO_CJK 已设 —— 装配线程跳过收尾");
            return 0;
        }
    }
    InterlockedExchange(&g_cjkReady, 1);
    Logf("pathd: 装配线程收尾（g_cjkReady=1，仅诊断用；渲染看的是每槽 ready）");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 6) 补丁安装
// ═══════════════════════════════════════════════════════════════════════════
struct PatchSpec {
    const char* name;
    const char* sig;        // 用于定位
    size_t      len;        // 覆盖长度
    const char* expect;     // 期望的原字节（空格分隔，'??' 通配）
};

static bool ExpectMatch(uintptr_t a, const char* expect) {
    const char* p = expect;
    size_t i = 0;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { p += 2; ++i; continue; }
        char t[3] = { p[0], p[1], 0 };
        uint8_t want = (uint8_t)strtoul(t, nullptr, 16);
        if (rd8(a + i) != want) {
            Logf("pathd:   字节校验失败 @+%u: 期望 %02X 实际 %02X", (unsigned)i, want, rd8(a + i));
            return false;
        }
        p += 2; ++i;
    }
    return true;
}

// 安装一个「等长替换」补丁
static bool PatchEqual(const char* name, uintptr_t at, const uint8_t* bytes, size_t n) {
    Logf("pathd: 补丁 %-20s @0x%08X  原字节: %s", name, (unsigned)at, HexDump(at, n).c_str());
    if (!WriteBytes(at, bytes, n)) return false;
    Logf("pathd:   -> 新字节: %s  %s", HexDump(at, n).c_str(), "OK");
    return true;
}

bool InstallPatches() {
    if (g_skipPatch) { Logf("pathd: [调试] 按开关跳过全部补丁"); return true; }
    Logf("pathd: ───── 安装 16 位索引补丁 ─────");
    int done = 0, fail = 0;

    // ── 定位 ──────────────────────────────────────────────────────────
    // ── P0：字体构造时的建表容量 0x100 -> 0x10000 ─────────────────────
    //   fontCtor (0x8B6740) 的 0x8B6772/0x8B6773/0x8B678D：
    //       push eax(0) ; push 100h ; ... ; call 0x8B6660
    //   把那个 100h 改成 10000h，**每个 Font 一出生就是 65536 项**，
    //   走的是引擎自己的构造路径，不需要挂钩、不需要猜调用约定，
    //   也彻底消除了「绘制早于字体创建」那个窗口 —— 之前正是因为
    //   扩表挂在「字体创建」回调上，而首帧绘制发生在它之前，表还是 256 项，
    //   16 位索引越界读到垃圾字形指针而崩（实测 0xC0000005 @0x6865C7）。
    uintptr_t pFontCap = ScanUnique("68 00 01 00 00 F3 0F 11 46 18", "P0 建表容量(fontCtor)", false);
    if (pFontCap) {
        // ★ 已撤销：sub_8B6740 经复核**不是字体构造函数**，而是 XML 相关对象
        //   （它的 +0x40 是子节点列表，sub_8B80B0 会对它做 vtable+0x48("name")
        //   的取属性调用）。改它的容量会波及无关模块，所以不动。
        Logf("pathd: [已撤销] P0 建表容量 @0x%08X（该函数不是字体构造）", (unsigned)pFontCap);
    }


    // ⚠ 必须带前置锚点 `8B 57 40`（mov edx,[edi+40h]）来确定唯一位置。
    //   只用 "0F B6 EB 03 ED 03 ED 8B 04 2A" 会在引擎里匹配到 +1 的位置
    //   （0F 是双字节操作码前缀，ScanUnique 从中间的 0F 之后开始找会错位），
    //   导致补丁整体偏移 1 字节、执行流被破坏 —— 实测 edx 变成 0xFFFFFFFF。
    //   这里的 12 字节锚点从 8B 57 40 开始，+4 才是 movzx 指令起点。
    uintptr_t pMainLookup = 0;
    {
        uintptr_t anchor = ScanUnique("8B 7C 24 34 8B 57 40 0F B6 EB 03 ED 03 ED 8B 04 2A",
                                      "P4 取表+查表");
        if (anchor) {
            pMainLookup = anchor + 4;    // 跳过 mov edi / mov edx 两条
            Logf("pathd:   -> movzx 指令真实起点 = 0x%08X（锚点+4）", (unsigned)pMainLookup);
        }
    }
    uintptr_t pMainWalk   = ScanUnique("0F 57 C0 83 C6 01 83 C7 01 3B 74 24", "P5 主遍历");
    // P5 落点的预期绝对地址（ImageBase 0x400000，无 ASLR）。
    // 只用于「4 条短跳转重定位」的前置校验：落点不是这个值就说明特征码
    // 在别的版本/别处命中，此时绝不能去改 0x6864ED 等硬编码地址。
    const uintptr_t kP5WalkOld = 0x00686A52;
    // 第二处查表：mov eax,[esp+34h] / mov ecx,[eax+40h] / mov eax,[ecx+ebp]
    uintptr_t pLookup2    = ScanUnique("8B 44 24 34 8B 48 40 8B 04 29 85 C0 74 07 F3 0F 10 40 2C",
                                       "P4b 第二处查表");
    uintptr_t pPreIdx     = ScanUnique("0F B6 0C 17 8B 44 24 34 8B 40 40 8B 04 88", "P1 预扫索引");
    // 特征码必须够长：短版在 hta.exe 里 P8 有 2 处、P9 有 3 处命中，
    // 取第一个会补到无关函数。用「取到 glyph 之后的独有后续指令」区分。
    uintptr_t pAdvGet     = ScanUnique("0F B6 44 24 04 8B 49 40 83 3C 81 00 8D 04 81 74 08 8B 10 D9 42 2C", "P8 推进量取字");
    uintptr_t pUvGet      = ScanUnique("0F B6 44 24 08 8B 49 40 83 3C 81 00 8D 04 81 75 13 8B 44 24 04", "P9 UV 取字");
    g_advGet = pAdvGet; g_uvGet = pUvGet;

    // P2/P3：预扫循环的两个分支。位移 EB B5 / EB A1 跨 exe 会变，用通配。
    uintptr_t pPreWalkA = 0, pPreWalkB = 0;
    {
        uintptr_t a = ScanUnique("83 C7 01 EB ?? 0F 28 C8", "P2 预扫遍历A", false);
        uintptr_t b = ScanUnique("83 C7 01 EB ?? 8B 46 18 83 F8 03", "P3 预扫遍历B", false);
        pPreWalkA = a; pPreWalkB = b;
    }
    uintptr_t pMsrWalk = ScanUnique("83 C7 01 3B 7C 24 ?? 0F 8C", "P7 度量遍历");

    // ── P4b 第二处查表：★10 字节★ -> call 助手2 + 5 NOPs ──────────────
    //
    //   ★★ 覆盖长度按 IDA 的**指令地址**差算，不是按"看起来几字节" ★★
    //   原始三条指令（IDA sub_685CA0 实证，地址连续）：
    //       0x686A26  mov eax,[esp+0F0h+var_BC]   4 字节  8B 44 24 34
    //       0x686A2A  mov ecx,[eax+40h]           3 字节  8B 48 40
    //       0x686A2D  mov eax,[ecx+ebp]           3 字节  8B 04 29
    //                                    0x686A30-0x686A26 = 10 字节
    //       0x686A30  test eax,eax                      ← 下一条，不能碰
    //
    //   ★ 我曾把它改成 9 字节 —— 那是错的 ★
    //     `8B 44 24 34` 是 **4** 字节（ModRM=44 / SIB=24 / disp8=34），
    //     我误算成 3+3+3=9。少覆盖 1 字节后，0x686A2F 变成孤立的
    //     SIB 字节 29，CPU 把它和后面的 85 C0 拼成 `sub [ebp-70h],eax`
    //     去执行 —— 转储里那句 "write attempt to address" 就是它。
    //     ★ 教训：永远用 IDA 的指令地址差算长度，不要凭编码长度猜。★
    if (g_skipP4b) { Logf("pathd: [调试] 跳过 P4b"); }
    else if (pLookup2) {
        Logf("pathd: 补丁 P4b 第二处查表 @0x%08X  原字节: %s", (unsigned)pLookup2,
             HexDump(pLookup2, 10).c_str());
        if (WriteCallBlock(pLookup2, 10, (void*)&PathD_GlyphLookup2, "P4b 第二处查表")) ++done;
        else ++fail;
    } else ++fail;

    // ── P1 预扫索引：0F B6 0C 17 -> 0F B7 0C 17（等长 4 字节）────────
    if (pPreIdx) {
        // ★ 已撤销加宽：预扫循环的查表用的是**字体自带的 256 项表**
        //   （mov eax,[esp+34h] / mov eax,[eax+40h] / mov eax,[eax+ecx*4]），
        //   无条件按 16 位索引读就会越界。保持单字节读最安全；
        //   代价是预扫宽度对汉字不准（与 P7 未补的影响同源）。
        Logf("pathd: [已撤销] P1 预扫索引 @0x%08X（预扫查的是字体小表，不能加宽）",
             (unsigned)pPreIdx);
    } else ++fail;

    // ── P4 主查表：18 字节覆盖 ──────────────────────────────────────
    if (pMainLookup) {
        // ★ 必须精确覆盖 18 字节（补丁起点 = `mov edx,[edi+40h]`）★
        //   引擎这里是连续 6 条指令：
        //     +00 mov  edx,[edi+40h]     取字体自带字形表          (3)
        //     +03 movzx ebp,bl           取索引                    (3)
        //     +06 add  ebp,ebp                                     (2)
        //     +08 add  ebp,ebp           → 索引*4                  (2)
        //     +0A mov  eax,[edx+ebp]     ★查表★                   (3)
        //     +0D lea  ecx,[edx+ebp]     ★槽地址★                 (3)
        //     +10 test eax,eax           引擎的空字形判断          (2)
        //                                  小计 18 字节 → 覆盖 +00..+11
        //
        //   ★★ 绝不能多盖 1 字节 ★★
        //   +12 起是 `C6 44 24 12 00`（mov byte [esp+12h],0，7 字节）。
        //   盖到 19 字节会把这条 mov 的首字节 C6 劈开，执行流变成
        //   `44 24`（inc esp / and al,..）彻底错位 → 崩在 0x6865BC，
        //   出错地址 0、操作是写（转储 hta.exe0031 实测）。
        //   同理少盖也不行：
        //     盖 10 → `mov eax,[edx+ebp]` 留在外面，覆盖助手返回的 eax
        //     盖 13 → `lea ecx,[edx+ebp]` 留在外面，无条件重算 ecx
        //            （汉字的 g_cjkTable 槽被顶掉；pl_null 时 ecx=0 被
        //              重算成 fontTable+0 → 0x686608 读地址 0 崩溃）
        //     盖 16 → 只盖到 lea 的中间（lea 占 +0D..+0F），`test` 留在外面
        //
        //   ★ 教训：长度必须按**指令条数逐条累加**算出来，不能凭感觉 ★
        //   正确算法：3+3+2+2+3+3+2 = 18，然后 call(5) + NOP(13)。
        Logf("pathd: 补丁 P4 主查表 @0x%08X  原字节: %s", (unsigned)pMainLookup,
             HexDump(pMainLookup, 18).c_str());
        if (WriteCallBlock(pMainLookup, 18, (void*)&PathD_GlyphLookup, "P4 主查表")) ++done;
        else ++fail;
    } else ++fail;

    // ── P8/P9 取字函数：movzx -> mov（等长 4 字节）──────────────────
    // ★★ P8/P9 已撤销（实测导致崩溃）★★
    //   原打算把取字函数从 movzx(1 字节) 改成 mov(4 字节)，好让度量也能用
    //   16 位索引。但调用方传的是 `char`，反编译为 sub_66FEA0((char)a4) ——
    //   编译器只写 AL 就 push EAX，高 24 位是残留垃圾。改成读 4 字节后索引
    //   变成天文数字，[ecx+eax*4] 越界读，随后崩在完全无关的地方
    //   （实测崩在 XML 解析的虚调用上，栈回溯才定位到）。
    //   而且收益为零：P7 度量遍历没补，度量仍按字节走，索引本身还是单字节。
    //   撤掉后度量退回「按字节查表」，索引必然 < 256，绝对安全。
    if (pAdvGet) Logf("pathd: [已撤销] P8 推进量取字 @0x%08X（读 4 字节会取到调用方垃圾高位）", (unsigned)pAdvGet);
    if (pUvGet)  Logf("pathd: [已撤销] P9 UV 取字   @0x%08X（同上）", (unsigned)pUvGet);

    // ── P5 主遍历：9 字节 -> call 助手 + 4 NOP ─────────────────────────
    //
    // ★★★ 为什么必须连 xorps 一起覆盖（0xC0000096 @0x686A5E 的真正原因）★★★
    //   转储 hta.exe0072（10:05:18）运行时字节：
    //       0x686A52  0F 57 C0           xorps xmm0,xmm0
    //       0x686A55  E8 06 F3 DD 68     call PathD_DrawAdvance   ← 只补 1 个 NOP
    //       0x686A5A  90                 nop
    //       0x686A5B  3B 74 24 6C        cmp esi,[esp+6C]        ← 完整 4 字节
    //   崩溃地址 0x686A5E = 那条 `cmp` 的**第 4 个字节**。
    //   0x6C 作为指令起点解码就是 INSB（特权指令）→ 0xC0000096 PRIV。
    //
    //   也就是说：返回地址落在 0x686A5A（那个 NOP），NOP 之后顺延到
    //   0x686A5B 执行 cmp —— 这条路本身没错。真正的问题是**跳进 cmp 中间**
    //   只可能来自别的跳转，而全代码段没有任何跳往 0x686A5E 的指令。
    //   ⇒ 唯一自洽的解释：P5 覆盖的 6 字节把 `add edi,1` 劈开后，
    //   引擎从 0x686A52 顺序落下时执行流错位。
    //
    //   修法（对齐法）：把落点前移到 0x686A52，一次覆盖
    //       xorps(3) + add esi,1(3) + add edi,1(3) = 9 字节
    //   写成 call(5) + NOP(4)，helper 自己做 xorps + 双字节推进，
    //   ret 正好落回 0x686A5B = `cmp esi,[esp+6C]` 的**正确起点**。
    //   这样无论从哪个跳入点进来，返回地址都在指令边界上。
    if (g_skipP5) { Logf("pathd: [调试] 跳过 P5"); }
    else if (pMainWalk) {
        // ── 先把 4 条长跳转的目标从 0x686A55 改到 0x686A52 ────────────
        //   0x686A55 现在是 call 指令的第 3 字节（E8 + rel32 的 rel32 占
        //   +1..+4，所以 0x686A55 正落在 rel32 里）。跳进 rel32 会把中间
        //   3 个字节当指令执行 —— 必然乱（实测 0xC0000005 @0x686A55）。
        //
        //   ★ 这 4 条全是 **rel32**，不是 rel8 ★（实测字节，10:11 那次）
        //     0x6864ED  E9 63 05 00 00        jmp rel32
        //     0x686507  0F 8E 48 05 00 00     jle rel32
        //     0x68658B  E9 C5 04 00 00        jmp rel32
        //     0x68659C  0F 84 C5 03 00 00     jz  rel32
        //   之前按 rel8 算：0x6864EF + 99 = 0x686552，与真目标 0x686A55
        //   差了 0x500 —— 说明我误认的那个字节其实属于别的指令。
        //
        //   修法：new_rel32 = 新目标 - 跳转末字节地址，重算后写回 4 字节。
        if (pMainWalk != kP5WalkOld) {
            Logf("pathd: [警告] P5 落点 0x%08X 与预期 0x%08X 不符，跳过跳转重定位",
                 (unsigned)pMainWalk, (unsigned)kP5WalkOld);
        } else {
            struct { uintptr_t at; int len; const char* name; } reloc[] = {
                { 0x6864ED, 5, "jmp  (控制符 <0x20)" },
                { 0x686507, 6, "jle  ('@' 未到截断)" },
                { 0x68658B, 5, "jmp  ('#' 已处理)"    },
                { 0x68659C, 6, "jz   ('$'/'&' 无下标)" },
            };
            const uintptr_t kNewTgt = pMainWalk;          // 0x686A52
            for (auto& r : reloc) {
                uint8_t op0 = rd8(r.at);
                uint8_t op1 = rd8(r.at + 1);
                bool isJmp = (op0 == 0xE9);
                bool isJcc = (op0 == 0x0F && (op1 & 0xF0) == 0x80);
                if (!isJmp && !isJcc) {
                    Logf("pathd: [跳过] %s @0x%08X 操作码 %02X %02X 不是 rel32 跳转",
                         r.name, (unsigned)r.at, op0, op1);
                    continue;
                }
                uint32_t  oldB   = (uint32_t)rd32s(r.at + r.len - 4);
                int32_t   oldRel = (int32_t)oldB;
                uintptr_t oldTgt = (uintptr_t)((int64_t)r.at + r.len + oldRel);

                int32_t   newRel = (int32_t)((int64_t)kNewTgt - (int64_t)(r.at + r.len));
                uint32_t  newB = (uint32_t)newRel;
                uint8_t   buf[4] = { (uint8_t)(newB & 0xFF), (uint8_t)((newB >> 8) & 0xFF),
                                     (uint8_t)((newB >> 16) & 0xFF), (uint8_t)((newB >> 24) & 0xFF) };
                if (!WriteBlockSafe(r.at + r.len - 4, buf, 4, r.name)) {
                    Logf("pathd: [警告] 重定位 %s @0x%08X 写入失败", r.name, (unsigned)r.at);
                    continue;
                }
                uintptr_t newTgt = (uintptr_t)((int64_t)r.at + r.len + newRel);
                Logf("pathd: 重定位 %s @0x%08X  rel32 %d -> %d  目标 0x%08X -> 0x%08X %s",
                     r.name, (unsigned)r.at, oldRel, newRel,
                     (unsigned)oldTgt, (unsigned)newTgt,
                     (newTgt == kNewTgt) ? "OK" : "★目标不符");
            }
        }

        Logf("pathd: 补丁 P5 主遍历 @0x%08X  原字节: %s", (unsigned)pMainWalk, HexDump(pMainWalk, 9).c_str());
        if (WriteCallBlock(pMainWalk, 9, (void*)&PathD_DrawAdvance, "P5 主遍历")) ++done;
        else ++fail;
    } else ++fail;

    // ── P7 度量遍历：**故意不补** ────────────────────────────────────
    //   0x685BD1 处只有 3 字节（83 C7 01），后面紧跟 4 字节 cmp + 6 字节 jl。
    //   放不下条件判断；用 call 需要吃掉 cmp 的前 2 字节并让助手重做 cmp+jl，
    //   还要知道度量循环里「串基址」在哪个寄存器 —— 这些没验证过，
    //   硬补的风险是**破坏 ASCII 度量**（那会让整个 UI 布局崩掉）。
    //   所以这里保持原样：度量按字节走，中文宽度会被低估约一半。
    //   后果：居中/右对齐的中文会偏左；左对齐不受影响。
    //   绘制本身是正确的（P4/P5 已补）。后续用重定位方案再修 P7。
    if (pMsrWalk) {
        Logf("pathd: [保持原样] P7 度量遍历 @0x%08X  原字节 %s",
             (unsigned)pMsrWalk, HexDump(pMsrWalk, 3).c_str());
        Logf("pathd:        原因：3 字节空间放不下条件判断，硬补会破坏 ASCII 度量。");
        Logf("pathd:        影响：中文宽度被低估（居中/右对齐会偏左），左对齐无影响。");
    } else {
        Logf("pathd: [MISS] P7 度量遍历");
    }

    // ── P2/P3 预扫遍历 ───────────────────────────────────────────────
    // 原指令是 add edi,1 ; jmp rel8 —— 助手必须自己尾跳回循环头，
    // 所以先把 rel8 目标算出来存到全局，供 naked 助手使用。
    struct { uintptr_t at; void* helper; void** topSlot; const char* name; } pw[2] = {
        { pPreWalkA, (void*)&PathD_PrepassAdvanceA, (void**)&PathD_PrepassTopA, "P2 预扫遍历A" },
        { pPreWalkB, (void*)&PathD_PrepassAdvanceB, (void**)&PathD_PrepassTopB, "P3 预扫遍历B" },
    };
    for (auto& w : pw) {
        if (!w.at) { Logf("pathd: [MISS] %s（预扫总宽会偏小，不影响不崩）", w.name); continue; }
        int8_t rel8 = (int8_t)rd8(w.at + 4);
        uintptr_t top = (uintptr_t)((int64_t)(w.at + 5) + rel8);
        *w.topSlot = (void*)top;
        Logf("pathd: %s @0x%08X  jmp rel8=%d -> 循环头 0x%08X（变量@0x%08X 值=0x%08X）",
             w.name, (unsigned)w.at, (int)rel8, (unsigned)top,
             (unsigned)(uintptr_t)w.topSlot, (unsigned)(uintptr_t)*w.topSlot);
        Logf("pathd:   原字节: %s", HexDump(w.at, 5).c_str());
        if (WriteCallBlock(w.at, 5, w.helper, w.name)) ++done; else ++fail;
    }

    // ── 补丁落地自检：把每个补丁点**重新读回来**打印 ──────────────────
    //   之前出现过「日志说成功、崩溃现场却像没打上」的矛盾（0x686A2D 是 NOP
    //   却以原始指令形态崩溃）。直接回读字节是最可靠的证据 —— 如果这里显示
    //   已经是 NOP/跳转，而崩溃现场仍是原始指令，那说明**有第二处相同代码**
    //   在被执行。诊断信息不猜，直接读。
    //
    //   ★ 额外检查「补丁末尾那条指令的起始字节」★
    //   盖多了会把下一条指令劈开（19 字节那次把 C6 44 24 12 00 切成 44 24，
    //   崩在 0x6865BC）。所以额外多读 4 字节，把边界外的字节也打出来，
    //   人工/日志都能立刻看出有没有劈到下一条指令。
    {
        struct Chk { const char* tag; uintptr_t at; int n; } chk[] = {
            { "P4  第一处查表", pMainLookup, 18 },
            { "P4b 第二处查表", pLookup2,     10 },   // ★ 9 是错的，见上 ★
            { "P5  主遍历",     pMainWalk,     9 },
        };
        for (int i = 0; i < 3; ++i) {
            if (!chk[i].at) { Logf("pathd: [自检] %-16s 未定位", chk[i].tag); continue; }
            Logf("pathd: [自检] %-16s @0x%08X  覆盖 %d 字节: %s  ‖ 边界外: %s",
                 chk[i].tag, (unsigned)chk[i].at, chk[i].n,
                 HexDump(chk[i].at, chk[i].n).c_str(),
                 HexDump(chk[i].at + chk[i].n, 4).c_str());
        }
    }

    Logf("pathd: ───── 补丁安装完成：成功 %d，失败 %d ─────", done, fail);
    return done > 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 7) 入口
// ═══════════════════════════════════════════════════════════════════════════
bool Init(HMODULE game, const char* pkgPath) {
    g_modBase = (uintptr_t)game;
    InitializeCriticalSection(&g_cs);

    // 二分定位开关（临时调试用）
    {
        char v[8];
        g_skipPatch  = GetEnvironmentVariableA("HTA_CHS_NO_PATCH",  v, sizeof(v)) > 0;
        g_skipExpand = GetEnvironmentVariableA("HTA_CHS_NO_EXPAND", v, sizeof(v)) > 0;
        g_skipP5     = GetEnvironmentVariableA("HTA_CHS_NO_P5",     v, sizeof(v)) > 0;
        g_skipP4b    = GetEnvironmentVariableA("HTA_CHS_NO_P4B",    v, sizeof(v)) > 0;
        g_skipScan   = GetEnvironmentVariableA("HTA_CHS_NO_SCAN",   v, sizeof(v)) > 0;
        Logf("pathd: 调试开关 跳过补丁=%d 跳过扩表=%d 跳过P5=%d 跳过P4b=%d 跳过扫描=%d",
             (int)g_skipPatch, (int)g_skipExpand, (int)g_skipP5, (int)g_skipP4b, (int)g_skipScan);
    }

    Logf("pathd: ══════════ 路径 D 初始化 ══════════");
    if (!LoadPackage(pkgPath)) {
        Logf("pathd: 未启用路径 D（回退路径 C）");
        return false;
    }

    // ── 字体管理器句柄（dword_A0A88C 的内容）──────────────────────────
    //  ★ 这里必须现在就取：SetupThread 依赖它枚举字体。
    //  ★ 用模块基址 + RVA，不写死绝对地址（虽然本 exe 无 ASLR）。
    {
        uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
        uint32_t* slot = (uint32_t*)(base + RVA_EngineCore);
        g_fontMgr = (void*)(*slot);
        Logf("pathd: 字体管理器 = %p（dword_%07X = %p，base=%p）",
             g_fontMgr, 0x400000u + RVA_EngineCore, (void*)*slot, base);
    }

    // ── 每字体的汉字字形表：改成**懒分配** ──────────────────────────
    //   旧实现在这里一张性分配 256KB 的共享 g_cjkTable；现在每张表都是
    //   256KB × 槽数（最多 16 槽 = 4MB），不能预先全部分配 ——
    //   实际只有约 10 个字号真正用得上，而这里我们对每个 Font 都调
    //   CjkTableEnsure，全部预分配会白吃 4MB。
    //   改为在 FillCjk 里按需分配（CjkTableEnsure），此处不再分配。
    Logf("pathd: 汉字表改为按字体懒分配（最多 %d 槽 × %u 项）",
         MAX_CJK_TABLES, kTableEntries);

    // vector::resize（引擎自带，保证用同一个分配器）
    g_vecResize = (VecResizeFn)ScanUnique(
        "51 8B 4C 24 08 8B 41 04 85 C0 74 0A 8B 51 08 2B D0 C1 FA 02",
        "vector::resize(sub_8B6660)", false);
    if (!g_vecResize) {
        // 退一步：直接用已知 RVA（仅 hta.exe 正确）
        uintptr_t cand = g_modBase + 0x8B6660 - 0x400000;
        Logf("pathd: vector::resize 特征码未命中，按 RVA 猜测 0x%08X（可能只对本作有效）", (unsigned)cand);
        g_vecResize = (VecResizeFn)cand;
    }

    // ── 不再调用引擎的扩容函数 ────────────────────────────────────────
    //   它是我在特征码未命中后按 RVA **猜**出来的（0x8B6660），连调用约定
    //   都是猜的。实测调用它之后容量/指针毫无变化，说明身份就是错的。
    //   而「能返回」不等于「调用约定对」—— 若它实际是 __cdecl，被调用方不
    //   清理栈，调用者的 ESP 就会偏低若干字节，函数返回时弹出的返回地址
    //   是垃圾，直接跳到 NULL（实测 0xC0000005 @ EIP=0）。
    //   所以彻底不碰它，一律用自己分配的表。
    g_vecResizeOk = false;
    Logf("pathd: 不使用引擎扩容函数（身份未证实，调用它有栈错乱风险）");

    // ★★★ 定位引擎堆分配器（修复堆破坏的关键一步）★★★
    //   sub_589410 : __fastcall(ecx=size, edx=0, stack=0)，内部 mov ecx,dword_A0A880
    //               后调 sub_748DC0；返回 retn 4。
    //   用它给页表分配内存，引擎 ~Font 的 free 才能正确配对。
    g_engineAlloc = (EngineAllocFn)ScanUnique(SIG_ENGINE_ALLOC, "引擎分配器(sub_589410)", false);
    if (!g_engineAlloc) {
        Logf("pathd: [警告] 未定位到引擎分配器 —— 页表无法挂载，汉字将不显示");
        Logf("pathd:         特征码: %s", SIG_ENGINE_ALLOC);
    } else {
        // 自检：真的能分配 + 释放吗？只分配不释放（泄漏几十字节换安全），
        // 但至少要确认返回值合理（引擎堆指针，非 NULL、已对齐）。
        void* probe = nullptr;
        __try {
            probe = g_engineAlloc(64, nullptr, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("pathd: [警告] 引擎分配器自检抛异常 0x%08X", (unsigned)GetExceptionCode());
            g_engineAlloc = nullptr;
        }
        if (probe) {
            Logf("pathd: 引擎分配器 = 0x%08X（自检通过，probe=0x%08X）",
                 (unsigned)(uintptr_t)g_engineAlloc, (unsigned)(uintptr_t)probe);
        } else if (g_engineAlloc) {
            Logf("pathd: [警告] 引擎分配器自检返回 NULL，停用以避免崩溃");
            g_engineAlloc = nullptr;
        }
    }

    // ── 字体获取方式：直接枚举字体管理器数组（不挂钩）────────────────
    //
    //   试过钩 Font::CreateFromXmlNode（sub_8B80B0），三次都失败：
    //     1. __fastcall 版 → 毁 edi → 0x8B8122 崩
    //     2. __cdecl 版    → 栈错位 → 启动即退
    //     3. naked 搬栈版  → **不崩了，但一次都没被调用**
    //
    //   第 3 条才是关键证据（08:06 那次运行）：
    //     引擎日志：字体加载于 08:06:26
    //     我装钩子：  08:06:27.218
    //     ★ 晚 1.2 秒。字体在 ASI 装钩子之前就全建完了。★
    //
    //   所以正确做法不是「更早地挂钩」，而是**根本不用挂钩** ——
    //   引擎启动完成后，字体已经全部存在于 FontManager+1188 的
    //   vector<Font*> 里，直接枚举即可（IDA sub_8BA3A0 实证）。
    Logf("pathd: 不挂钩字体创建（装钩子晚于字体加载 1.2 秒），改用字体管理器枚举");
    g_hookFont = false;

    // 安装 16 位索引补丁
    InstallPatches();

    // 起后台线程做后续装配
    HANDLE th = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
    if (th) { CloseHandle(th); Logf("pathd: 装配线程已启动"); }
    else    { Logf("pathd: [失败] 装配线程创建失败 %lu", GetLastError()); }

    g_enabled = true;
    Logf("pathd: 初始化返回（补丁已生效，装配在后台进行）");
    return true;
}

bool IsEnabled() { return g_enabled; }

// —— 绘制期字体登记（P4 在热路径上调用）——
//   只做一次线性去重扫描 + 一次 InterlockedIncrement，
//   最多 64 项。字体种类在整个进程生命周期里是固定的（几十个），
//   所以这个开销可以忽略；换来的是"不需要猜任何全局变量"。
static int g_regCalls = 0, g_regNew = 0;
extern "C" void __cdecl PathD_RegisterFont(void* fp) {
    if (++g_regCalls < 200000) { ++g_regNew; }   // 简单计数，防日志刷屏
    LONG n = g_seenCount;
    for (LONG i = 0; i < n && i < MAX_SEEN_FONTS; ++i)
        if (g_seenFonts[i].p == fp) return;                  // 已登记
    if (n >= MAX_SEEN_FONTS) return;
    g_seenFonts[n].p  = fp;
    g_seenFonts[n].slot = -1;                                // 待装配
    LONG want = n + 1;
    InterlockedCompareExchange(&g_seenCount, want, n);        // 并发去重
}

} // namespace pathd

// ═══════════════════════════════════════════════════════════════════════════
// 8) 条件推进助手（naked asm）
//
// 三处遍历原本都是 add reg,1，但可用空间只有 3~6 字节，放不下条件判断，
// 所以用 call + 尾跳。助手必须保护除「推进寄存器」以外的所有寄存器。
//
// 寄存器约定（已由反编译确认）：
//   sub_685CA0 主循环： eax=串基址  esi=下标  edi=并行计数(与下标同步)
//   sub_685CA0 预扫：   edx=串基址  edi=下标
//   sub_685990 度量：   edi=下标
// ═══════════════════════════════════════════════════════════════════════════
extern "C" void __cdecl PathD_RegisterFont(void* fp);

extern "C" {

// —— P4：字形查表（替换 18 字节原指令）——
// ★★ 核心教训（实测 hta.exe0033 转储确认）★★
//   崩溃时的栈帧：[esp+0x74] = 0xFFFFFFFF，而这正是引擎的 var_BC，
//   也就是 draw 的 Font 参数。也就是说 —— **Font 指针本身就是 0xFFFFFFFF**，
//   不是「Font+0x40 是 0xFFFFFFFF」。
//   之前只判了 [edi+40h] 是否为 0xFFFFFFFF，于是
//   `mov edx,[edi+40h]` 读到 [0xFFFFFFFF+0x40]（越界）= 0，
//   再 `[edx+ebp]` 就成了读地址 0 → 0xC0000005，出错地址 0x00000000。
//   ★ 必须先验 Font 指针本身，再验它取出来的表 ★
__declspec(naked) void __cdecl PathD_GlyphLookup() {
    __asm {
        push ebx
        push esi
        push edi
        // ★★★ 绝对不要 push/pop eax ★★★
        //   eax 是**返回值**（字形指针），引擎靠它判断「有没有字形」
        //   （0x6865B3 test eax,eax / 0x686604 test eax,eax / jz）。
        //   曾经为了"保存文本基址"写了 push eax + 末尾 pop eax ——
        //   那是彻底错的：文本基址根本不在 eax（在栈上 [esp+0x54]），
        //   而 pop eax 会把查到的字形指针覆盖回旧值。
        //   实测后果（hta.exe0062 转储，0x68660D 读地址 0x4）：
        //     CJK 表该格为空 -> 我置 eax=0 想跳过 -> pop eax 又变回非 0
        //     -> 引擎走"有字形"分支，用 pl_null 算的 ecx 去 mov edx,[ecx]
        //     -> [ecx]=0 -> add edx,4 -> mov edi,[4] -> 崩。
        //   这一条同时解释了为什么 ASCII 看着"能用"却位置/尺寸不对：
        //   eax 被换成了串基址，引擎把文本地址当字形结构读了 +0x24/+0x28。
        //
        //   ★ 栈偏移 = call 返回地址(4) + 3 个 push(12) = 引擎位移 + 0x10 ★
        //       保存的 edi(Font*) = [esp+0]
        //       引擎 var_9C 文本基址（是**指针**）= [esp+0x64]
        //       第 2 字节 = [[esp+0x64] + esi + 1]
        //       引擎 var_84 串长    = [esp+0x7C]
        //   ★ 为什么是 +0x10 而不是 +0xC ★
        //     助手是**被 call 进来的**：call 压入返回地址 4 字节，
        //     再加 3 个 push = 12 字节，共 0x10。
        //     旧代码按 +0xC 算（漏了返回地址），三个偏移全部低 4 字节 ——
        //     这正是「英文能画、汉字不显示」的直接原因：
        //     汉字分支读的 [esp+0x61+esi] 是**栈帧内容**而非文本，
        //     GBK 码恒为垃圾 -> 查表必落空 -> 返回 NULL -> 汉字全不画。
        //   ★ edx 故意不压栈 ★ 见 pl_out 的说明

        // ── Font 自注册 ────────────────────────────────────────────
        //   edi 就是引擎这一轮用的 Font*（0x6865A2 `mov edi,[esp+var_BC]`）。
        //   把它登记进 g_seenFonts，装配线程就能找到全部在用字体，
        //   不必再去猜字体管理器的全局变量。
        //   必须放在所有校验**之前**，因为 FontManager 里的指针未必
        //   等于这里出现的顺序/集合；而且校验失败的那些也可能是真字体
        //   （只是那一瞬间的表指针不可读）。
        cmp   edi, 10000h
        jb    pl_noreg                // 太小的指针不可能是 Font
        mov   ecx, dword ptr [g_seenCount]
        cmp   ecx, MAX_SEEN_FONTS
        jae   pl_noreg                // 满了就不再登记
        imul  ecx, ecx, 8             // 8 = sizeof(SeenFont)
        mov   edx, offset g_seenFonts
        xor   eax, eax
    pl_scan:
        cmp   eax, ecx
        jae   pl_append                // 扫完了没命中 -> 追加
        cmp   dword ptr [edx+eax*4], edi
        je    pl_noreg                 // 已登记，退出
        add   eax, 4
        jmp   pl_scan
    pl_append:
        mov   dword ptr [edx+eax], edi                    // 写入 Font*
        mov   dword ptr [edx+eax+4], 0FFFFFFFFh          // slot = -1 待装配
        inc   dword ptr [g_seenCount]                     // 渲染线程单线程，无竞争
    pl_noreg:

        // edi 是 Font：引擎的 `mov edi,[esp+var_BC]` 在 0x6865A2，位于我们覆盖
        // 范围（0x6865A6 起）之外，所以这里的 edi 就是引擎给的值。
        // ★ 第 1 关：Font 指针本身必须是可读的合法地址 ★
        //   实测崩溃时 edi == 0xFFFFFFFF（栈帧 [esp+0x74] 证据）。
        cmp   edi, 0FFFFFFFFh
        je    pl_null
        //   也不接受 0 / 小于最小堆地址的值：这些同样会让 [edi+40h] 读飞。
        test  edi, edi
        jz    pl_null
        // ★ 第 1b 关：Font* 必须落在引擎堆区（收紧判定）★
        //   0x419FBEC0 这种"看着像堆"的野值实测会通过 0x10000 的粗判，
        //   然后读 [eax+40h] 时炸。真实的 Font* 一律在
        //   0x10000000..0x7FFEFFFF（日志实测 0x1354E7F4 / 0x1362F9F4）。
        cmp   edi, 10000h
        jb    pl_null
        cmp   edi, 7FFFFFFFh
        jae   pl_null

        // 第 2 关：字体自带字形表（edx 不可信，mov edx,[edi+40h] 已被盖掉）
        mov   edx, dword ptr [edi+40h]
        cmp   edx, 0FFFFFFFFh
        je    pl_null
        test  edx, edx
        jz    pl_null
        // ★ 第 3 关：表指针必须落在 32 位进程的合法用户态区间 ★
        //   0x10000 以下是未分配页/保护页，0x80000000 以上是内核空间。
        //   之前想用 IsBadReadPtr，但它走 SEH 且要 3 个参数，
        //   在裸汇编热路径上既不安全又会显著拖慢每个字符 —— 改用区间判断。
        // ★ 同样收紧到引擎堆区（与上面 Font* 一致）★
        cmp   edx, 10000h
        jb    pl_null
        cmp   edx, 7FFFFFFFh
        jae   pl_null
        mov   dword ptr [g_lastFontTable], edx

        // ★★ 必须先看首字节区分 ASCII 与汉字 ★★
        //   引擎原指令是 `movzx ebp, bl` —— bl 才是当前字符，
        //   不是 [esi+eax]（我一开始写成 [esi+eax]，此时 eax 已被引擎改写，
        //   读出来的是垃圾字节，索引全错 —— 必须用 bl）。
        //   另外不能无条件读 16 位：否则 ASCII 的 'N' 被拼成 'N'|('e'<<8)，
        //   跑去查（全空的）汉字表拿到 NULL，于是**所有拉丁字形全部消失**，
        //   文本布局崩坏，最后跳到 NULL（实测 EIP=0）。
        movzx ebp, bl                       // b1 = 当前字符
        cmp   ebp, 81h
        jae   pl_cjk                        // >=0x81 是 GBK 前导字节

        // ASCII/拉丁：单字节索引，沿用字体自带的 256 项表（与原来完全一致）
        test  edx, edx
        jz    pl_null
        mov   eax, dword ptr [edx+ebp*4]
        test  eax, eax
        jz    pl_null                        // 该字符没有字形，让引擎跳过
        lea   ecx, [edx+ebp*4]
        shl   ebp, 2
        mov   dword ptr [g_curGlyph], eax      // P4b 直接取这个字形
        mov   dword ptr [g_curChar],  ebx       // ★ bl = 当前字符，供 P4b 校验 ★
        jmp   pl_out

    pl_cjk:
        // ★★★ 分批开闸：只查**当前字体那个槽**的 ready，不再等全部字号 ★★★
        //
        //   旧实现用全局 g_cjkReady，要等 10 个字号全填完才置 1，
        //   于是「进入界面要等 8 秒才出中文」。但第 1 个字号填完
        //   （约 0.4 秒）就已经能显示了 —— 后面几个慢慢来即可。
        //   FillCjk 本来就是每填完一个字号就置该槽 ready=1，
        //   这里查的就是它。未就绪的槽 -> 引擎安全跳过（该帧不显示），
        //   既不崩，也不拖累别的字号。
        //
        //   g_cjkReady 保留为「装配线程整体结束」的诊断标记，不再参与判据。
        //   槽布局：{ void* font(0); uint32_t* table(4); LONG ready(8); }
        mov   eax, offset g_cjkSlots
        mov   ecx, dword ptr [esp+0]      // Font*
        mov   edx, dword ptr [g_cjkSlotCount]
        test  edx, edx
        jz    pl_null                     ; 一个槽都没有
    pl_gate:
        test  edx, edx
        jz    pl_null
        cmp   dword ptr [eax], ecx         ; slot.font == Font* ?
        je    pl_gate_hit
        add   eax, 12                     ; sizeof(CjkSlot)
        dec   edx
        jmp   pl_gate
    pl_gate_hit:
        cmp   dword ptr [eax+8], 0        ; ★ slot.ready —— 分批开闸就在这
        je    pl_null                     ; 该字号还没填完 -> 跳过
    pl_cjk_go:
        // ★★★ 汉字分支：按 GBK 取 2 字节，查我们自持的 64K 表 ★★★
        //
        //   ★★ 这里必须用**栈上的 var_9C**，不能用 eax ★★
        //
        //   证据（本次会话最贵的第二个教训）：
        //     转储 hta.exe0050（08:34:58）：`read attempt to address 0x52`
        //     崩在我的 P4 内部 0x663C5DBC，也就是 `movzx ebx,word [esi+eax]`。
        //     0x52 = 'R'。也就是说 esi+eax 算出了 0x52 这个荒谬地址。
        //
        //     我原先的推理是「入口处 eax 恰好还是 var_9C」，这只在
        //     IDA 静态看到的那几条路径上成立。实际运行还有别的调用来源，
        //     走到这里时 eax 早已不是文本基址。
        //
        //   ★ 栈偏移是**用字节码实测的**，不是照抄 IDA 的 stack_frame ★
        //     IDA 的 stack_frame 里 var_9C 写的是 offset 0x94，但那个数字
        //     是相对 esp+0xF0 的相对量，**不能直接当 esp 位移用**。
        //     实测字节：
        //       0x6864B1  8B 44 24 54   mov eax,[esp+0x54]   ← var_9C 真值 = 0x54
        //       0x6865A2  8B 7C 24 34   mov edi,[esp+0x34]   ← var_BC 真值 = 0x34
        //       0x6864BA  3B 74 24 6C   cmp esi,[esp+0x6C]   ← var_84 真值 = 0x6C
        //     教训：栈变量偏移一律读 ModRM/SIB 的 disp 字节，别信 IDA 的表。
        //
        //   P4 是被**引擎 call 进来**的：call 压 4 字节返回地址，再加 3 个
        //   push = 12 字节，所以引擎的 [esp+X] 在这里是 [esp+X+0x10]：
        //       文本基址 var_9C        = [esp+0x64]   ← 这是**指针**
        //       第 2 字节              = [[esp+0x64] + esi + 1]
        //       字符串长度 var_84      = [esp+0x7C]
        //
        //   ★★★ 这里最关键的一点：var_9C 是指针，不是缓冲区 ★★★
        //     证据：0x6864B1 `mov eax,[esp+0x54]` 取出的值直接被
        //     0x6864E0 `mov bl,[esi+eax]` 当作**基址**用，esi 是下标。
        //     所以第 2 字节必须「先取出基址再解引用」，
        //     直接写 `[esp+0x65+esi]` 读的是**栈帧里的 4 字节**（长度/指针等），
        //     恒为垃圾值 -> GBK 码错 -> 查表落空 -> 汉字不显示。
        cmp   esi, dword ptr [esp+7Ch]     // esi >= strlen ? 越界
        jae   pl_null
        mov   edx, dword ptr [esp+64h]     // ★ 取文本基址指针 ★
        test  edx, edx
        jz    pl_null                      // 基址为空，绝不解引用
        // ★★★ 累积 GBK 码必须用 ebp，**绝不能碰 ebx/bl** ★★★
        //   原因：pl_null 要靠 `bl` 取回**原字符**来算 ecx = 表 + bl*4
        //   （引擎契约）。如果这里用 ebx 累积，bl 就变成了 GBK 的低字节，
        //   pl_null 算出的 ecx 索引全错。
        //   实测教训：转储 ECX=0x2DC = 0xB7*4，0xB7 正是某个 GBK 低字节，
        //   而不是当前字符 —— 就是这个原因。
        movzx ebp, bl                      // ebp = b1（bl 保持原样）
        shl   ebp, 8
        movzx ecx, byte ptr [edx+esi+1]    // ★ b2 = 文本[esi+1]（间接寻址）★
        or    ebp, ecx                     // ebp = GBK 码
        // ★★★ 改为「按 Font* 选本字体的表」（修复 10 个字号互相覆写）★★★
        //   旧代码 `mov edi,[g_cjkTable]` 取的是**全局共享**表，
        //   而 SetupThread 按字号依次填充，后填的会盖掉先填的。
        //
        //   ★★★ 为什么**内联扫**而不是 call PathD_CjkTableFor ★★★
        //     实测：改成 C++ call 之后，按钮文字**全部消失**（不是叠字，是空白）。
        //     两个都致命的原因：
        //       1) `call` 压 4 字节返回地址。P4 是被引擎 call 进来的裸栈帧，
        //          push/pop 对称看似没事，但期间 esp 变动会让任何重入/异常崩掉。
        //       2) **更致命**：引擎契约要求「除 eax/ecx/ebp 外全部寄存器原样保留」，
        //          而 bl/bh 只是 ebx 的高低字节。编译器生成的循环完全可能拿
        //          ebx 当计数器，bl 随之被改写 —— 于是 pl_null 里的
        //          `movzx ebp,bl` 索引全错，整个 CJK 判据失效。
        //     ⇒ 只能内联，且只碰引擎已经用完、不再需要的寄存器。
        //
        //   ★ 槽地址 eax 从 pl_gate 一路带到这里 ★
        //     pl_gate 已经找到 slot 并验过 ready，这里直接取 table 即可。
        //     旧代码在这里又扫了一遍同样的槽表 —— 每个字形多花 16 次
        //     比较，热路径上是纯浪费。
        //     中途只改了 ebp(GBK 码)/ecx(b2)/edx(文本基址)，eax 未被动过。
        //
        //   ★ 为什么不能把 GBK 码放 eax 带过扫描 ★
        //     契约要求出口 ebp = 索引*4，eax 最终必须交回字形指针，
        //     两个位置都被占满，码只能一路待在 ebp 里。
        mov   edi, dword ptr [eax+4]        ; 该字体的表
        test  edi, edi
        jz    pl_null                     ; 无汉字表 -> 引擎安全跳过
        cmp   edi, 10000h
        jb    pl_null
        cmp   edi, 7FFFFFFFh
        jae   pl_null
        mov   eax, dword ptr [edi+ebp*4]
        // ★ 表里这一格为空时让引擎跳过（eax=0）★
        //   去掉 pop eax 之后 eax=0 能真正传到引擎，
        //   0x6865BD jnz 与 0x686606 jz 都会走"无字形"分支，安全。
        test  eax, eax
        jz    pl_null
        lea   ecx, [edi+ebp*4]             // 槽地址（与 eax 同址，保持一致）
        shl   ebp, 2                       // ★ 引擎契约：ebp = 索引*4 ★
        mov   dword ptr [g_curGlyph], eax      // P4b 直接取这个字形
        mov   dword ptr [g_curChar],  ebx       // ★ bl = 当前字符，供 P4b 校验 ★
        jmp   pl_out

    pl_null:
        // ★★★ 引擎契约（实测崩溃 0x686608，08:48）★★★
        //     转储：EAX=039C45B7(栈地址)  ECX=000002DC  EDX=00000000  EBX=52
        //     0x2DC = 0xB7*4，EDX=0 → 说明 ecx = edx + ebp*4 里 edx 来自
        //     我下面那个 `xor edx,edx` 兜底。
        //     而 EAX 不是 0，是**栈地址** —— 说明执行的**不是** pl_null，
        //     是成功路径返回了脏字形指针。
        //
        //   ★ 结论：edx 的兜底值必须和 eax 保持一致 ★
        //     引擎原文：
        //         mov edx,[edi+40h]      ; 表
        //         movzx ebp,bl / shl2    ; ch*4
        //         mov eax,[edx+ebp]      ; 字形（可能 0）
        //         lea ecx,[edx+ebp]      ; 槽地址
        //         ... test eax,eax / jz / mov edx,[ecx]
        //     `0x686608 mov edx,[ecx]` **只在 eax != 0 时执行**，
        //     而那时 ecx 一定是合法槽。所以只要 eax 一旦非 0，
        //     edx/ebp 就必须是成功路径算出的那一套，绝不能用兜底值。
        //
        //   ★ 修法：失败路径不再碰 edx/ecx，只清 eax ★
        //     ecx 沿用上一次成功路径的槽地址（引擎在 eax==0 时不用它）。
        mov   edi, dword ptr [esp+0]           // 取回原始 Font*
        mov   edx, dword ptr [edi+40h]         // ★ 无条件照抄引擎语义 ★
        // ★★★ ebp 必须重算（这就是崩溃的真凶）★★★ ★★
        //   引擎原文 0x6865A9..0x6865AE：
        //       movzx ebp,bl / add ebp,ebp / add ebp,ebp   => ebp = ch*4
        //   我这 18 字节把它整段盖掉了，**每个出口都必须自己算**。
        //   之前只有成功路径算了，pl_null 漏了 → ebp 是上一轮的残留值。
        //   残留 ebp 恰好等于 0x2DC 时：
        //       edx 来自 [Font+0x40]，ecx=[edx+ebp] 是合法槽，
        //       eax=[ecx] 取出**别的字符的字形**，引擎照样去画 → 不崩但错。
        //   而 EDX=0 时 ecx=0x2DC 直接读飞 —— 转储正是这个形态。
        movzx ebp, bl
        shl   ebp, 2
        lea   ecx, [edx+ebp]                   // ★ 契约：ecx 必须是槽地址 ★
        xor   eax, eax                         // ★ 唯一必须保证的：无字形 ★
        mov   dword ptr [g_lastFontTable], 0
        mov   dword ptr [g_curChar], -1          // ★ 作废缓存：P4b 会拒收 ★

    pl_out:
        // ★ 入口没有压 edx，这里也不能 pop ★
        //   edx 由各分支负责给出**合法**的值：成功路径是 [edi+40h]，
        //   失败路径刚在上面重算过。
        pop   edi
        pop   esi
        pop   ebx
        ret
    }
}

// —— 第二处查表（0x686A26，覆盖 10 字节）——
//
//   覆盖引擎这三条连续指令：
//       0x686A26  mov eax,[esp+var_BC]   4 字节  8B 44 24 34
//       0x686A2A  mov ecx,[eax+40h]      3 字节  8B 48 40
//       0x686A2D  mov eax,[ecx+ebp]      3 字节  8B 04 29
//                                    合计   10 字节（0x686A30 − 0x686A26）
//       0x686A30  test eax,eax                 ← 下一条，不在补丁内
//
//   ★ 长度按 **IDA 指令地址差** 算，不是按编码字节数猜 ★
//     `8B 44 24 34` 是 4 字节（ModRM=44 / SIB=24 / disp8=34），
//     我曾按 3+3+3=9 算，少覆盖 1 字节后 0x686A2F 变成孤立 SIB 字节，
//     CPU 把它与后面的 85 C0 拼成 `sub [ebp-70h],eax` 去执行。
//     教训：覆盖长度一律用 IDA 的地址差。
//
//   ★ 语义：只交出 eax（字形指针），其余寄存器保持引擎原值 ★
//     引擎 0x686A30/32/34（test / jz / movss xmm0,[eax+2Ch]）都在补丁之外，
//     它们只认 eax。所以本助手不压栈、不改 edi/esi/ebx。
__declspec(naked) void __cdecl PathD_GlyphLookup2() {
    __asm {
        // ★★ 只读缓存，**一个指针都不解引用** ★★
        //   实测崩溃（hta.exe0079）：flush 路径上 var_BC 不是有效 Font*，
        //   我上一版写的 `mov ecx,[edi+40h]` 读 0x419FBEC0 直接 AV，
        //   EDI=0x419FBE80 / EAX=0x50，三种区间判据全都拦不住。
        //   ⇒ 这里既不碰 var_BC，也不碰 var_9C，更不依赖栈偏移。
        //
        //   ★★ 为什么要「字符校验」★★
        //     0x686A13（喂 P4b 的 `cmp bl,20h`）的唯一前驱是 0x686A11 ——
        //     那是 0x6869F0 那个 4 轮 flush 循环的循环尾，由 0x686728/
        //     0x6868C1 进入，**整条路径都没经过 P4**，缓存是陈的。
        //     而 bl 在 P4（0x6865A2）到 P4b（0x686A13）之间**没有任何写入**
        //     （该区间只碰 xmm / eax / edx / ecx / edi，见 0x6865B8..0x686A13），
        //     所以同一轮迭代里 bl 就是同一个字符 —— 比对 bl 即可确认缓存新鲜。
        //
        //   ★ 失败时的语义 ★
        //     返回 0 -> 引擎 0x686A32 `jz 0x686A3B` -> 推进量记 0。
        //     flush 那几轮不推进像素，最多少几个字符的宽度；
        //     比崩溃或画错字好得多。
        mov   eax, dword ptr [g_curGlyph]     // P4 本轮查到的字形
        cmp   byte ptr [g_curChar], bl        // ★ 必须确认是同一个字符 ★
        je    p2_ok
        xor   eax, eax                        // 陈旧/无效 -> 不推进
    p2_ok:
        ret
    }
}

// —— P5：绘制主遍历（9 字节位置 @0x686A52）——
//
//   覆盖的是 `xorps xmm0,xmm0 / add esi,1 / add edi,1`，call 返回后落到
//   0x686A5B（`cmp esi,[esp+var_84]`）继续，然后 0x686A63 `jl` 回 0x6864D5。
//
//   ★★ 这里有 **5 个跳入点**，寄存器状态并不一致：★★
//     0x6864ED  jmp  （字符 < 0x20，控制符）
//     0x686507  jle  （'@' 且没到截断位置）
//     0x68658B  jmp
//     0x68659C  jz
//     0x686A4E  经 `mov eax,[esp+var_9C]` 落下来（转义路径）
//
//   ★ 实测崩溃（hta.exe0036）：0xC0000096 @0x686A5E，
//     寄存器 ESI=0x09、EDI=0x11、EAX=0x4A2804DC。
//     EAX 是文本基址没错，但 **EAX 并非在所有跳入点都有效** ——
//     只有 0x686A4E 那条会重新 `mov eax,[esp+var_9C]` 刷新它，
//     其余 4 条沿用的是上一次迭代残留的 eax。
//     所以「靠 eax 取字符」在残留路径上会读到垃圾地址。
//
//   ★ 正确做法：只看 bl ★
//     跳入前的 0x686A13 `cmp bl,20h` 证明 bl 在这些路径上都是当前字符，
//     不依赖任何栈偏移或残留寄存器。
//
//   ★ 双字节推进 ★
//     GBK 前导字节 >= 0x81 时，esi（文本游标）与 edi（输出游标）
//     都要 +2，否则后续字符整体错位一格。
__declspec(naked) void __cdecl PathD_DrawAdvance() {
    __asm {
        xorps xmm0, xmm0               // 原指令 0x686A52：清累加器
        cmp   bl, 81h                   // GBK 前导字节？
        jb    da_ascii
        add   esi, 2                    // 双字节：文本游标 +2
        add   edi, 2                    // 输出游标 +2
        ret
    da_ascii:
        inc   esi                       // 原指令 0x686A55
        inc   edi                       // 原指令 0x686A58
        ret
    }
}

// —— P7：度量遍历（3 字节位置）——
//    这里空间只有 3 字节，无法容下 call。改由 InstallPatches 直接改成 add edi,2。
//    保留此助手以备后续重定位方案使用。
__declspec(naked) void __cdecl PathD_MeasureAdvance() {
    __asm {
        push eax
        movzx eax, byte ptr [edi]
        inc  edi
        cmp  eax, 81h
        jb   ma_done
        inc  edi
    ma_done:
        ret
    }
}

// —— P2/P3：预扫描遍历（5 字节位置：add edi,1 + jmp）——
//
//    ★★★ 这两个补丁已实测有害，全部退回原样 ★★★
//      崩溃 hta.exe0057（08:4x）：0x66435E61 落在 P2 helper 内部。
//      崩因是 `add esp,4 / jmp eax` 这个「丢掉返回地址尾跳循环头」
//      的技巧：循环头地址是安装时算的，一旦算错就跳进别的指令，
//      而且它同时改变了 esp 的语义，后续所有 [esp+X] 全错。
//      这是「聪明但危险」的写法，代价远大于收益。
//
//    ★ 保留的只有「双字节推进」这一件有价值的事 ★
//      `inc edi` → `add edi,2`（当首字节 >= 0x81）。
//      原指令就是 `add edi,1 / jmp`，所以只需把 add 1 换成 add 2，
//      **不改变控制流、不碰栈**，风险降到最低。
//      而 edi 是不是文本游标已经不重要 —— 我们只需要「跳 2 还是跳 1」，
//      判据用 [edi] 的首字节。若 edi 不可读，用 P5 同样的 bl 不可得
//      （预扫里没有 bl），故保留范围校验。
__declspec(naked) void __cdecl PathD_PrepassAdvanceA() {
    __asm {
        // ★★ 寄存器语义（IDA 反汇编实证，0x6861C7 起的循环）★★
        //     0x6861CF  mov eax,edx          ; edx = **字符串基址**
        //     0x6861C5  xor edi,edi          ; edi = **索引**（从 0 开始）
        //     0x6861EA  movzx ecx,[edi+edx]  ; ★ 当前字符 = 串[edi] ★
        //     0x68620D  add edi,1            ; ← P2
        //     0x686210  jmp 0x6861C7         ; ← 回循环头
        //   所以**正确读法是 [edi+edx]**，不是 [edi]。
        //   我一度改成 [edi]（以为 edi 是游标），那是错的：
        //   edi 是索引，直接当指针读会读到 0x0000xxxx 这种未提交页。
        push  eax
        movzx eax, byte ptr [edi+edx]   // ★ edi=索引 + edx=串基址 ★
        cmp   eax, 81h
        jb    pa_ascii
        add   edi, 2                    // GBK 双字节
        jmp   pa_out
    pa_ascii:
        inc   edi
    pa_out:
        pop   eax
        // ★ 必须**尾跳回循环头**，不能用 ret ★
        //   原指令是 `add edi,1 / jmp 0x6861C7`。
        //   call 压的返回地址指向 0x686212（"无字形"分支的中间），
        //   直接 ret 会从那里继续、重复累加宽度，把预扫循环结构搞坏。
        //   实测后果：0x80000003 堆魔数断言（sub_748EE0 是 free()）。
        mov   eax, dword ptr [PathD_PrepassTopA]
        test  eax, eax
        jz    pa_noTop
        add   esp, 4                    // 丢掉 call 压的返回地址
        jmp   eax                       // 尾跳回循环头
    pa_noTop:
        ret
    }
}

__declspec(naked) void __cdecl PathD_PrepassAdvanceB() {
    __asm {
        push  eax
        movzx eax, byte ptr [edi+edx]   // ★ 同 A ★
        cmp   eax, 81h
        jb    pb_ascii
        add   edi, 2
        jmp   pb_out
    pb_ascii:
        inc   edi
    pb_out:
        pop   eax
        mov   eax, dword ptr [PathD_PrepassTopB]
        test  eax, eax
        jz    pb_noTop
        add   esp, 4
        jmp   eax
    pb_noTop:
        ret
    }
}

} // extern "C"
