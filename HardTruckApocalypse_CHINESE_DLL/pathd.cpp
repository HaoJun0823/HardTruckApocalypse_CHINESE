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

// —— P7/P8：度量路径（sub_685990）——
//   ★★ 这两个是 **jmp 进入**（不是 call），所以**栈上没有返回地址**。
//      助手里的 [esp+X] 与引擎循环体内完全同基准，可直接用字节 disp ★★
extern "C" void __cdecl PathD_MsrAdvanceW();   // P7 宽度
extern "C" void __cdecl PathD_MsrAdvanceB();   // P8 双字节推进

// 度量循环的两个绝对回跳目标 + P7 落点（P7/P8 助手要跳回去）
//   循环头 0x685A80 / 退出 0x685C12 / P7 落点 0x685B2B（相对基址在 InstallPatches 里算）
//   ★ x86 不允许 `jmp dword ptr [标号]`（内存间接到控制转移），必须经寄存器中转。
extern "C" uint32_t g_msrLoopHead = 0;
extern "C" uint32_t g_msrLoopExit = 0;
extern "C" uint32_t g_msrAfterW   = 0;
// P7 扫槽表时暂存 GBK 码（ecx 要留给 Font*，x86 也没有两个内存操作数的 cmp）
extern "C" uint32_t g_msrGbk = 0;
// P7 一次性诊断：记录前几次调用看到的 Font* / 字符 / 表指针 / 字形指针。
//   汇编里没法调 Logf（会破坏寄存器与浮点栈），所以只落几个数，
//   由装配线程或下一次日志点统一打印。
extern "C" uint32_t g_msrDiagFont  = 0;
extern "C" uint32_t g_msrDiagChar  = 0;
extern "C" uint32_t g_msrDiagTable = 0;
extern "C" uint32_t g_msrDiagGlyph = 0;
extern "C" uint32_t g_msrDiagCount = 0;
// 分类失败计数：汉字分支到底卡在哪一步（此前"表=0 字形=0"是诊断漏赋值，
// 不是真实故障 —— 必须能区分「没命中槽」「槽未就绪」「表空」「字形空」）
extern "C" uint32_t g_msrFailNotReady = 0;
extern "C" uint32_t g_msrFailNoTable  = 0;
extern "C" uint32_t g_msrFailNoGlyph  = 0;
extern "C" uint32_t g_msrFailNoSlot   = 0;
// ★ 成功/失败分支的**总**计数（自进程启动累计）★
//   只有"装配完成后"的差值才有意义 —— 装配本身要跑好几秒，
//   期间槽还没填完，失败计数必然很大（实测"未就绪=352"）。
extern "C" uint32_t g_msrHitCjk   = 0;   // 汉字分支成功
extern "C" uint32_t g_msrHitAscii = 0;   // ASCII 分支成功
extern "C" uint32_t g_msrHitNoDef = 0;   // 缺字出口
// 装配完成瞬间的快照：用 (当前 - 快照) 得到装配后的真实分布
extern "C" uint32_t g_msrSnapHitCjk = 0;
extern "C" uint32_t g_msrSnapNoDef  = 0;
extern "C" uint32_t g_msrSnapNotReady = 0;
// 装配后的缺字计数（由 mw_nodef 直接累加，装配收尾时清零重新开始）
extern "C" uint32_t g_msrPostNoDef   = 0;
extern "C" uint32_t g_msrPostHitCjk  = 0;
// 绘制路径（P4）的诊断：区分"没命中槽"与"槽未就绪"。
//   tips 只显示英文 = 汉字查不到 → 必须知道是这两者中的哪一个。
extern "C" uint32_t g_plNoSlot = 0;
extern "C" uint32_t g_plNotReady = 0;
extern "C" uint32_t g_plLastNoSlotFont = 0;
// ★★★ 渲染线程识别（解决"启动卡 30 秒"）★★★
//
//   实测：WriteBlockSafe 每次要挂起**全部** 55 个线程，每个约 51ms
//   ⇒ 单点 ~2.8 秒，11 个点累计 30+ 秒。这就是用户说的"太卡"。
//
//   ★ 但真正危险的只有正在执行目标函数的线程 —— 也就是渲染线程 ★
//   怎么知道哪个是渲染线程？**让渲染线程自己报告**：
//   P4/P5 助手在热路径上执行，它们所在的线程就是渲染线程。
//   助手只做一次 GetCurrentThreadId 并把结果存进 g_renderTid。
//   之后 WriteBlockSafe 就只挂那一个线程 + 少数几个，而不是 55 个。
extern "C" uint32_t g_renderTid = 0;
extern "C" uint32_t g_renderTidHits = 0;
// ★ 「过场不换行」的决定性诊断 ★
//   引擎折行判定被编译器缓存在 ebp（0x685AF1 就是 `test ebp,ebp`）。
//   统计它：只有 ebp != 0 时才可能折行；ebp == 0 必然截断。
extern "C" uint32_t g_msrHasWidth = 0;   // ebp != 0（有可用行宽 -> 可折行）
extern "C" uint32_t g_msrNoWidth  = 0;   // ebp == 0（无行宽 -> 永不折行）

// 启动冻结：定义在 5.5 节（同在 pathd 命名空间内，且在匿名 namespace 之外）。
//   声明必须放在 `namespace pathd {` **之内**，否则会被当成全局名，
//   与定义（pathd::UnfreezeGameThreads）不匹配 → 「已声明但未定义」。
//   见文件下方 namespace pathd 开头的重复声明。
// 度量缺字默认宽度：与引擎 sub_66FEA0 缺字分支**同一个值**
//   实测来源：xmmword_9E6A74 + 8 = 字节 00 00 00 3E = 0.125f
extern "C" float    g_msrNoGlyphW    = 0.125f;
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

// 启动冻结的前置声明（定义在 5.5 节；SetupThread 在它之前就要用）
//   ★ g_frozen 的声明**不能**再带 static 初始化式 ★
//     `static bool g_frozen;` 在 C++ 里是**定义**（默认零初始化），
//     写两次就是重定义。所以这里用 extern 声明，定义处才是 static。
static void UnfreezeGameThreads();
extern bool g_frozen;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// 状态
// ─────────────────────────────────────────────────────────────────────────
bool     g_enabled   = false;   // 路径 D 是否启用（包文件存在才启用）
uintptr_t g_modBase  = 0;

// ★ 补丁安装状态（2026-10-07 新增）★★
//   0 = 正在安装（cave 会等它）
//   1 = 全部装好
//   2 = 装失败并已整体回滚
//
//   ★ 为什么必须有这个门 ★
//     08:35 两次实测对照，装配结果**完全一致**（10 字号 × 2079 字形齐备），
//     唯一差别是有没有补丁：
//       失败那次  装配 08:35:09~12（引擎主线程）→ 补丁 08:35:13~30 写了 17 秒未完
//       成功那次  装配 08:35:50~53            → 补丁 08:35:54~55（1.98 秒）写完
//     没有 P4/P5 补丁，引擎按 8 位查表 → 双字节汉字被拆成两个字节
//     → 字叠在一起 + 乱码。所以装配**必须排在补丁之后**，不能并行。
//     （旧的注释"补丁不参与字体加载，装配不依赖补丁，顺序正确"是错的。）
static volatile LONG g_patchState = 0;   // 见上方三种取值
#define PATCH_STATE_BUSY   0
#define PATCH_STATE_OK     1
#define PATCH_STATE_FAILED 2

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
bool        g_skipP7     = false;    // HTA_CHS_NO_P7=1   跳过 P7/P8 度量补丁（二分定位用）
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
    // ★ 必须是 volatile ★
    //   它被 InitThread 写、被引擎主线程轮询读。
    //   若只是普通 bool，编译器有权把它缓存进寄存器，
    //   于是 `while (!g_pkg.loaded && ...)` 可能永远看不到变化。
    volatile bool loaded = false;
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
// ★★★ 冻结期间的实时进度追踪（不走 Logf！）★★★
//
//   为什么需要它：
//     实测 P4/P4b 的写入有时卡 10~36 秒，而**日志里什么都看不到** ——
//     因为卡死发生在"打印"之前，诊断行根本执行不到。
//     Logf 自己每行都 FlushFileBuffers，所以不是缓冲问题，
//     而是**我们压根没走到那行代码**。
//
//   为什么不能直接用 Logf：
//     冻结期间若调用 Logf，会和被冻结线程持有的 CRT/文件锁互等 → 死锁。
//
//   ⇒ 用一个**独立的、无锁的**文件句柄，直接 WriteFile + Flush。
//     只写极短的一行（<128 字节），且**不取任何锁**：
//     即使两个线程同时写，最坏也只是两行交错，不会死锁。
//     目的只是"卡住时能知道卡在第几个线程、第几次尝试"。
static HANDLE g_progFile = INVALID_HANDLE_VALUE;

static void ProgOpen() {
    if (g_progFile != INVALID_HANDLE_VALUE) return;
    char dir[MAX_PATH] = {0};
    // 与日志同目录（插件所在目录）
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ProgOpen, &self);
    if (!GetModuleFileNameA(self, dir, sizeof(dir))) return;
    char* slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = '\0';
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\hta_chs_progress.txt", dir);
    g_progFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

// 无锁、无 CRITICAL_SECTION —— 只为"卡死时留下痕迹"
//
// ★★★ 性能教训（必须记住）★★★
//   初版对**每一行**都调 FlushFileBuffers。实测代价：
//       P4b 挂起耗时：无进度文件 147ms → 有进度文件 1360ms（**慢 9 倍**）
//   因为 55 线程 × 2 次刷盘 × 11 个补丁点 ≈ 1200 次磁盘刷写。
//   用户反馈"实在是太卡了"，根因就是这段**我自己加的诊断代码**。
//
//   ⇒ 两点修正：
//     1) **不再 FlushFileBuffers**：WriteFile 会把数据放进内核文件缓存，
//        进程即使崩溃，缓存内容依然可读（崩溃后看文件是完整的）。
//        只有 LogClose 那种正常退出才需要 Flush。
//     2) 只写关键行，不逐线程刷 —— 由调用方控制粒度。
static void ProgRaw(const char* fmt, ...) {
    if (g_progFile == INVALID_HANDLE_VALUE) return;
    char buf[192];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    DWORD w = 0;
    WriteFile(g_progFile, buf, (DWORD)n, &w, NULL);   // ★ 不 Flush，交给内核缓存 ★
}

// ── 延迟写入缓冲（2026-10-07）────────────────────────────────────────────
//
//   ★ 为什么不能在挂起窗口内调 ProgRaw ★
//   ProgRaw 看着"无锁"，但 WriteFile 进内核要走文件系统对象锁；
//   而此刻被挂起的 55 个游戏线程里，很可能有一个正持有 update 目录
//   所在卷的文件锁（比如它自己也在写存档/日志）。我们在冻结期间碰同一
//   个卷，就可能与之互等 —— 表现就是「游戏卡死 + 调试器看起来死锁」。
//
//   之前的进度文件印证了这个窗口存在：文件最后写入时间 09:58:43
//   与日志最后一行 09:58:43.156 完全一致，即进程死在挂起期间。
//
//   ⇒ 做法：**挂起期间只往内存里拼字符串，绝不碰文件系统**；
//     统一出口（恢复完所有线程之后）再一次性落盘。
//     代价是"崩在半路时看不到最后几行"，但相比死锁完全可以接受 ——
//     而且挂起点与写入点之间本来就只有一次 memcpy，丢的信息有限。
//
//   ★ 不加锁的原因 ★
//   g_progProg 只在 WriteBlockSafe 这一个线程里用（补丁安装是单线程），
//   即使将来多线程用，也只是内容交错，不会死锁 —— 绝不在这里引入
//   CRITICAL_SECTION，那才是真的会和被冻结线程互等。
struct PendingProg {
    char     buf[4][160];
    int      n;
};
static PendingProg g_pending;
static inline void ProgDefer(const char* fmt, ...) {
    if (g_pending.n >= 4) return;                       // 只留最近 4 条
    char* dst = g_pending.buf[g_pending.n];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(dst, sizeof(g_pending.buf[0]), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_pending.n++;
}
static inline void ProgFlushPending() {
    for (int i = 0; i < g_pending.n; ++i) ProgRaw("%s", g_pending.buf[i]);
    g_pending.n = 0;
}

// ★★★ 「全或无」补丁记录 ★★★
//
//   每成功写入一个补丁点，就把**原字节**存下来。若后续任何一个补丁
//   写入失败，就用这些记录把已生效的补丁全部还原 —— 绝不允许
//   "部分补丁生效"的半成品状态（实测 hta.exe0004 就是它导致的
//   巨卡/没字/崩溃：查表的补丁装了、推进的补丁没装）。
struct InstalledPatch {
    uintptr_t at;
    uint8_t   orig[64];
    size_t    len;
};
static std::vector<InstalledPatch> g_installedPatches;
// 回滚期间置 true：此时不要再记录"原字节"（那会记成已被补丁覆盖的值）
static bool g_rollingBack = false;

// 记录一次成功写入（WriteBlockSafe 成功后由调用方登记）
static void RecordInstalled(uintptr_t at, size_t len) {
    if (len == 0 || len > 64) return;
    InstalledPatch ip;
    ip.at = at;
    ip.len = len;
    memcpy(ip.orig, (const void*)at, len);   // ★ 必须在写入**之前**调用 ★
    g_installedPatches.push_back(ip);
}

bool WriteBlockSafe(uintptr_t at, const uint8_t* buf, size_t n, const char* what) {
    DWORD old = 0;
    // ★ 2026-10-07 计时诊断 ★
    //   实测 P4b 写入耗时 2.71 秒，而进度文件显示「挂起 0ms / 校验 0ms /
    //   写入成功 attempt=0」—— 三步都秒完成，2.71 秒必然花在**循环之外**。
    //   WriteBlockSafe 里循环之外只有两件事：开头的 VirtualProtect 和
    //   结尾的 VirtualProtect + 恢复线程。
    //   头号嫌疑是 VirtualProtect 改页权限触发写时复制 + TLB 刷新 +
    //   该页上所有线程的指令缓存失效（0x686xxx 是被 55 个线程反复执行的
    //   引擎代码页）；x64dbg 下更会放大 —— 这与「开调试器反而死锁」吻合。
    //   ⇒ 这里把两处 VirtualProtect 各自计时，下次日志直接给出答案。
    const DWORD tVpEntry0 = GetTickCount();
    if (!VirtualProtect((LPVOID)at, n, PAGE_EXECUTE_READWRITE, &old)) {
        Logf("pathd: [%s] VirtualProtect 失败 0x%08X (%lu)", what, (unsigned)at, GetLastError());
        return false;
    }
    const DWORD tVpEntry = GetTickCount() - tVpEntry0;

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
    // ★★★ 实测修正（2026-10-07，progress 文件 988 行给出决定性数据）★★★
    //
    //   我把这个预算设成 3000ms 是**错的**，它把正常操作误判成卡死，
    //   导致 7 个补丁点全部"写入失败"，最终 `成功 3，失败 4`：
    //       [jmp (控制符 <0x20)] ★写入失败★ 重试 0 次/超时 3047ms
    //       [P5 主遍历]          ★写入失败★ 重试 0 次/超时 3079ms
    //       [P7 度量宽度]        ★写入失败★ 重试 0 次/超时 3078ms
    //       [P8 度量推进]        ★写入失败★ 重试 0 次/超时 3094ms
    //       ...
    //   而成功那几点的挂起耗时是 1485 / 1531 / 2859ms。
    //   ⇒ **挂起 55 个线程本来就要 1.5~3.1 秒**（每个 OpenThread+
    //     SuspendThread 约 55ms），3000ms 的预算正好横在正常波动中间。
    //
    //   后果最严重的是"部分补丁生效"这个半成品状态：
    //     P4/P4b（查表）装上了，P5/P7/P8（推进/度量）没装上
    //     → 引擎按新逻辑查我们**没准备好**的表 → 巨卡 + 没字 + 崩溃。
    //
    //   ★ 两条修正 ★
    //   1) 预算给足：挂起阶段允许 20 秒（正常 3 秒，留 6 倍余量）
    //   2) 超时**不再放弃补丁**，而是继续用已挂起的线程去写 ——
    //      少挂几个线程的风险，远小于"补丁装一半"的风险
    const DWORD kTotalBudgetMs    = 30000;   // 重试循环总预算（写给"写不进去"）
    const DWORD kSuspendBudgetMs  = 20000;   // ★ 挂起阶段单独预算（新增语义）★
    int attempt = 0;
    bool wrote = false;
    uintptr_t blockerEip = 0;

    // ★★★ 实测根因（2026-10-07，用户提供"正常1 vs 俄语乱码"两份日志）★★★
    //
    //   同一个补丁点（P4b @0x686A26），写入耗时：
    //       正常1   :   147 ms
    //       俄语乱码: 36457 ms     ← 差 248 倍
    //   而 kTotalBudgetMs = 3000 并**没有**拦住它 —— 36 秒不在重试循环里
    //   （40 次重试 + 递增 Sleep 最多几百毫秒），而是**单次 SuspendThread
    //   就阻塞了几十秒**：目标线程若正持有内核对象（加载锁/堆锁/CRT 锁），
    //   SuspendThread 可能长时间不返回；它一卡，游戏主线程也随之停住。
    //   ⇒ 这解释了「同样等待、结果不同」，**与你等多久无关**。
    //
    //   ★ 修法方向（本轮**只加计时诊断，不改写入策略**）★
    //   我一度想改成"只挂 8 个线程就写"，但那有安全漏洞：
    //   漏检的那个线程若 EIP 正落在 [at-15, at)，补丁后必崩。
    //   在**没有实测数据证明 36 秒花在哪一步之前**，不动安全边界。
    //   下面给挂起/校验两个阶段分别计时，下一轮日志就能定位。
    //
    //   ★ 安全边界不变 ★：EIP ∈ [at-15, at) 仍然拒绝写入。

    DWORD tSuspendTotal = 0, tCheckTotal = 0;   // 诊断：两阶段各耗多久
    // 挂起超时的延迟上报（不能在冻结窗口内 Logf，理由见下方使用处）
    bool        g_abortWarn = false;
    const char* g_abortWhat  = nullptr;
    DWORD       g_abortMs    = 0;
    int         g_abortHeldN = 0;
    ProgOpen();                                  // 打开"卡死也能留痕"的进度文件

    for (attempt = 0; attempt < 40; ++attempt) {
        heldN = 0;
        // ★ 挂起阶段的独立预算 ★
        //   实测 P4b 曾卡 36 秒，而 kTotalBudgetMs=3000 根本拦不住 ——
        //   因为耗时发生在**单次 SuspendThread** 内部，不在重试循环里。
        //   这里给"挂起全部线程"这一步也加上预算：一旦超时，
        //   立即停止继续挂起、恢复已挂起的、放弃这次尝试。
        const DWORD tSuspend0 = GetTickCount();
        bool suspendAborted = false;
        DWORD tOneSuspendMax = 0;      // 诊断：单次 SuspendThread 最长耗时
        DWORD tOpenMax = 0;            // 诊断：单次 OpenThread 最长耗时
        ProgRaw("=== %s @0x%08X attempt=%d tidN=%d ===\n",
                what, (unsigned)at, attempt, tidN);
        for (int i = 0; i < tidN; ++i) {
            // ★ 用独立的挂起预算（20 秒），不再用重试总预算 ★
            //   实测正常挂起就要 1.5~3.1 秒；超时也只停止"继续挂"，
            //   并用**已经挂起的那些**继续走校验 + 写入（见下方）。
            if (GetTickCount() - tSuspend0 > kSuspendBudgetMs) { suspendAborted = true; break; }
            // ★ 不再逐线程写进度文件 ★
            //   那是"太卡"的元凶：55 线程 × 2 次刷盘 = 110 次磁盘操作/补丁点。
            //   现在只在**单个线程挂起超过 200ms**时才记一行 —— 那才是
            //   真正的异常（正常每线程约 7ms）。
            DWORD ta = GetTickCount();
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, tids[i]);
            DWORD tb = GetTickCount();
            if (tb - ta > tOpenMax) tOpenMax = tb - ta;
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            DWORD tc = GetTickCount();
            if (tc - tb > tOneSuspendMax) tOneSuspendMax = tc - tb;
            if (tc - ta > 200) {
                ProgDefer("  慢! #%d tid=%lu Open=%lums Suspend=%lums\n",
                        i, (unsigned long)tids[i],
                        (unsigned long)(tb - ta), (unsigned long)(tc - tb));
            }
            held[heldN++] = th;
        }
        ProgDefer("  挂起完成 %d 个 %lums\n", heldN,
                (unsigned long)(GetTickCount() - tSuspend0));
        tSuspendTotal += GetTickCount() - tSuspend0;
        if (tOneSuspendMax > 200 || tOpenMax > 200) {
            // 只在真的出现长阻塞时打印（避免污染日志）
            Logf("pathd:   [计时] %s 第%d轮 挂起阶段 %u ms（OpenThread 峰值 %u ms，SuspendThread 峰值 %u ms，线程数 %d）",
                 what, attempt, (unsigned)(GetTickCount() - tSuspend0),
                 (unsigned)tOpenMax, (unsigned)tOneSuspendMax, tidN);
        }
        if (suspendAborted) {
            // ★★★ 关键修正：超时**不再放弃补丁** ★★★
            //
            //   旧代码在这里 `break` 直接返回 false —— 那会让这个补丁
            //   **完全不装**，而前面几个补丁已经生效，于是引擎处于
            //   "查新表但没人填/推进逻辑不配套"的半成品状态：
            //   实测表现就是巨卡 + 没字 + 崩溃（hta.exe0004）。
            //
            //   现在改为：带着**已经挂起的那些线程**继续校验并写入。
            //   少挂几个线程确实有残余风险（未挂起的线程若正好 EIP 落在
            //   [at-15, at)），但那个风险远小于"补丁装一半"——
            //   后者是**必然**的严重破坏，前者是小概率且可用重试缓解。
            //   ★ 这里**绝不能**调 Logf ★
            //     Logf 每行都 FlushFileBuffers，要进 CRT 与文件系统。
            //     而此刻 55 个游戏线程已被挂起，其中任何一个都可能正持有
            //     CRT 锁或卷锁 —— 我们在同一把锁上等，就成了真死锁。
            //     这不是理论风险：用户实测「开 x64dbg 时会死锁在补丁里」，
            //     而进度文件的最后写入时间与日志最后一行完全一致，
            //     说明进程确实死在冻结窗口内。
            //   ⇒ 记下标志，等统一出口恢复完线程后再补打日志。
            suspendAborted = true;
            g_abortWarn = true;
            g_abortWhat  = what;
            g_abortMs    = GetTickCount() - tSuspend0;
            g_abortHeldN = heldN;
            ProgDefer("  挂起超时，用已挂起的 %d 个继续\n", heldN);
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
        DWORD tChk0 = GetTickCount();
        ProgDefer("  校验开始（%d 个已挂起，危险区 [0x%08X,0x%08X)）\n",
                heldN, (unsigned)loGuard, (unsigned)at);
        for (int i = 0; i < heldN && !busy; ++i) {
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(held[i], &c)) continue;
            uintptr_t eip = (uintptr_t)c.Eip;
            if (eip >= loGuard && eip < at) { busy = true; blockerEip = eip; }
        }
        tCheckTotal += GetTickCount() - tChk0;
        ProgDefer("  校验完成 %lums，busy=%d blockerEip=0x%08X\n",
                (unsigned long)(GetTickCount() - tChk0), (int)busy, (unsigned)blockerEip);

        if (!busy) {
            // ★ 写入前先把原字节记下来（供"全或无"回滚用）★
            //   放在这里而不是要求每个调用方记得调用，避免遗漏。
            if (!g_rollingBack) RecordInstalled(at, n);
            memcpy((void*)at, buf, n);
            FlushInstructionCache(GetCurrentProcess(), (LPVOID)at, n);
            wrote = true;
            ProgDefer("  ★写入完成 attempt=%d\n", attempt);
            break;
        }
        ProgDefer("  busy，恢复线程重试\n");

        // ★ 重试节流 ★
        //   实测（05:19 那份"俄文乱码然后空白"日志）：P4b 写入用 8 秒、
        //   P3 用 4 秒，十几个补丁点累积几十秒 —— 用户等不及就关了游戏，
        //   于是停在"补丁装了一半"的中间态（= 乱码 + 空白）。
        //
        //   慢的根因：每次重试都 SuspendThread **全部**线程（几十个），
        //   再逐个 GetThreadContext。而真正可能挡路的只有**正好执行到
        //   目标函数**的那个渲染线程。
        //
        //   这里把重试间隔从固定 1ms 改成递增，让"短暂冲突"能很快过去，
        //   同时总预算不变 —— 不改变安全性，只减少无谓的全量挂起次数。
        for (int i = 0; i < heldN; ++i) { ResumeThread(held[i]); CloseHandle(held[i]); }
        heldN = 0;
        if (GetTickCount() - t0 > kTotalBudgetMs) break;
        // 递增间隔：前几次几乎立即重试（短暂冲突立刻过去），
        // 后面逐步拉长以免空转。上限 4ms。
        DWORD backoff = (attempt < 8) ? 0 : ((attempt < 20) ? 1 : 4);
        if (backoff) Sleep(backoff);
    }

    // ★ 统一出口：无论如何都必须恢复所有线程，否则游戏直接卡死 ★
    for (int i = 0; i < heldN; ++i) { ResumeThread(held[i]); CloseHandle(held[i]); }
    const DWORD tVpExit0 = GetTickCount();
    VirtualProtect((LPVOID)at, n, old, &old);
    const DWORD tVpExit = GetTickCount() - tVpExit0;

    // ★ 落盘时机 ★
    //   必须放在**恢复全部线程之后**。冻结期间任何文件系统访问都可能与
    //   被冻结线程持有的卷锁互等（详见 PendingProg 上方注释）。
    ProgFlushPending();

    // ★ 补打挂起超时的警告 ★
    //   现在才打是安全的：所有游戏线程都已恢复，不再有锁互等的风险。
    //   这条信息很重要（意味着"只挂了部分线程就写入"），不能丢。
    if (g_abortWarn) {
        Logf("pathd:   [警告] %s 挂起超时（%lums），用已挂起的 %d 个线程继续写入",
             g_abortWhat, (unsigned long)g_abortMs, g_abortHeldN);
        g_abortWarn = false;
    }

    if (wrote) {
        if (attempt) Logf("pathd:   %s 冻结重试 %d 次后写入成功", what, attempt);
        // ★ 计时汇总：只在异常慢时打印，用来定位"2.71 秒 / 36 秒花在哪" ★
        //   本轮新增 tVpEntry / tVpExit 两项 —— 它们是**循环之外**仅有的
        //   两个可能耗时点。加上它们，这行日志就能把总耗时完整归因。
        DWORD tot = GetTickCount() - t0;
        if (tot > 400) {
            Logf("pathd:   [计时] %s 总耗时 %u ms（改权限 %u ms + 还原权限 %u ms"
                 " + 挂起累计 %u ms + 校验累计 %u ms，重试 %d 次）",
                 what, (unsigned)tot, (unsigned)tVpEntry, (unsigned)tVpExit,
                 (unsigned)tSuspendTotal, (unsigned)tCheckTotal, attempt);
        }
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

// 把 "jmp 助手 + NOP 填充" 组装成单个缓冲区后**一次**写入
//
//   ★ 为什么度量补丁用 jmp 而不是 call ★
//     call 会把返回地址压栈，而 P7（双字节推进）挂在 `add edi,1` 上，
//     引擎原本是「顺序执行、靠末尾的 jl 回跳」。用 call 就必须在助手里
//     手工丢弃那个返回地址（`add esp,4`），而历史上 P2/P3 正是用
//     `add esp,4 / jmp eax` 尾跳崩在助手内部（hta.exe0057）。
//     jmp 不动栈，栈平衡由构造保证，栈错乱这条失败模式直接消失。
bool WriteJmpBlock(uintptr_t at, size_t total, void* target, const char* what) {
    int64_t rel = (int64_t)target - (int64_t)(at + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: %s 目标 0x%p 距离过远，无法 E9 相对跳转", what, target);
        return false;
    }
    if (total < 5)   { Logf("pathd: %s 覆盖 %u 字节 < 5，放不下 jmp", what, (unsigned)total); return false; }
    if (total > 64)  { Logf("pathd: %s 覆盖 %u 字节超缓冲", what, (unsigned)total); return false; }
    uint8_t b[64];
    b[0] = 0xE9;
    *(int32_t*)(b + 1) = (int32_t)rel;
    for (size_t i = 5; i < total; ++i) b[i] = 0x90;
    if (!WriteBlockSafe(at, b, total, what)) return false;
    Logf("pathd:   %s @0x%08X -> jmp 0x%p (rel=%d, 共 %u 字节一次写入)",
         what, (unsigned)at, target, (int)rel, (unsigned)total);
    return true;
}

// 按特征码在模块里找唯一命中；通配 '?'
//
//   ★★★ 多命中时必须拒绝，而不是取第一个 ★★★
//     实测教训（2026-10-07）：度量推进的锚点
//     "83 C7 01 3B 7C 24 ?? 0F 8C" 有 8 处命中，真正的那处在第 5 位
//     （0x685BD1）。取第一处 → 装到 0x44ED34 这个无关函数上，
//     把正常代码覆盖成 jmp → 乱码 + 字符全叠在一起。
//     函数叫 ScanUnique，就该兑现 unique 的承诺：
//     命中数 != 1 一律返回 0，让上层打印 [MISS] 并保持原样。
//     **宁可不打补丁，也不能打错位置** —— 打错会破坏无关功能，
//     而不打补丁只是"这个 bug 没修"。
//
//   ★ 但 `allowMulti` 逃生口是必需的 ★
//     引擎分配器 sub_589410 的特征码**天然**就是 2 处命中
//     （alloc 的两包装），它一直是「取第一处 + 运行时 probe 自检」的设计。
//     实测（04:29:46）我加了全局歧义拦截后，它直接返回 0，
//     日志变成「[歧义] 引擎分配器 —— 拒绝安装」，
//     结果该字号汉字无法挂页 —— 这是**我引入的回归**。
//     凡是**自带运行时自检**的目标都该传 allowMulti=true。
uintptr_t ScanUniqueEx(const char* sig, const char* what, bool allowMulti) {
    // ★ 一次扫描拿「首命中 + 总数」★
    //   早先写成 ScanModule() + CountModule()，那是**两次**全模块扫描。
    //   实测（04:37 那一轮）单次 ScanModule 就要 3.5~4.9 秒
    //   （朴素 O(n·m) 匹配 6.6MB），每个锚点扫两遍 ⇒ 初始化从基线
    //   4 秒涨到 >19 秒还没跑完。用户中途关掉了游戏，看到的是
    //   「P4 已装、表还空」的半成品 ⇒ 满屏西里尔乱码 + 叠字 + 空白。
    //   **保护机制自己变成了 bug**，必须消掉重复扫描。
    uintptr_t hit = 0;
    int n = pattern::ScanModuleCount((HMODULE)g_modBase, sig, &hit);
    if (!hit) {
        Logf("pathd: [MISS] %s  特征码 %s", what, sig);
        return 0;
    }
    if (n != 1 && !allowMulti) {
        Logf("pathd: [歧义] %s 特征码命中 **%d 处**（0x%08X 起）——拒绝安装，保持原样",
             what, n, (unsigned)hit);
        Logf("pathd:        特征码: %s", sig);
        Logf("pathd:        ★必须先把这个锚点收紧到唯一★ 打错位置会破坏无关功能");
        return 0;
    }
    Logf("pathd: %-22s -> 0x%08X  (%d 处命中%s)", what, (unsigned)hit, n,
         allowMulti && n > 1 ? "，按设计取首个 + 运行时自检" : "");
    return hit;
}

uintptr_t ScanUnique(const char* sig, const char* what, bool required = true) {
    uintptr_t hit = ScanUniqueEx(sig, what, false);
    if (!hit && !required) Logf("pathd: [可选缺失] %s", what);
    return hit;
}

// 允许「多命中取首个」的版本 —— 仅供**自带运行时自检**的目标使用
uintptr_t ScanFirst(const char* sig, const char* what) {
    return ScanUniqueEx(sig, what, true);
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
    // ★★★ 池的容量策略（改自 1100KB 起步 / 翻倍）★★★
    //
    //   实测泄漏证据（一次运行）：
    //       [07:29:53.791] pathd: 字形池 1100KB @0x1D990000
    //       [07:30:01.549] pathd: 字形池 2200KB @0x732C0000
    //   一次运行漏了 3.3MB —— 因为原策略是「起步 1.1MB，不够就**翻倍**」，
    //   而 1.1MB 只够装 ~10000 个字形（每个 48 字节），实际要装 2207×12
    //   ≈ 26500 个 → **必然触发翻倍**。
    //
    //   ★ 我原先注释里写的"泄漏上限 ~2MB"是错的 ★
    //     那个估计按"只有 10 个字号"算的。加了周期补装配后，
    //     ProcessFont 的调用次数**不再有上界**（每个运行期新建的字体都会
    //     再走一遍），于是每次调用都重新摊一份 1.1~2.2MB。
    //     → 2026-10-07 07:27 那次崩溃（hta.exe0009）就是这个：
    //       Virtual memory available 只剩 39MB，崩在引擎自己的代码里。
    //     （LAA 只是把 2GB 上限抬到 4GB，掩盖了症状，没解决泄漏。）
    //
    //   ★ 改法：按"每个槽需要多少"成组预留，一次到位 ★
    //     每字形 48 字节，每槽 2207 个 → ≈103KB / 槽。
    //     槽的硬上限是 MAX_CJK_TABLES = 16（见 CjkSlotFor / FillCjk），
    //     **不是字号的种类数**。补装配会为"同一字号的新 Font*"再开槽：
    //     实测 07:40 那次基础 10 槽 + 补装配 2 槽 = 12 槽，已超出我上次
    //     按 10 配的 kInit → 触发了第二次分配（又漏 1034KB）。
    //     → 教训：容量必须按**槽上限 16** 配，不能按"字号种类"。
    //   16 槽 × 103KB ≈ 1.6MB，一次 VirtualAlloc 到位，永不扩容。
    const size_t kPerFont = 2207u * kGlyphSize;        // ≈103 KB / 槽
    const size_t kInit    = kPerFont * MAX_CJK_TABLES;  // 16 槽 ≈1.6MB，封顶
    if (used + kGlyphSize > cap) {
        // ★ 兜底增量（正常不会走到）★
        //   万一将来 MAX_CJK_TABLES 调大或字体数超 16，再 +4 槽，不翻倍。
        const bool  bFirst = (cap == 0);      // ★ 首配 vs 扩容
        size_t ncap = bFirst ? kInit : cap + kPerFont * 4;

        // ★★★ 内存保险丝（2026-10-07 修正）★★★
        //
        //   原实现在这里对**首配也生效**，后果是实测启动直接废掉：
        //       [08:31:28] [字形池] 可用虚拟内存仅 132MB，拒绝扩容  ← 刷屏开始
        //       ...重复 500+ 行，每行对应一个字形...
        //       [08:31:41] 可用虚拟内存仅 198MB，拒绝扩容
        //       [08:31:42] 字形池 1655KB @0x11620000             ← 14 秒后才成功
        //       [08:31:42] [ 18.750] 填充 **1807** 个汉字字形      ← 应为 2079
        //
        //   两个缺陷：
        //     (a) 首配只需要 1.6MB，而判定用的是"全局可用虚拟内存"。
        //         引擎自身+D3D 在 2GB 地址空间下常把可用压到 130~200MB，
        //         这是**正常稳态**，不是危险信号 → 首配被无理由拒绝。
        //     (b) 拒绝后 cap 仍为 0，于是**每一个字形都重走这条拒绝路径**
        //         （FillCjk 的 `if (!g) continue;` 不计数也不报警），
        //         于是刷出上千行日志、并静默丢掉 272 个字形 → 汉字空白。
        //
        //   修正：
        //     1) 首配（cap==0）**不做**保险丝检查 —— 1.6MB 相对整个进程
        //        是可忽略的量，不值得为它放弃全部汉字。
        //     2) 扩容才检查，且失败日志**每次只打一条**（去重），
        //        避免上千行日志淹没真正的错误。
        //     3) 若最终连首配都没成功，FillCjk 那边会打出
        //        "缺 N 个"的明确诊断，不再静默 continue。
        if (!bFirst) {
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms) && ms.ullAvailVirtual < (200ull << 20)) {
                static bool warned = false;   // ★ 只警告一次
                if (!warned) {
                    warned = true;
                    Logf("pathd: [字形池] 可用虚拟内存仅 %uMB，**扩容**被拒（防 OOM，本次新增字形将缺失）",
                         (unsigned)(ms.ullAvailVirtual >> 20));
                }
                return nullptr;
            }
        }

        uint8_t* np = (uint8_t*)VirtualAlloc(NULL, ncap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!np) {
            static bool warnedAlloc = false;
            if (!warnedAlloc) {
                warnedAlloc = true;
                Logf("pathd: ★字形池 VirtualAlloc 失败(%u 字节) —— 汉字将全部缺失★", (unsigned)ncap);
            }
            return nullptr;
        }
        // ★★★ 旧池**绝不释放** ★★★
        //   g_cjkSlots[].table 里存的是**每个字形的原始地址**，不是偏移。
        //   一旦 VirtualFree 旧池，先前填好的所有字形指针立刻悬空 ——
        //   引擎 0x6865C7 `movss xmm1,[eax+24h]`（读字形宽高）读到的
        //   就是已释放页，直接 0xC0000005。
        //   实测崩溃 hta.exe0080：EAX=0x68948340，故障地址 0x68948364 = EAX+0x24，
        //   而 0x68948340 正是被 MEM_RELEASE 掉的旧池地址。
        //   代价：扩容仍会漏旧池。但现在扩容**极少发生**（首配 10 个字号，
        //   实测最多用到 12 个槽），所以泄漏从"每次调用都摊"
        //   降到"进程生命周期内摊一次几 MB 以内"。
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
    // ★ 丢弃分类计数（2026-10-07）：解释"填充数 < 包内字形数"的差额
    uint32_t nNoCell = 0, nBadPage = 0, nAllocFail = 0;
    for (uint32_t i = 0; i < g_pkg.glyphCount; ++i) {
        uint32_t cell = rec->cell[i];
        uint32_t page = cell / perPage;
        uint32_t pos  = cell % perPage;
        uint32_t col  = pos % cols;
        uint32_t row  = pos / cols;
        // ★ 丢弃计数（2026-10-07）★
        //   下面这一行原本是无声 continue，导致"填充 1807 / 2079"这种
        //   少 272 个字形的事故在日志里看不出少了什么、也看不出为什么。
        //   分开统计三种丢弃，让下一次出问题能一眼定性。
        if (cell == 0xFFFFFFFFu) { ++nNoCell; continue; }   // 该字不在本字号图集里
        if ((int)page >= (int)rec->pageCount) { ++nBadPage; continue; }  // ★ 异常：页号越界

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
        if (!g) { ++nAllocFail; continue; }   // ★ 不再静默丢字（见下方汇总日志）
        FillGlyph(g, gbk, basePage + (int)page, u0, v0, u1, v1, pxW, pxH, adv);
        table[idx] = (uint32_t)g;      // ★ 写进**本字体**的表（见函数头说明）★
        ++n;
    }
    g_cjkSlots[slot].ready = 1;              // ★ 填完才开闸 ★
    Logf("pathd: [%7.3f] 填充 %d 个汉字字形（单元 %ux%u，每页 %u 格，%u 页，页基址 %d，槽 %d）",
         h, n, cw, chh, perPage, rec->pageCount, basePage, (int)slot);

    // ★ 差额诊断（2026-10-07）★
    //   包内字形数与实际填充数必须对得上；对不上就把差在哪一类讲清楚。
    //   实测事故："填充 1807 / 包内 2079" 少 272 个 → 当时 AllocGlyph 因
    //   内存保险丝反复拒绝（且每次都静默 continue），于是这些汉字没有
    //   字形，界面上就是空白/缺字。现已让每一类丢弃都有计数。
    {
        const uint32_t total = (uint32_t)g_pkg.glyphCount;
        const uint32_t diff  = total - (uint32_t)n;
        if (diff != 0) {
            Logf("pathd: [%7.3f] ★字形差额 %u（包内 %u，实填 %u）："
                 "无图集格 %u  页号越界 %u  分配失败 %u",
                 h, diff, total, (uint32_t)n, nNoCell, nBadPage, nAllocFail);
        } else {
            Logf("pathd: [%7.3f] 字形齐备（%u/%u，无缺失）", h, total, (uint32_t)n);
        }
        //   分配失败是唯一可自愈的一类（其余是烘包期就该发现的数据问题）
        if (nAllocFail) {
            Logf("pathd: [%7.3f]   ★%u 个字形因内存分配失败而缺失 —— 这些汉字会显示空白★",
                 h, nAllocFail);
        }
    }
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
    // ★★ 合并区间必须限幅，否则会爆炸（实测 06:08 那次）★★
    //
    //   锚点各自 ±2MB 再取并集。**锚点一多、且分散，并集就会覆盖整个跨度**：
    //     实测绘制期登记 5 个字体，锚点跨 0x12DADB24 .. 0x1D6AB564，
    //     并集 = **169 MB** → 每 4 字节一次 LooksLikeCjkFont = 4200 万次
    //     → 装配线程卡死（用户看到的"太卡"）。
    //   正常情况只有 1~2 个锚点、彼此相邻，并集才 4MB。
    //
    //   ⇒ 按"离锚点最近"排序，只保留能覆盖在 kMaxSpan 内的锚点。
    //     宁可少扫几个字号（漏掉的字号只是没有汉字，引擎会安全跳过），
    //     也不能把装配卡死几分钟。
    const uintptr_t kMaxSpan = 8u << 20;      // 总跨度上限 8MB
    uintptr_t lo = 0, hi = 0;
    {
        // 以第一个锚点起步，逐个尝试并入；超限就丢弃该锚点
        uintptr_t a0 = anchors[0];
        lo = (a0 > kSpan) ? a0 - kSpan : 0x10000;
        hi = a0 + kSpan;
        for (size_t i = 1; i < anchors.size(); ++i) {
            uintptr_t a = anchors[i];
            uintptr_t s = (a > kSpan) ? a - kSpan : 0x10000;
            uintptr_t e = a + kSpan;
            uintptr_t nlo = (s < lo) ? s : lo;
            uintptr_t nhi = (e > hi) ? e : hi;
            if (nhi - nlo <= kMaxSpan) { lo = nlo; hi = nhi; }
            else {
                Logf("pathd: [跳过] 锚点 0x%08X 会使扫描区间超 %uMB，忽略它",
                     (unsigned)a, (unsigned)(kMaxSpan >> 20));
            }
        }
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

// ★ 装配状态：0=未装，1=已装（或正在装）。
//   ★ 定义放在这里（SetupThread 之前）★，而不是下面 5.4 节 ——
//     C++ 里 `static volatile LONG x;` 就是**定义**（零初始化），
//     在下面再写一次 `= 0` 会构成重定义错误（g_frozen 那次已踩过）。
static volatile LONG g_assembled = 0;
static int PathD_AssembleAll(const char* why);
// 前置声明：补装配线程定义在 5.3b 节（在 SetupThread 之后），
//   但 SetupThread 要调用它。注意 C++ 里 `static` 前置声明就是**定义**，
//   不能再写一份定义（g_frozen 那次已踩过这个坑）。
static DWORD WINAPI PathD_RescanThread(LPVOID);

// ═══════════════════════════════════════════════════════════════════════════
// 内存快照 + LAA 检测（2026-10-07）
// ═══════════════════════════════════════════════════════════════════════════
//
//  ★ 为什么需要 ★
//    两类崩溃的内存画像完全相反，但都缺一个关键数据：**谁在占内存**。
//      无 LAA（hta.exe0025）：vmAvail = 19MB，崩在 0x8CA1BE
//                           `call [edx+480h]` 分配顶点缓冲返回 NULL，
//                           引擎不检查就 `rep movsd` 拷 47808 字节到 NULL。
//      有 LAA（hta_laa0000）：vmAvail = 1993MB，崩在 dxrender9+188022 读 NULL。
//    MemoryManager 自报 mem used ≈ 108MB，但用户区可用只剩 19MB ——
//    差额去哪了没人知道。**先测量，再优化**，不然只能瞎改。
//
//  ★ LAA 怎么判断（踩过的坑）★
//    转储字段极易读错，务必分清：
//        Total memory                     = 4095 MB  ← **不代表 LAA**
//                                              （hta.exe0025 无 LAA 时也是 4095）
//        Total virtual memory             = 2047 MB  ← ★ 这才是用户区上限 ★
//                                              4095 = 有 LAA，2047 = 无 LAA
//        Total virtual memory available   = 实际剩余（与 LAA 无关）
//    我此前把"可用内存低"当成"LAA 关"，误判过一轮。
//    程序内的判定方式：直接探测能否在 0x90000000（2304MB）提交一页。
//    无 LAA 的 32 位进程用户区止于 0x80000000（2048MB），那里探测必然失败。

static bool DetectLargeAddressAware() {
    //   探测后立刻释放，不留任何痕迹，也不影响内存画像。
    void* p = VirtualAlloc((void*)0x90000000, 4096,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return false;
    VirtualFree(p, 0, MEM_RELEASE);
    return true;
}

// 打印一次完整内存画像
static void LogMemorySnapshot(const char* tag) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;

    const bool laa = DetectLargeAddressAware();

    Logf("pathd: ── 内存快照 [%s] ──", tag);
    Logf("pathd:   LAA(大地址感知) = %s   ← 转储里对应 Total virtual memory = %s",
         laa ? "已启用 (4095MB 用户区)" : "未启用 (2047MB 用户区)",
         laa ? "4095 MB" : "2047 MB");
    Logf("pathd:   物理内存   总 %llu MB / 可用 %llu MB",
         (unsigned long long)(ms.ullTotalPhys >> 20),
         (unsigned long long)(ms.ullAvailPhys >> 20));
    Logf("pathd:   虚拟内存   总 %llu MB / 可用 %llu MB",
         (unsigned long long)(ms.ullTotalVirtual >> 20),
         (unsigned long long)(ms.ullAvailVirtual >> 20));
    //   ★ 提交剩余才是决定"分配会不会失败"的量 ★
    //     转储里的 Total virtual memory available 就是它：
    //       无 LAA 崩溃那次 = 19MB，引擎分配顶点缓冲失败 → 返回 NULL
    //       有 LAA 崩溃那次 = 1993MB，内存充足却崩在别处
    Logf("pathd:   ★提交剩余 = %llu MB   ← 转储里的 Total virtual memory available 就是它★",
         (unsigned long long)(ms.ullAvailPageFile >> 20));
    Logf("pathd:   提交内存   上限 %llu MB / 已提交 %llu MB",
         (unsigned long long)(ms.ullTotalPageFile >> 20),
         (unsigned long long)((ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20));
    Logf("pathd:   扩展内存(仅64位有效)  可用 %llu MB",
         (unsigned long long)(ms.ullAvailExtendedVirtual >> 20));
    //   提交率是"离 OOM 还有多远"的直接指标。
    //   注意分母用**用户区上限**（ullTotalVirtual）：它同时包含了
    //   已提交页和保留地址空间，无 LAA 时是 2047MB、LAA 时是 4095MB。
    if (ms.ullTotalVirtual) {
        const unsigned long long committed = ms.ullTotalPageFile - ms.ullAvailPageFile;
        const unsigned pct = (unsigned)((committed * 100) / ms.ullTotalVirtual);
        Logf("pathd:   ★提交占用率 = %u%%（已提交 %llu MB / 用户区上限 %llu MB）"
             " ← 接近 100%% 时引擎分配会开始返回 NULL★",
             pct, committed >> 20, (unsigned long long)(ms.ullTotalVirtual >> 20));
    }

    // ── 我们自己占了多少（这是要对比的重点）────────────────────────
    int   slotsUsed = 0;
    size_t tableBytes = 0;
    for (int i = 0; i < MAX_CJK_TABLES; ++i) {
        if (g_cjkSlots[i].table) { ++slotsUsed; tableBytes += 0x10000 * sizeof(uint32_t); }
    }
    Logf("pathd:   [我们] CJK 槽 %d/%d，汉字码表 %u KB；字形池见上方“字形池”行",
         slotsUsed, MAX_CJK_TABLES, (unsigned)(tableBytes / 1024));
}

// ★ 字体加载器 hook 抓到的 FontManager ★
//
//   为什么不复用 g_fontMgr：那个变量存的是 **uiCore**（实测 0xA0A890），
//   不是 FontManager。而 FontManager 的真实来源在这里：
//       sub_6843F0:  684E48  mov ecx, [edx+4A4h]   ← ecx = FontManager
//                    684E4E  call sub_8BA480        ← 我们的 hook 点
//   也就是说 **hook 入口时 ecx 就是 FontManager**。
//   所以 cave 里第一件事就是把 ecx 存下来 —— 比事后反推可靠得多。
//
//   ★ 布局（由 sub_8BA3A0 的字体查询代码实证）★
//       FontManager+4 = vector<Font*> begin
//       FontManager+8 = vector<Font*> end
//       count = (end - begin) / 4
extern "C" uint32_t g_hookFontMgr = 0;
// 早期 hook 是否安装成功（Init 里置位，InstallPatches 里读）
static bool g_loaderHooked = false;

// ★ 旧路径（后备）：后台线程等引擎画过一帧后装配。
//   自从在字体加载器 sub_8BA480 里直接装配之后，这条路径**通常不会被执行**
//   （g_assembled 已被 hook 置 1）。保留它是因为：
//     · 万一加载器的特征码在别的版本上定位失败，还有后备
//     · 若 hook 装配失败，它仍有第二次机会（但此时时机已晚，属兜底）
static DWORD WINAPI SetupThread(LPVOID) {
    if (g_assembled) {
        Logf("pathd: [SetupThread] 加载器 hook 已完成装配，转入周期补装配");
    } else {
        Logf("pathd: [SetupThread] 后备路径启动，等待字体就绪…");
        for (int i = 0; i < 900; ++i) {               // 最多等 90 秒
            Sleep(100);
            LONG cur = g_seenCount;
            if (cur > 0 && cur >= (LONG)g_fonts.size()) break;
            if (i > 20 && g_fontMgr) break;           // 有管理器就够，不必死等
        }
        InterlockedExchange(&g_assembled, 1);
        PathD_AssembleAll("SetupThread 后备路径");
    }

    // ★★★ 周期补装配（方案 B）—— 复用这个线程，不再另开 ★★★
    //
    //   为什么不单独开线程：这个进程里 CreateThread 曾反复返回错误 8
    //   （见 Init 里那段长注释）。既然 SetupThread 已经成功跑起来了，
    //   直接让它继续做补装配，就不会再遇到"线程起不来"这条路。
    PathD_RescanThread(nullptr);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.4) 装配主体（可被两条路径复用）
// ═══════════════════════════════════════════════════════════════════════════
//
//   ★ 为什么要抽出来（架构变更 2026-10-07）★
//     原来只有 SetupThread 一条路：起线程 → 等引擎画过一帧 → 收集字体 → 装配。
//     问题是"事后追赶"：字体可能在我们装配完成**之后**才被创建
//     （实测 tips 用的 18.75 字体就是这样），于是永远没有槽 → 无中文。
//     而且 setup 与引擎并发跑，时序不确定 —— 用户看到"每次启动不一样"。
//
//   ★ 新路径：在字体加载器 sub_8BA480 里直接装配 ★
//     实测（x64dbg 断点统计）：
//         sub_8B9B60 (字体加载器)  hit = 1      ← 只调用一次
//         sub_8B80B0 (CreateFromXmlNode) hit = 99  ← 10 字号 + 89 CJK 页
//     所以在 sub_8BA480 里 `call sub_8B9B60` 返回之后装配，
//     此刻 99 个字体**全部就绪**，且引擎**还没渲染过任何文本** ——
//     引擎第一次看到字体时它就已经是完整的，不存在竞态。
//
//   两条路径都调这个函数，靠 g_assembled 保证只装一次。
//   （g_assembled 的定义在上方 SetupThread 之前）

// 从 FontManager 的 vector 直接枚举字体（**不依赖绘制期登记**）
//   这是新路径的关键：加载器返回时引擎还没画过，g_seenFonts 是空的。
//
//   ★ FontManager 来源（见 g_hookFontMgr 上方说明）★
//     hook 入口的 ecx 就是它，由 cave 存进 g_hookFontMgr。
//     布局：+4 = begin, +8 = end（sub_8BA3A0 实证）。
//   ★ 不再用 g_fontMgr+4 ★ —— 那个存的是 uiCore，不是 FontManager。
// ★★★ 路线 2（2026-10-07）：g_hookFontMgr 现在存的是 **GfxServer** ★★★
//   它由 sub_6843F0 入口 hook 抓来（ecx = this = GfxServer，无条件执行）。
//   而 FontManager = GfxServer + 0x4A4 —— IDA 0x684E48 `mov ecx,[edx+4A4h]` 实证。
//   FontManager 自身布局：+4 = begin, +8 = end（sub_8BA3A0 实证）。
//
//   ★ 为什么不能直接抓 FontManager（路线 1 的教训）★
//     sub_8BA480 只在 sub_6843F0 的 schema 解析成功时才被调用
//     （0x68444E `jz loc_684484` 提前跳走），实测偶发不执行
//     ⇒ 抓到的 FontManager 偶发为 0 ⇒ 空白字。
//     GfxServer 本身则每次 init 必到。
// ★★★ 路线 3（2026-10-07）：GfxServer 改从引擎全局直读 ★★★
//   数据源 = hta.exe 的 dword_A13CC0（RVA 0xA13CC0）。
//   ★ 为什么不再靠 hook ★
//     sub_6843F0 只在 0x5aa061 被调一次，位置在 init 的很前半段；
//     而冻结门在 0x5AA388（同函数尾部，中间还隔着十几个子系统初始化）。
//     注入线程若在此窗口内才写完 hook，那一次调用已经错过
//     ⇒ hook 报「已安装」但 g_hookFontMgr 恒为 0 ⇒ 空白字。
//   ★ 为什么直读一定有效（静态可证，非推测）★
//     写者只有两处（全镜像字节扫描穷举）：
//       A3 C0 3C A1 00        @0x594281  sub_594130  → 赋值
//       C7 05 C0 3C A1 00 ... @0x593D55  sub_593CD0  → 清零
//     赋值路径：sub_594130 ← sub_5AA440 @0x5aa464 ← sub_414DF0 @0x414dfd
//               ← WinMain @0x414cd9
//     冻结门在 sub_5A9040（WinMain @0x414d1c 才调用），严格晚于赋值。
//     清零路径：sub_593CD0 三个调用点中，
//       0x5AA27D —— 其所在块 0x5aa27a 是 0x5aa26f 的「jnz 不取」分支；
//                   而门所在块 0x5aa2a3 的唯一前驱就是那条 jnz。
//                   ⇒ 门触发 ⟹ jnz 取了 ⟹ 从未执行 0x5aa27a ⟹ 未清零。
//       0x5A0459 —— 在 sub_59FE60，仅 WinMain @0x414d71 调用，
//                   晚于 sub_5A9040 返回。
//       0x59C183 —— sub_59C180 的 xrefs_to = 0，死代码。
//     ⇒ 门触发时 dword_A13CC0 必然非 0。
//   hook 保留，仅作交叉校验与兜底。
static int CollectFontsFromManager(std::vector<FontRec>& out) {
    void* gfxHook = (void*)(uintptr_t)g_hookFontMgr;
    void* gfx = nullptr;
    // 直读引擎全局（g_modBase 非 0 才是可信基址）
    if (g_modBase)
        gfx = *(void**)((uint8_t*)g_modBase + (0xA13CC0 - 0x400000));
    if (!gfx) gfx = gfxHook;                 // 全局为空才退回 hook
    Logf("pathd: [装配] GfxServer 全局 dword_A13CC0=%p，hook=%p%s",
         gfx, gfxHook,
         (gfx && gfxHook && gfx != gfxHook) ? "（★不一致，以全局为准★）" : "");

    void* mgr = gfx ? *(void**)((uint8_t*)gfx + 0x4A4) : nullptr;
    if (!mgr) {
        // 兜底：若两条路都没拿到，退回旧推导（实测 uiCore+0x4A4 恒为 0，
        // 这里只是保留最后一条路，不指望它）
        void* uiCore = g_fontMgr;
        if (uiCore) mgr = *(void**)((uint8_t*)uiCore + 0x4A4);
        if (!mgr) {
            Logf("pathd: [装配] FontManager 不可得（全局=%p hook=%p，+0x4A4=0）",
                 gfx, gfxHook);
            return 0;
        }
        Logf("pathd: [装配] 用 uiCore+0x4A4 兜底得到 FontManager=%p", mgr);
    } else {
        Logf("pathd: [装配] FontManager=%p 直接枚举（GfxServer=%p + 0x4A4）", mgr, gfx);
    }
    Vec3* vec = (Vec3*)((uint8_t*)mgr + 4);
    if (!vec->begin || !vec->end || vec->end < vec->begin) {
        Logf("pathd: [装配] FontManager=%p 的 vector 无效（begin=%p end=%p）",
             mgr, vec->begin, vec->end);
        return 0;
    }
    size_t n = (size_t)(vec->end - vec->begin);
    if (n > 4096) n = 4096;                     // 保险上限
    int added = 0;
    for (size_t i = 0; i < n; ++i) {
        void* fp = (void*)vec->begin[i];
        if (!fp) continue;
        float h = 0.0f;
        if (!SafeFontHeight(fp, &h)) continue;
        bool dup = false;
        for (auto& r : out) if (r.font == fp) { dup = true; break; }
        if (dup) continue;
        FontRec rec; rec.font = fp; rec.height = h;
        out.push_back(rec);
        ++added;
    }
    Logf("pathd: [装配] FontManager=%p 直接枚举：vector 有 %u 项，新增 %d 个字体",
         mgr, (unsigned)n, added);
    return added;
}

// 装配主体：收集字体 → 归类 CJK 页 → 建槽 + 填字。
//   返回装入汉字的字号数；<0 表示被环境变量关闭。
static int PathD_AssembleAll(const char* why) {
    Logf("pathd: [装配] 开始（触发点：%s）", why);

    std::vector<FontRec> fonts;

    // 来源 A：FontManager 的 vector（新路径主要靠它，不依赖渲染）
    CollectFontsFromManager(fonts);

    // 来源 B：绘制期登记 + 堆扫描（旧路径的兜底；新路径下 g_seenCount 通常为 0）
    if (g_seenCount > 0) {
        std::vector<FontRec> more;
        CollectAllFonts(more);
        for (auto& r : more) {
            bool dup = false;
            for (auto& q : fonts) if (q.font == r.font) { dup = true; break; }
            if (!dup) fonts.push_back(r);
        }
    }

    Logf("pathd: [装配] 合计收集到 %u 个 Font", (unsigned)fonts.size());
    if (fonts.empty()) {
        Logf("pathd: [装配失败] 一个字体都没收集到");
        return 0;
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
        fr.height = FontHeight(fr.font);   // ★ 重新读
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
        Logf("pathd: [装配失败] 没有找到任何 CJK 图集页");
        Logf("pathd:        需要在 fonts.xml 里加 height>=900 的 Item 指向 CJK 图集");
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
    //   立刻打一次内存画像：这是"进世界前"这个关键节点的基线，
    //   与之后崩溃转储里的Total virtual memory available 对照，
    //   就能看出是我们自己占的，还是引擎/驱动占的。
    LogMemorySnapshot("装配完成（进世界前）");
    Logf("pathd: 汉字现在用 16 位索引查表（GBK 原样，不做转码）");

    // ── P7 诊断汇总 ──────────────────────────────────────────────────
    if (g_msrDiagCount > 0) {
        Logf("pathd: [P7诊断] 调用 %u 次，最后一次: Font*=0x%08X 字符=0x%04X "
             "表=0x%08X 字形=0x%08X",
             (unsigned)g_msrDiagCount, g_msrDiagFont, g_msrDiagChar,
             g_msrDiagTable, g_msrDiagGlyph);
        Logf("pathd: [P7诊断] 汉字分支失败分类(累计): 无槽=%u 未就绪=%u 无表=%u 无字形=%u",
             (unsigned)g_msrFailNoSlot, (unsigned)g_msrFailNotReady,
             (unsigned)g_msrFailNoTable, (unsigned)g_msrFailNoGlyph);
        Logf("pathd: [P7诊断] 槽数 g_cjkSlotCount=%d", (int)g_cjkSlotCount);
        Logf("pathd: [P7诊断] 分支累计: 汉字成功=%u ASCII成功=%u 缺字=%u",
             (unsigned)g_msrHitCjk, (unsigned)g_msrHitAscii, (unsigned)g_msrHitNoDef);
        Logf("pathd: [P4诊断] 绘制路径汉字: 无槽=%u 未就绪=%u",
             (unsigned)g_plNoSlot, (unsigned)g_plNotReady);
    }
    InterlockedExchange(&g_cjkReady, 1);
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.3b) 运行期补装配（方案 B）—— 修 tips 框没有汉字
// ═══════════════════════════════════════════════════════════════════════════
//
// ★ 问题 ★
//   tips 框用的是一个**运行期新建**的 18.750 字体（实测 Font*=0x12FEDB24）。
//   而 CjkSlotFor(font) 是**按 Font\* 指针精确匹配**的：
//       if (g_cjkSlots[i].font == font) return i;
//   那个字体不在已装配的 10 个槽里 → 查不到 → 走 ASCII 分支 → 显示英文。
//
// ★ 为什么以前"有时正常"★
//   那次正常运行里 g_cjkSlotCount=11 —— 引擎**恰好**在装配之后又建了一个
//   18.750 字体并被枚举到，于是各占一槽。
//   换句话说：**那是运气，不是设计**。本次（07:11）只有 10 个槽，tips 就废了。
//
// ★ 为什么按 height 回退不安全（我没选它）★
//   回退要在**裸汇编**渲染助手里加逻辑：读 [Font+0x18] 拿 height，
//   再按 height 找已装槽。裸汇编是本项目踩坑最多的地方，且 height 是 float、
//   逐位比较易出边界错误。更重要的是它只治标 —— 任何**新字号**仍然会缺。
//
// ★ 本方案（用户选定 B）★
//   后台线程周期性重扫 FontManager，发现**没登记过**的新字体就补一次
//   SafeProcessFont —— 复用已经验证过的装配路径，不碰任何裸汇编。
//   同时天然覆盖未来 DLC 引入的新字号。
//
// ★ 关键防坑（每条都对应一个具体的失败模式）★
//   1. 幂等：CjkSlotFor 已登记的直接跳过，绝不重复填表。
//   2. 槽满保护：MAX_CJK_TABLES=16，已用 10，余 6。满了就放弃并记日志，
//      不会越界写。
//   3. 首字节闸门：FillCjk 内部已有 ready=0 → 填 → ready=1，
//      渲染线程绝不会读到半成品表。
//   4. 不用锁碰渲染路径：本线程只写 g_cjkSlots，渲染线程只读。
//      槽分配用 g_cjkSlotCount++（原子自增），登记顺序是
//      **先占槽再填表**，所以渲染线程最坏看到 ready=0 而跳过，不会读到野指针。
//   5. 不在首次装配完成前跑：那时 g_pkg / pagesBySize 还不完整。
static volatile LONG g_rescanStop = 0;
static volatile LONG g_rescanRuns = 0;

static void PathD_RescanOnce(const char* why) {
    // 依赖未就绪就不跑（首次装配前、或包文件没载入）
    if (!g_engineAlloc || !g_pkg.loaded) return;
    if (InterlockedCompareExchange(&g_cjkReady, 1, 1) == 0) {
        return;                 // 首次装配还没完成
    }

    std::vector<FontRec> fonts;
    if (!CollectFontsFromManager(fonts) || fonts.empty()) return;

    // ── 先重建 CJK 页表（pagesBySize）────────────────────────────────
    //   每次都重建是对的：CJK 页字体可能也是运行期加载的，
    //   而首次装配时它们已经在（99 项里），所以重建结果与首次一致。
    std::vector<std::vector<uint32_t> > pagesBySize;
    for (auto& fr : fonts) {
        fr.height = FontHeight(fr.font);
        if (fr.height < kCjkBase || fr.height > kCjkMax) continue;
        int enc = (int)(fr.height - kCjkBase + 0.5f);
        int si = enc / kCjkStep, pi = enc % kCjkStep;
        uint8_t* f = (uint8_t*)fr.font;
        uint32_t* vec = (uint32_t*)(f + kFontOffPages);
        uint32_t cnt = (vec[1] && vec[0]) ? (vec[1] - vec[0]) / 4 : 0;
        uint32_t id = cnt ? ((uint32_t*)vec[0])[0] : 0;
        if (si >= (int)pagesBySize.size()) pagesBySize.resize(si + 1);
        if (pi >= (int)pagesBySize[si].size()) pagesBySize[si].resize(pi + 1, 0);
        pagesBySize[si][pi] = id;
    }
    if (pagesBySize.empty()) return;

    // ── 找未登记的真实字号字体，补装配 ──────────────────────────────
    int added = 0;
    for (auto& fr : fonts) {
        fr.height = FontHeight(fr.font);
        if (fr.height >= kCjkBase || fr.height <= 0.0f) continue;   // CJK 页 / 非法
        if (CjkSlotFor(fr.font) >= 0) continue;                     // ★ 已登记，幂等 ★

        int si = -1;
        for (size_t k = 0; k < g_pkg.sizes.size(); ++k)
            if (fabsf(g_pkg.sizes[k].height - fr.height) < 0.01f) { si = (int)k; break; }
        if (si < 0) continue;                       // 新字号，包文件没有 → 不处理
        if (si >= (int)pagesBySize.size() || pagesBySize[si].empty()) continue;
        if (g_cjkSlotCount >= MAX_CJK_TABLES) {
            Logf("pathd: [补装配%u] 槽已满(%d/%d)，放弃剩下的新字体",
                 (unsigned)InterlockedIncrement(&g_rescanRuns), (int)g_cjkSlotCount, MAX_CJK_TABLES);
            break;
        }
        const std::vector<uint32_t>& pv = pagesBySize[si];
        int n = SafeProcessFont(fr.font, fr.height, pv.data(), (int)pv.size());
        if (n > 0) {
            ++added;
            Logf("pathd: [补装配%u] ★新增字体 %.3f @%p（槽 %d，%d 个汉字）★",
                 (unsigned)InterlockedIncrement(&g_rescanRuns),
                 fr.height, fr.font, (int)(g_cjkSlotCount - 1), n);
        }
    }
    if (added) {
        Logf("pathd: [补装配] 本轮新增 %d 个字体，累计槽数 %d", added, (int)g_cjkSlotCount);
    }
}

static DWORD WINAPI PathD_RescanThread(LPVOID) {
    const DWORD kFirstDelayMs = 5000;    // 给首次装配留足时间
    const DWORD kIntervalMs   = 2000;
    Sleep(kFirstDelayMs);
    for (int round = 1; round <= 180 && !InterlockedCompareExchange(&g_rescanStop, 1, 1); ++round) {
        __try {
            PathD_RescanOnce("周期补装配");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("pathd: [补装配异常] 0x%08X", (unsigned)GetExceptionCode());
        }

        // ── 内存变化哨兵（2026-10-07）────────────────────────────────
        //   目的：抓"进世界读条阶段"的内存走势。转储显示崩在读条 1/4 处时
        //   可用虚拟内存只剩 19MB，但我们不知道那19MB 是谁吃掉的。
        //   做法：**只在变化显著时才记一行**，避免每2 秒刷一次日志把
        //   真正的错误淹没（v23 那次刷了 500+ 行教训）。
        //   阈值 16MB：既抓得住"几十 MB 级"的下跌，又不会被小抖动带偏。
        {
            static DWORD  lastAvailMB  = 0xFFFFFFFFu;
            static DWORD  lastCommitMB = 0xFFFFFFFFu;
            MEMORYSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms)) {
                const DWORD availMB  = (DWORD)(ms.ullAvailVirtual  >> 20);
                const DWORD commitMB = (DWORD)(ms.ullTotalPageFile >> 20);
                const DWORD dAvail = (lastAvailMB  == 0xFFFFFFFFu) ? 0u
                                 : (lastAvailMB  > availMB  ? lastAvailMB  - availMB  : 0u);
                const DWORD dCommit = (lastCommitMB == 0xFFFFFFFFu) ? 0u
                                  : (lastCommitMB > commitMB ? lastCommitMB - commitMB : 0u);
                if (lastAvailMB == 0xFFFFFFFFu || dAvail >= 16 || dCommit >= 16) {
                    Logf("pathd: [内存哨兵] 第 %d 轮：可用 %u MB（较上次 -%u）提交 %u MB（较上次 -%u）",
                         round, availMB, dAvail, commitMB, dCommit);
                }
                lastAvailMB  = availMB;
                lastCommitMB = commitMB;
            }
        }

        // 分段睡眠，好让退出能及时响应
        for (int s = 0; s < kIntervalMs / 250 && !g_rescanStop; ++s) Sleep(250);
    }
    Logf("pathd: [补装配] 线程退出（共 %u 轮）", (unsigned)g_rescanRuns);
    return 0;
}

// 卸载时叫停补装配线程。
//   ★ 只置标志，不等它退出 ★
//     DllMain 里不能 Join（会死锁：等一个正在 Sleep 的线程，
//     而它的下一段代码又要调 Logf，而 Logf 的锁可能已经在卸载路径上）。
//     标志置上后它最多再跑 250ms 就自行退出，而进程本来就要卸载了。
void PathD_StopRescan() {
    InterlockedExchange(&g_rescanStop, 1);
}

// 供 hook 调用（cdecl 无参，汇编里 call 它）
// ★ 前置声明（cave 构建器在定义之前引用它）
extern "C" void __cdecl PathD_InitExitGate();

extern "C" void __cdecl PathD_HookAssemble() {
    if (InterlockedCompareExchange(&g_assembled, 1, 0) != 0) {
        return;                 // 已经装过了，幂等
    }

    // ★★★ 必须等全部前置依赖就绪 ★★★
    //
    //   背景：字体加载器**只被调用一次**（x64dbg 实测 hit=1），错过就没第二次，
    //   所以不能"先放弃、等下次"，必须在这里阻塞等待。
    //   问题是它什么时候被调用完全由引擎决定（实测跨度从 0.3 秒到 65 秒），
    //   而我们的 InitThread 正在并行建立依赖 —— 两者赛跑，谁快谁慢全随机。
    //
    //   ★ 下面是一次修两次的记录（都因"门开了但门里没东西"）★
    //     07:03 那次：等包文件 → 漏了分配器 → 0 个字号
    //     07:16 那次：等分配器 → 漏了包文件 → 0 个字号
    //     07:24 那次：等分配器 → 漏了包文件 → 9 个字号（少一个，闪乱码）
    //
    //   装配依赖两样东西，缺一不可：
    //     (a) g_engineAlloc —— 挂 CJK 图集页表必须用引擎堆，
    //         否则引擎 ~Font 的 free 配不上（页表挂不上 → 汉字没图集 → 俄文）
    //     (b) g_pkg        —— 字号表/字形表，装配按 height 逐个匹配它
    //
    //   ★ 我上一版只等 (a)，并写下"分配器就绪蕴含包文件已载入"—— 实测证明是错的 ★
    //     我把分配器定位提到了 LoadPackage **之前**，所以顺序其实是：
    //         引擎分配器定位 → LoadPackage → 字体管理器 → 补丁
    //     分配器就绪时包文件**还早得很**。两次实测各暴露一半：
    //
    //       07:16:26 那次（只等包文件 → 漏了分配器）：
    //           07:16:26.207  包文件已就绪
    //           07:16:29.653  [ 12.000] 无引擎分配器，跳过挂页
    //           07:16:29.986  === 准备完成：0 个字号 ===
    //           07:16:30.641  引擎分配器就绪（晚了 4.4 秒）
    //
    //       07:24:03 那次（只等分配器 → 漏了包文件）：
    //           07:24:03.121  [装配] 开始
    //           07:24:07.177  [ 15.625] 包文件里没有这个字号，跳过   ← ★ 撕裂 ★
    //           07:24:07.210  === 准备完成：9 个字号 ===
    //           07:24:07.620  包文件载入成功                        ← 晚 0.4 秒
    //       15.625 正好是 g_pkg.sizes 的**最后一项**，装配扫到最后一项时
    //       LoadPackage 恰好还没写进去。**跑赢赢 10 个、跑输赢 9 个**，
    //       取决于磁盘快慢与线程调度 —— 这就是"启动后随机乱码"的来源。
    //
    //   ★ 教训：竞态的门必须覆盖**全部**前置依赖，一个都不能少 ★
    //     我两次都是"修好了 A，漏了 B"。所以这里用显式的双条件，
    //     并在注释里列出清单 —— 以后再加依赖时，照着清单补，别再靠推断。
    //     （依赖清单：g_engineAlloc、g_pkg。g_fontMgr 有 hook 抓的
    //       g_hookFontMgr 兜底，不参与本门。）
    //
    //   ★ 死锁风险已排除（实测线程归属）★
    //     装配运行在**引擎主线程**（cave 是引擎调加载器时进入的），
    //     依赖建立运行在 **InitThread**。两者不同，不会自锁。
    //     唯一的交互是 InitThread 装补丁时会挂起引擎主线程 ——
    //     那只会让装配暂停一会儿（补丁装完自动恢复），不是死锁。
    // ★ 可观测性补丁（2026-10-07）★
    //   起因：07:55:01 那次失败启动，日志最后一行停在
    //        "[SetupThread] 后备路径启动，等待字体就绪…"，此后**完全静默**，
    //        整个进程还活着但汉字没装上。没有"进入/退出装配"的日志，
    //        导致无法区分"卡在枚举里"和"根本没进装配"。
    //   这里给 cave 的每次进入/退出、以及依赖门的三种结局都留痕，
    //   下次再出现"装不上"，一眼就能看出停在哪一行。
    Logf("pathd: [装配] 进入（依赖：分配器=%p 包文件=%d 字号数=%u 补丁=%ld）",
         g_engineAlloc, (int)g_pkg.loaded, (unsigned)g_pkg.sizes.size(),
         (long)g_patchState);

    if (!g_engineAlloc || !g_pkg.loaded) {
        Logf("pathd: [装配] 依赖未就绪（分配器=%p 包文件=%d）—— 等待 Init 线程完成初始化…",
             g_engineAlloc, (int)g_pkg.loaded);
        DWORD t0 = GetTickCount();
        while ((!g_engineAlloc || !g_pkg.loaded) && (GetTickCount() - t0) < 10000) {
            Sleep(5);
        }
        DWORD waited = GetTickCount() - t0;
        if (!g_engineAlloc || !g_pkg.loaded) {
            Logf("pathd: [装配] ★等待 %u ms 后依赖仍未就绪（分配器=%p 包文件=%d），放弃★（该次无汉字）",
                 (unsigned)waited, g_engineAlloc, (int)g_pkg.loaded);
            InterlockedExchange(&g_assembled, 0);   // 让后备线程还有机会再试
            return;
        }
        Logf("pathd: [装配] 依赖已就绪（等待 %u ms，分配器=%p，字号数 %u）",
             (unsigned)waited, g_engineAlloc, (unsigned)g_pkg.sizes.size());
    }

    // ═══════════════════════════════════════════════════════════════════
    // ★★★ 第二道门：等补丁装完（2026-10-07）★★★���════════════════════════════
    //
    //   依赖清单要更新了 —— 现在是**三样**：
    //     (a) g_engineAlloc —— 挂 CJK 图集页表必须用引擎堆
    //     (b) g_pkg        —— 字号表/字形表
    //     (c) g_patchState —— ★ 新增 ★ P4/P5 查表补丁必须已就位
    //
    //   ★ 为什么 (c) 缺一不可（08:35 两次实测对照）★
    //     失败那次  08:35:09.405 装配开始 → 12.746 装完（10 字号 2079 齐备）
    //                08:35:13.315 P0撤销 → 30.260 P1撤销（写了 17 秒仍未完）
    //                → 全程没有"补丁安装完成"这一行
    //     成功那次  08:35:50.382 装配开始 → 53.906 装完（同上，也是齐备的）
    //                08:35:54.080 P1撤销 → 55.881 补丁安装完成（仅 1.98 秒）
    //
    //     **两次的装配结果完全一致**，差别只在补丁有没有写完。
    //     而没有 P4/P5，引擎就用 8 位查表 + 8 位前进量去渲染双字节汉字：
    //     前导字节和后继字节被当成两个独立字符，前进量按字节累加
    //     → 字叠在一起 + 乱码。**这才是"装不上"的真正形态。**
    //
    //   ★ 与旧注释的说法相反 ★
    //     这里原来写着"补丁不参与字体加载，装配只写数据结构不依赖补丁，
    //     所以顺序正确"。实测证明那是错的：装配产出的 64K 码表**只有
    //     引擎用 16 位索引去查它才有意义**，补丁没装 = 表白填。
    //
    //   ★ 阻塞多久 ★
    //     正常情况补丁 2 秒内装完 → 引擎主线程被扣住约 2 秒，可接受
    //     （此前是界面 17 秒不可用且最终仍失败）。
    //     设 30 秒上限只是防挂死；补丁失败时 Init 线程会弹框并退出进程，
    //     那时这里根本等不到超时。
    {
        DWORD tw0 = GetTickCount();
        while (g_patchState == PATCH_STATE_BUSY && (GetTickCount() - tw0) < 30000) {
            Sleep(5);
        }
        const DWORD waited = GetTickCount() - tw0;
        if (g_patchState != PATCH_STATE_OK) {
            Logf("pathd: [装配] ★补丁未就绪（状态=%ld，等待 %u ms），放弃装配★"
                 "（补丁缺失时填表无意义；这属于致命失败，Init 会弹框并退出）",
                 (long)g_patchState, (unsigned)waited);
            InterlockedExchange(&g_assembled, 0);
            return;
        }
        if (waited) {
            Logf("pathd: [装配] 补丁已就绪（等待 %u ms）", (unsigned)waited);
        }
    }

    __try {
        PathD_AssembleAll("字体加载器返回");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("pathd: [装配异常] 0x%08X", (unsigned)GetExceptionCode());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.4c) ★ 冻结门本体（方案 F）★
// ═══════════════════════════════════════════════════════════════════════════
//
//   触发时机：Application::init 的**成功出口**（0x5AA388，IDA 实证唯一）。
//   此刻引擎状态满足：
//     · 字体已全部加载完（sub_8BA480 已返回过）
//     · 尚未进入主循环 sub_5A8310
//     · 运行在主线程，引擎没有任何绘制在跑
//
//   ⇒ 定位可以任意慢（单核也无所谓），引擎不会撞上半装状态。
static bool g_gateDone = false;
// ★ 方案 B 交接标志 ★
//   Init() 跑完依赖准备（引擎分配器 / 包文件 / 字体管理器）后置 1。
//   冻结门只在看到这个标志才敢装补丁 —— 否则就是在半成品依赖上打补丁。
static volatile LONG g_initDepsReady = 0;

// ★★★ 方案 B：这里做**全部**工作（定位 → 写补丁 → 装配）★★★
//   Init() 只负责装冻结门 + 备好依赖，然后立刻返回；
//   引擎主线程走到 Application::init 出口时进入本函数，此时它已停住。
//
// ★ 前置声明（两者定义都在本函数之后）★
static bool InstallPatches();
static void FatalInstallFailure(const char* reason);
static bool InstallGfxServerHook(uintptr_t modBase);

extern "C" void __cdecl PathD_InitExitGate() {
    if (g_gateDone) return;                  // 幂等（理论上只会命中一次）
    g_gateDone = true;

    const DWORD t0 = GetTickCount();
    Logf("pathd: ╔═════════ [冻结门] 接管 Application::init 出口 ═════════");

    // ── 前置条件 1：Init() 的依赖准备已完成 ──────────────────────────
    //   引擎跑完 init 通常只要 1.7~4 秒，而 Init() 要 3.4 秒 ——
    //   **引擎可能先到**。
    //
    // ★★ 2026-10-07 修正：这里必须"等"，不能"立刻放弃" ★★
    //   实测失败日志（hta_chs.20261007_210533.log）：
    //       38.051  冻结门接管
    //       38.095  ★Init 依赖尚未就绪，放弃★     ← 旧版在这里直接弹框退出
    //       38.880  [方案B] 依赖已就绪             ← 只晚了 0.83 秒！
    //   而"门"的整个意义就是**让引擎主线程停在这里**（见本函数上方注释），
    //   依赖准备跑在独立的 InitThread 上（特征码扫描 / 读包文件 / 读全局，
    //   只有一个分配器自检探针会调引擎），所以原地等待不存在竞争。
    //   ⇒ 有界等待：等到了就正常装配；20 秒还等不到才是真失败。
    if (InterlockedCompareExchange(&g_initDepsReady, 1, 1) == 0) {
        const DWORD tWait0 = GetTickCount();
        Logf("pathd: [冻结门] Init 依赖尚未就绪 —— 原地等待"
             "（引擎主线程已停在门里，等待是安全的）");
        while (InterlockedCompareExchange(&g_initDepsReady, 1, 1) == 0) {
            if (GetTickCount() - tWait0 > 20000) break;
            Sleep(10);
        }
        const DWORD waited = GetTickCount() - tWait0;
        if (InterlockedCompareExchange(&g_initDepsReady, 1, 1) == 0) {
            Logf("pathd: [冻结门] ★已等待 %u ms，依赖仍未就绪，放弃★", (unsigned)waited);
            FatalInstallFailure("汉化补丁的依赖准备迟迟未完成（已等待 20 秒）");
            return;
        }
        Logf("pathd: [冻结门] 依赖就绪（等待了 %u ms），继续装配", (unsigned)waited);
    }
    if (!g_engineAlloc) {
        Logf("pathd: [冻结门] ★引擎分配器未就绪，放弃★（会导致俄文乱码）");
        FatalInstallFailure("未能定位引擎内存分配器，汉化无法挂载字形图集");
        return;
    }
    if (!g_pkg.loaded) {
        Logf("pathd: [冻结门] ★包文件未载入，放弃★（会导致无汉字）");
        FatalInstallFailure("未能载入汉化字库包文件（hta_chs_cjk.bin）");
        return;
    }

    // ── 第 1 步：安装 16 位索引补丁 ──────────────────────────────────
    //   ★ 为什么放在这里 ★
    //     旧做法在 InitThread 上装，与引擎主线程赛跑（实测只赢 0.6 秒）。
    //     现在引擎主线程**就在本函数里停着**，不存在竞争，
    //     单核慢 20 倍也无所谓 —— 游戏本来就该停在这里。
    const DWORD tPatch0 = GetTickCount();
    if (!g_skipPatch) {
        if (!InstallPatches()) {
            // 失败 = 已整体回滚。带着原版引擎进主循环时不会有汉字，
            // 但玩家会看到满屏乱码且不知为何 ⇒ 明确告知并退出。
            FatalInstallFailure("运行时代码补丁未能写入游戏进程（16 位汉字索引补丁安装失败）");
            return;
        }
        Logf("pathd: [冻结门] 补丁安装完成，耗时 %u ms",
             (unsigned)(GetTickCount() - tPatch0));
    } else {
        Logf("pathd: [冻结门] HTA_CHS_NO_PATCH 已设 —— 跳过补丁安装");
    }

    // ── 第 2 步：枚举 FontManager 装汉字 ─────────────────────────────
    //   此时字体 100% 完整（引擎刚做完 init），枚举必然成功。
    //   这也绕开了旧方案最大的软肋：冻结渲染线程会导致收集不到字体。
    __try {
        PathD_AssembleAll("冻结门");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("pathd: [冻结门] ★装配异常 0x%08X★", (unsigned)GetExceptionCode());
    }
    Logf("pathd: ╚═════════ [冻结门] 放行（总耗时 %u ms）═════════",
         (unsigned)(GetTickCount() - t0));
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.4b) 字体加载器 hook（trampoline / 代码洞）
// ═══════════════════════════════════════════════════════════════════════════
//
//   ★ 为什么选 sub_8BA480 而不是 sub_8B9B60（用户建议用代码洞重放原指令）★
//
//   sub_8BA480 的**整个函数**只有 9 字节：
//       E8 DB F6 FF FF    call sub_8B9B60     ← 5 字节，完整一条指令
//       83 E0 01          and  eax, 1
//       C3                ret
//   前 5 字节恰好是一条完整的 call —— 覆盖它**不劈开任何指令**。
//   对比 sub_8B9B60 开头是 `81 EC 84 00 00 00`（sub esp,84h，6 字节），
//   覆盖 5 字节会劈开它，必须抄两条指令并算准回跳偏移，容易出错。
//
//   ★ trampoline 结构（原函数整体搬进我们的代码洞）★
//       cave:
//           call  <sub_8B9B60>      ; 重放被覆盖的原指令，语义完全一致
//           pushad                  ; ★ 保存全部寄存器 ★
//           call  PathD_HookAssemble; ← 此刻 99 个字体已就绪，做装配
//           popad                   ; 恢复（eax 也会被恢复）
//           and   eax, 1            ; 补回原函数的第 2 条指令
//           ret                     ; 补回原函数的第 3 条指令
//
//   注意 pushad/popad **会**保存/恢复 eax，所以 call 前的 eax（= call 的
//   返回值）会被完整保留，`and eax,1` 拿到的仍是原返回值 —— 语义正确。
//
//   ★ 为什么要 pushad ★
//     PathD_HookAssemble 是 C++ 函数，会破坏 eax/ecx/edx 等。
//     而原函数返回后调用方（sub_6843F0）依赖 eax（`and eax,1` 的结果）。
//     pushad/popad 把 8 个通用寄存器全部保护起来。
//
//   ★ cave 放在哪 ★
//     就地取材：直接在 sub_8BA480 之后（0x8BA489 起是 7 字节 CC 填充区，
//     不够放）。所以改用 DLL 内自建的可执行内存 —— 用 VirtualAlloc 申请
//     PAGE_EXECUTE_READWRITE，把上述机器码写进去。
//     （DLL 自己的 .text 是只读的，运行时改属性也行，但新建一块更干净。）
static void* g_fontHookCave = nullptr;

// 生成 cave 的机器码。返回 cave 地址（失败返回 nullptr）。
static void* BuildFontLoaderCave(uintptr_t callTarget) {
    // cave 布局（手工汇编，逐字节可控）：
    //   B8 <imm32>            mov eax, callTarget
    //   FF D0                 call eax                  ; 重放原 call
    //   83 E0 01              and eax, 1
    //   C3                    ret
    //
    // ★★ 2026-10-07 路线 2：这里**只重放原指令，不再抓任何指针** ★★
    //   抓 FontManager 的职责已移到 sub_6843F0 入口（InstallGfxServerHook），
    //   因为 sub_8BA480 只在 schema 解析成功时才被调用（实测偶发不执行）。
    //   保留这个 hook 只是为了不再动它 —— 它现在是纯 trampoline。
    uint8_t code[48];
    size_t  n = 0;
    // mov eax, callTarget
    code[n++] = 0xB8;
    *(uint32_t*)(code + n) = (uint32_t)callTarget; n += 4;
    code[n++] = 0xFF; code[n++] = 0xD0;                       // call eax
    code[n++] = 0x83; code[n++] = 0xE0; code[n++] = 0x01;      // and eax,1
    code[n++] = 0xC3;                                          // ret

    void* cave = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!cave) {
        Logf("pathd: [字体加载器hook] VirtualAlloc 失败");
        return nullptr;
    }
    memcpy(cave, code, n);
    FlushInstructionCache(GetCurrentProcess(), cave, n);
    Logf("pathd: [字体加载器hook] cave @0x%08X（%u 字节，仅重放原 call + 存 FontManager）",
         (unsigned)(uintptr_t)cave, (unsigned)n);
    return cave;
}

// ═══════════════════════════════════════════════════════════════════════════
// ★★★ 路线 2：FontManager 抓取点前移到 sub_6843F0 入口（2026-10-07）★★★
// ═══════════════════════════════════════════════════════════════════════════
//
// ── 为什么必须换点（实测 103904.log 失败 / 103918.log 成功）───────────
//   两次的 hook 装好→冻结门接管间隔几乎相同（3.36s vs 3.17s），
//   结果却一个 `FontManager=130A133C`、一个 `g_hookFontMgr=0`。
//   ⇒ 不是我们的初始化慢，是 **sub_8BA480 有时根本不执行**。
//
// ── IDA 实证的根因 ────────────────────────────────────────────────────
//   sub_6843F0 是 **schema 驱动的 UI 构建**，开头：
//       0x684401  mov ecx,[eax]          ; eax = arg_0 = schema 名
//       0x684406  cmp ecx,edi / jz ...   ; 空名 → 提前返回
//       0x684441  call sub_425DF0        ; ★ 在 schema 资源里查控件
//       0x684446  mov esi,eax
//       0x68444A  mov [var_4C],esi
//       0x68444E  jz  loc_684484         ; ★★ 查不到 → 不往下走 ★★
//   而抓 FontManager 的那句在很后面：
//       0x684E44  mov edx,[var_5C]       ; edx = GfxServer
//       0x684E48  mov ecx,[edx+4A4h]     ; ★ ecx = FontManager
//       0x684E4E  call sub_8BA480        ; ← 旧 hook 点
//   schema 解析失败 ⇒ 永远走不到 0x684E4E ⇒ 旧 hook 永不触发 ⇒ FontManager 丢失。
//
// ── 新点为什么必定成功 ────────────────────────────────────────────────
//   sub_6843F0 的**入口**：
//       0x6843F0  81 EC 8C 00 00 00     sub esp, 8Ch
//       0x6843FF  8B D9                 mov ebx, ecx     ← ecx = GfxServer
//   它由 sub_58DC50 无条件调用（Application::init 必经），入口处 ecx 已在手上，
//   且 `FontManager = GfxServer + 0x4A4` 在**任何分支之前**就成立。
//
// ── 入口字节与覆盖窗口 ────────────────────────────────────────────────
//   实测字节：
//       0x6843F0  81 EC 8C 00 00 00   (6B) sub esp,8Ch
//       0x6843F6  8B 84 24 90 00 00 00 (7B)
//   首条指令 6 字节 > 5 ⇒ 5 字节窗口会劈开它，
//   所以覆盖 **6 字节**（E9 rel32 + 1 字节 NOP），cave 原样重放那 6 字节。
//
// ── cave 布局 ────────────────────────────────────────────────────────
//   81 EC 8C 00 00 00        sub esp, 8Ch        ; ★ 原样重放被覆盖的指令
//   89 0D <imm32>            mov [g_gfxServer],ecx; ★ 抓 GfxServer（this）
//   E9 <rel32>               jmp 0x6843F6         ; 继续原函数
//   —— 只做一次「存指针」，不调任何 C++ 函数，无 CRT 锁风险。

static void* g_gfxCave = nullptr;

// 在 sub_6843F0 入口安装「抓 GfxServer」hook。返回是否成功。
// ★ 路线 3 起：本 hook 已降级为「交叉校验 + 兜底」，不再是唯一数据源。★
//   权威数据源是引擎全局 dword_A13CC0（见 CollectFontsFromManager）。
//   保留它有两个用处：1) 与全局对照，能立刻暴露偏移/版本错配；
//   2) 万一全局被意外清零，还有一条后路。
static bool InstallGfxServerHook(uintptr_t modBase) {
    const uintptr_t at = modBase + (0x6843F0 - 0x400000);
    const uintptr_t back = at + 6;               // 重放 6 字节后继续的位置

    // ★ 逐字节校验：必须是 `81 EC 8C 00 00 00` ★
    //   只认这一条，不盲写 —— 写错会把 UI 构建变成崩溃。
    static const uint8_t kWant[6] = { 0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00 };
    for (int i = 0; i < 6; ++i) {
        if (rd8(at + i) != kWant[i]) {
            Logf("pathd: [GfxServer hook] ★0x%08X 字节 %s ≠ 81 EC 8C 00 00 00，拒绝★",
                 (unsigned)at, HexDump(at, 6).c_str());
            return false;
        }
    }

    void* cave = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!cave) { Logf("pathd: [GfxServer hook] VirtualAlloc 失败"); return nullptr; }
    uint8_t* c = (uint8_t*)cave;
    size_t n = 0;
    c[n++] = 0x81; c[n++] = 0xEC; c[n++] = 0x8C; c[n++] = 0x00;
    c[n++] = 0x00; c[n++] = 0x00;                 // sub esp, 8Ch（原样重放）
    c[n++] = 0x89; c[n++] = 0x0D;                // mov [g_gfxServer], ecx
    *(uint32_t*)(c + n) = (uint32_t)(uintptr_t)&g_hookFontMgr; n += 4;
    c[n++] = 0xE9;                               // jmp back
    *(uint32_t*)(c + n) = 0;                     // 回填
    const size_t relOff = n; n += 4;
    *(uint32_t*)(c + relOff) =
        (uint32_t)(int32_t)((int64_t)back - (int64_t)(c + n));
    FlushInstructionCache(GetCurrentProcess(), cave, n);

    g_gfxCave = cave;

    // 写 6 字节：E9 rel32 + NOP
    int64_t rel = (int64_t)cave - (int64_t)(at + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: [GfxServer hook] ★cave 0x%08X 超 rel32 范围，拒绝★",
             (unsigned)(uintptr_t)cave);
        VirtualFree(cave, 0, MEM_RELEASE);
        g_gfxCave = nullptr;
        return false;
    }
    uint8_t patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(int32_t*)(patch + 1) = (int32_t)rel;
    if (!WriteBlockSafe(at, patch, 6, "GfxServer hook")) {
        VirtualFree(cave, 0, MEM_RELEASE);
        g_gfxCave = nullptr;
        return false;
    }
    Logf("pathd: [GfxServer hook] ★已安装★ 0x%08X → cave 0x%08X"
         "（重放 sub esp,8Ch 后抓 ecx=GfxServer）",
         (unsigned)at, (unsigned)(uintptr_t)cave);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ★★★ 方案 F：Application::init 出口冻结门（2026-10-07）★★★
//
// ── 为什么这是根治方案 ──────────────────────────────────────────────────
//   旧方案依赖「引擎什么时候调字体加载器」，这是**不可控的时序竞争**：
//     实测（正常.log vs 没冻上.log）：
//       正常那次    引擎 10:16:47.892 触发 → 门控生效 → 汉化正常
//       没冻上那次  引擎在我们装完补丁后才触发 → 无门控 → 俄文/叠字
//   两份日志的补丁定位耗时差 20 倍（0.42s vs 8.5s），慢的那次直接输了。
//
// ── IDA 实证的调用链（单线程、无分支、每个节点仅 1 个 xref）────────────
//     WinMain @0x414C80
//       └─ sub_5A9040 = Application::init      （字符串 "Application::init"）
//            ├─ sub_59F100 → sub_58DC50 → sub_6843F0 → sub_8BA480 ★字体全部加载完★
//            ├─ … 引擎各子系统 …
//            ├─ 打印 "----------------------- Engine inited in: "
//            └─ 0x5AA388: mov eax,1   ← ★★ 冻结点 ★★
//       └─ sub_5A8310 = 主循环（游戏正式开始）
//
//   5 个 retn 出口全部核实：
//     0x5A921F eax=0 config 打不开   0x5A92A7 eax=0 device 失败
//     0x5AA1F5 eax=0 input 失败      0x5AA411 eax=0 3D 失败
//     0x5AA394 eax=1 ★唯一成功★（紧邻 "Engine inited in: "）
//
// ── 为什么这个点能根治所有症状 ─────────────────────────────────────────
//   冻结时刻引擎满足三个条件，缺一不可：
//     (a) 字体**已全部建完** → 枚举 FontManager 必定成功（原问题：收集不到）
//     (b) **尚未进入主循环** → 不可能有半装状态被引擎执行（原问题：叠字/崩溃）
//     (c) 在**主线程**上，引擎此刻不跑任何绘制 → 收集不依赖渲染线程
//   于是定位/写补丁/装配可以任意慢（单核也无所谓），因为游戏本来就停着。
//
// ── 线程安全性 ────────────────────────────────────────────────────────
//   IDA 实证：整个引擎**只有 1 处 CreateThread**（sub_949FE0，线程池 worker）。
//   所以「挂起全部线程」不存在"挂起时对方正在建字体"的交错风险。

static void* g_gateCave = nullptr;

// cave 布局（5 字节窗口内做完 init 出口的拦截）：
//   60                    pushad
//   B8 <imm32>            mov eax, PathD_InitExitGate
//   FF D0                 call eax
//   61                    popad
//   B8 01 00 00 00        mov eax, 1        ← 恢复被覆盖的 mov eax,1
//   E9 <rel32>            jmp 0x5AA38D      ← pop ebx / add esp,0F0h / retn 14h
static void* BuildGateCave(uintptr_t backTo) {
    void* cave = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!cave) { Logf("pathd: [冻结门] VirtualAlloc 失败"); return nullptr; }

    // 布局（偏移固定，便于回填 rel32）：
    //   +00 60                    pushad
    //   +01 B8 <imm32>            mov eax, PathD_InitExitGate
    //   +06 FF D0                 call eax
    //   +08 61                    popad
    //   +09 B8 01 00 00 00        mov eax, 1
    //   +0E E9 <rel32>            jmp backTo        ← +13 处回填
    //   +13 = 结束
    uint8_t* c = (uint8_t*)cave;
    size_t n = 0;
    c[n++] = 0x60;
    c[n++] = 0xB8;
    *(uint32_t*)(c + n) = (uint32_t)(uintptr_t)&PathD_InitExitGate; n += 4;
    c[n++] = 0xFF; c[n++] = 0xD0;
    c[n++] = 0x61;
    c[n++] = 0xB8; c[n++] = 0x01; c[n++] = 0x00; c[n++] = 0x00; c[n++] = 0x00;
    c[n++] = 0xE9;
    const size_t relOff = n;                 // rel32 所在偏移
    *(uint32_t*)(c + n) = 0; n += 4;
    // ★ cave 基址此刻确定，才回填 jmp 目标 ★
    *(uint32_t*)(c + relOff) =
        (uint32_t)(int32_t)((int64_t)backTo - (int64_t)(c + n));

    FlushInstructionCache(GetCurrentProcess(), cave, n);
    Logf("pathd: [冻结门] cave @0x%08X（%u 字节，回跳 0x%08X）",
         (unsigned)(uintptr_t)cave, (unsigned)n, (unsigned)backTo);
    return cave;
}

// 在 Application::init 的成功出口安装冻结门。
static bool InstallInitExitHook(uintptr_t modBase) {
    const uintptr_t at = modBase + (0x5AA388 - 0x400000);   // mov eax, 1

    // ★ 逐字节校验：`B8 01 00 00 00` ★
    //   只认这一条指令。万一引擎小版本把它挪了或改了，立即拒绝安装，
    //   绝不盲写 —— 写错会把「init 成功」变成崩溃。
    if (rd8(at) != 0xB8 || rd8(at + 1) != 0x01 ||
        rd8(at + 2) != 0x00 || rd8(at + 3) != 0x00 || rd8(at + 4) != 0x00) {
        Logf("pathd: [冻结门] ★0x%08X 字节 %s ≠ B8 01 00 00 00，拒绝安装★",
             (unsigned)at, HexDump(at, 5).c_str());
        return false;
    }
    // 顺带核对紧随其后的两条，确���这就是成功出口而不是别处
    if (rd8(at + 5) != 0x5B) {          // pop ebx
        Logf("pathd: [冻结门] ★后继字节 %s 与预期 pop ebx 不符，拒绝★",
             HexDump(at + 5, 3).c_str());
        return false;
    }

    g_gateCave = BuildGateCave(at + 5);
    if (!g_gateCave) return false;

    int64_t rel = (int64_t)g_gateCave - (int64_t)(at + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: [冻结门] ★cave 0x%08X 距 0x%08X 超 rel32 范围，拒绝★",
             (unsigned)(uintptr_t)g_gateCave, (unsigned)at);
        VirtualFree(g_gateCave, 0, MEM_RELEASE);
        g_gateCave = nullptr;
        return false;
    }
    uint8_t patch[5];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)rel;
    if (!WriteBlockSafe(at, patch, 5, "冻结门 Application::init 出口")) return false;

    Logf("pathd: [冻结门] ★已安装★ 0x%08X: mov eax,1 -> jmp 0x%08X",
         (unsigned)at, (unsigned)(uintptr_t)g_gateCave);
    return true;
}

// 安装：把 sub_8BA480 前 5 字节改成 jmp cave
static bool InstallFontLoaderHook(uintptr_t at) {
    // ★ 逐字节核对原指令：必须是 `E8 xx xx xx xx`（call rel32）★
    if (rd8(at) != 0xE8) {
        Logf("pathd: [字体加载器hook] ★0x%08X 不是 E8（实为 %02X），拒绝安装★",
             (unsigned)at, rd8(at));
        return false;
    }
    int32_t oldRel = (int32_t)rd32s(at + 1);
    uintptr_t oldTgt = (uintptr_t)((int64_t)(at + 5) + oldRel);
    // 预期目标是字体加载器 sub_8B9B60（相对模块基址）
    if (oldTgt != g_modBase + (0x8B9B60 - 0x400000)) {
        Logf("pathd: [字体加载器hook] ★call 目标 0x%08X 不是 0x%08X（sub_8B9B60），拒绝安装★",
             (unsigned)oldTgt, (unsigned)(g_modBase + (0x8B9B60 - 0x400000)));
        return false;
    }
    // 后两条指令也核对（and eax,1 / ret）
    if (rd8(at + 5) != 0x83 || rd8(at + 6) != 0xE0 || rd8(at + 7) != 0x01 || rd8(at + 8) != 0xC3) {
        Logf("pathd: [字体加载器hook] ★后续字节 %s 与预期 (83 E0 01 C3) 不符，拒绝安装★",
             HexDump(at + 5, 4).c_str());
        return false;
    }

    g_fontHookCave = BuildFontLoaderCave(oldTgt);
    if (!g_fontHookCave) return false;

    // ★ E9 jmp rel32 到 cave，后 1 字节 NOP ★
    //   注意：cave 是 VirtualAlloc 来的，可能远离 hta.exe（>2GB）——
    //   E9 是 rel32，够不到时改用 `FF 25`（jmp [imm32] 绝对间接跳转，6 字节）。
    //   5 字节窗口只能放 E9，所以先算距离。
    int64_t rel = (int64_t)g_fontHookCave - (int64_t)(at + 5);
    uint8_t patch[5];
    if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) {
        Logf("pathd: [字体加载器hook] ★cave 0x%08X 距目标 0x%08X 超过 rel32 范围，拒绝★",
             (unsigned)(uintptr_t)g_fontHookCave, (unsigned)at);
        VirtualFree(g_fontHookCave, 0, MEM_RELEASE);
        g_fontHookCave = nullptr;
        return false;
    }
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)rel;

    if (!WriteBlockSafe(at, patch, 5, "字体加载器hook")) {
        Logf("pathd: [字体加载器hook] 写入失败");
        return false;
    }
    Logf("pathd: [字体加载器hook] ★已安装★ 0x%08X: call rel32 -> E9 jmp 0x%08X",
         (unsigned)at, (unsigned)(uintptr_t)g_fontHookCave);
    Logf("pathd:   cave: call 0x%08X (原 sub_8B9B60) → pushad → 装配 → popad → and eax,1 → ret",
         (unsigned)oldTgt);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.5) 启动冻结：DLL 安装期间把游戏线程全部挂起
// ═══════════════════════════════════════════════════════════════════════════
//
//   动机（用户实测反馈）：「游戏有时候会装不上」。
//   根因是**竞争**：我们在装补丁的同时，引擎的加载线程也在跑，
//   它会去调正在被我们改写的那些函数（度量/绘制/烘图）。
//   表现就是偶发装不上、或装到一半被游戏逻辑打断。
//
//   做法：初始化一开始就挂起除自己以外的全部线程，装配完成再恢复。
//   这与 WriteBlockSafe 的「短暂挂起→写→恢复」是同一套思路，
//   只是时间窗口从"微秒级"扩大到"整个初始化"。
//
//   ★ 死锁风险与规避 ★
//   被挂起的线程若正持有某个锁（比如引擎的加载锁、CRT 锁），
//   而**我们又去要那个锁**，就会死锁 —— 日志就是最典型的例子
//   （Logf 走 CRT/文件锁）。所以：
//     · 冻结期间**绝不调用 Logf**
//     · 冻结前把所有要打印的日志先打完
//     · 装配线程所需的锁（我们自己那把）在冻结前就已释放
//   我们冻结期间不调用任何引擎函数，只用 Win32 API 和自己的内存。
//
//   ★ 兜底 ★
//   万一装配卡住，不能把游戏永久冻死。用 HTA_CHS_NO_FREEZE=1 可整体关闭；
//   兜底超时 kFreezeBudgetMs 到点无条件恢复。
static HANDLE g_frozenThreads[512];
static int    g_frozenCount = 0;
bool         g_frozen = false;

static DWORD WINAPI FreezeWatchdog(LPVOID param);   // 前置声明（定义在下方）

static void FreezeGameThreads() {
    if (g_frozen) return;
    char v[8] = {0};
    if (GetEnvironmentVariableA("HTA_CHS_NO_FREEZE", v, sizeof(v)) > 0) return;

    const DWORD self = GetCurrentThreadId();
    g_frozenCount = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            if (te.th32ThreadID == self) continue;
            if (g_frozenCount >= 512) break;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            g_frozenThreads[g_frozenCount++] = th;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_frozen = true;

    // ★ 兜底：最长冻结 kFreezeBudgetMs，到点无条件解冻 ★
    //   装配实测约 10 秒（含字形填充）。给到 120 秒足够宽裕，
    //   同时避免"卡住就永久冻死"这种最坏情况。
    const DWORD kFreezeBudgetMs = 120000;
    HANDLE wd = CreateThread(nullptr, 64 * 1024, FreezeWatchdog,
                             (LPVOID)(uintptr_t)kFreezeBudgetMs, 0, nullptr);
    if (wd) CloseHandle(wd);
}

static void UnfreezeGameThreads() {
    if (!g_frozen) return;
    for (int i = 0; i < g_frozenCount; ++i) {
        ResumeThread(g_frozenThreads[i]);
        CloseHandle(g_frozenThreads[i]);
    }
    g_frozenCount = 0;
    g_frozen = false;
}

// ── 兜底看门狗 ──────────────────────────────────────────────────────────
//   装配线程若因为任何意外卡住（比如某个字体结构损坏导致死循环），
//   冻结就没人解除 —— 用户看到的是永久黑屏无响应。
//   所以起一个**独立**的看门狗线程：到点无条件解冻。
//   它只做一件事，不碰任何引擎数据，最坏情况也只是"提前解冻"。
static DWORD WINAPI FreezeWatchdog(LPVOID param) {
    DWORD ms = (DWORD)(uintptr_t)param;
    Sleep(ms);
    if (g_frozen) {
        // 这里可以安全 Logf：看门狗不持有任何锁，且此时解冻是正确行为
        Logf("pathd: [看门狗] 超过 %lu ms 仍未装配完成，强制解冻游戏线程", ms);
        UnfreezeGameThreads();
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 6) 补丁安装（仍在 pathd 内）
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

// ═══════════════════════════════════════════════════════════════════════════
// 汉化安装失败 → 弹框告知玩家 → 退出游戏
// ═══════════════════════════════════════════════════════════════════════════
//
//  ★ 为什么必须退出，而不是"回滚后继续玩原版" ★
//     以前失败时的做法是整体回滚、游戏按原版跑（稳定但没中文）。实测下来
//     这个"安静降级"反而更糟：
//       · 玩家看到的是**乱码 + 字叠在一起**，根本看不出"汉化没装上"，
//         只会以为游戏坏了；
//       · 更坑的是**半装状态**——引擎可能已经按 16 位索引取到了我们
//         填好的表，却仍用 8 位的前进/度量逻辑渲染，于是满屏叠字。
//     既然装了补丁却不能正确工作，就明确告诉玩家"没装上"并退出，
//     比让玩家对着一个坏画面反复重启、并误以为显卡/游戏有问题要好。
//
//  ★ 为什么归因写"游戏太老 / 电脑太新" ★
//     真实原因是这个补丁方案依赖**写引擎 .text 段**并在特定线程状态下抢写，
//     而现代 Windows 的内存保护（DEP/WER/线程调度）让这种"热补丁"越来越
//     难稳定成功。这话对玩家是准确的，也是可行动的（重试可能成功）。
static void FatalInstallFailure(const char* reason) {
    // ★ 不用 _snprintf_s 拼格式 ★
    //   实测编译警告 C4474：把 reason 作为可变参数传给 _snprintf_s 时，
    //   编译器判定该重载不接受可变参数，"原因"会变成垃圾。
    //   纯拼接没有这个歧义，也不会碰 % 之类的格式符转义问题。
    char        msg[1024];
    const char* head =
        "汉化安装失败。\n\n"
        "原因：";
    const char* tail =
        "\n\n"
        "《Hard Truck Apocalypse》(2006) 的引擎代码年代久远，"
        "而现代 Windows 的内存保护机制让“运行时热补丁”很难稳定写入。\n\n"
        "请尝试重新启动游戏 —— 多数情况下重试即可成功。\n"
        "若多次重试仍失败，请把 update 目录下的 hta_chs*.log 一并反馈。";

    size_t need = strlen(head) + strlen(reason ? reason : "(未知)") + strlen(tail) + 1;
    if (need > sizeof(msg)) need = sizeof(msg);
    msg[0] = '\0';
    strncat_s(msg, sizeof(msg), head, _TRUNCATE);
    strncat_s(msg, sizeof(msg), reason ? reason : "(未知)", _TRUNCATE);
    strncat_s(msg, sizeof(msg), tail, _TRUNCATE);

    Logf("pathd: ═════ 汉化安装失败，提示玩家并退出 ═════");
    Logf("pathd:   原因: %s", reason ? reason : "(未知)");
    Logf("pathd:   提示用户重新启动游戏；日志见 update\\hta_chs*.log");

    // ★ 顺序很关键：先弹框，等玩家点掉，再退出 ★
    //   （不能在弹框前 ExitProcess，那样玩家什么都看不到。）
    //
    // ★★ 为什么必须用 MessageBoxW（2026-10-07 修正）★★
    //   本 DLL 编译带 /utf-8 ⇒ 源码里的中文字面量是 **UTF-8 字节**。
    //   而 MessageBoxA 会把这些字节按**系统 ANSI 代码页**（中文系统 = 936/GBK）
    //   解释 ⇒ 标题和正文全成乱码。实测玩家看到的是：
    //       "Hard Truck Apocalypse-姹久穿瀹菱口澶辫触"
    //   正确做法：显式把 UTF-8 转成 UTF-16 再交给 MessageBoxW —— 与代码页无关。
    HMODULE u32 = ::GetModuleHandleA("user32.dll");
    if (u32) {
        typedef int (__stdcall *MsgBoxW_t)(void*, const wchar_t*, const wchar_t*, unsigned);
        MsgBoxW_t fnW = (MsgBoxW_t)::GetProcAddress(u32, "MessageBoxW");
        if (fnW) {
            wchar_t   wmsg[1024] = {0};
            wchar_t   wtitle[128] = {0};
            const int nMsg = ::MultiByteToWideChar(CP_UTF8, 0, msg, -1, wmsg,
                                                   (int)(sizeof(wmsg) / sizeof(wmsg[0])));
            const int nTitle = ::MultiByteToWideChar(CP_UTF8, 0, "Hard Truck Apocalypse - 汉化安装失败", -1,
                                                     wtitle, (int)(sizeof(wtitle) / sizeof(wtitle[0])));
            if (nMsg > 0 && nTitle > 0) {
                fnW(nullptr, wmsg, wtitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            } else {
                Logf("pathd:   [警告] UTF-8→UTF-16 转换失败(%d/%d)，无法弹框", nMsg, nTitle);
            }
        } else {
            Logf("pathd:   [警告] 找不到 MessageBoxW，无法弹框");
        }
    } else {
        // 连 user32 都没有（极端情况）：至少写日志，然后退出。
        Logf("pathd:   [警告] user32.dll 不可用，无法弹框");
    }
    Logf("pathd:   玩家已确认，正在退出进程");
    ::ExitProcess(1);
}

bool InstallPatches() {
    if (g_skipPatch) {
        Logf("pathd: [调试] 按开关跳过全部补丁");
        InterlockedExchange(&g_patchState, PATCH_STATE_OK);
        return true;
    }
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
    // ★ P7/P8 度量补丁定位 —— ★特征码必须**唯一**，多命中即拒绝 ★
    //
    //   实测教训（2026-10-07 首次上机）：锚点 "83 C7 01 3B 7C 24 ?? 0F 8C"
    //   在 hta.exe 里有 **8 处**命中（IDA find_bytes 实证）：
    //       0x44ED34 / 0x5C5B2C / 0x60B24D / 0x635F36 /
    //       0x685BD1（★ 真正的度量循环）/ 0x8AD87E / 0x8ADB83 / 0x8ADECE
    //   ScanUnique 只取第一处 -> 装到了 0x44ED34 这个**完全无关的函数**上，
    //   直接把正常代码覆盖成 jmp，症状就是"乱码 + 字全叠一起"。
    //
    //   ⇒ 修法：锚点**必须带上度量循环独有的上文**，让它全局唯一。
    //   上文（0x685BC3 起，逐字节实测）：
    //       movss xmm0,[esp+0x14]     F3 0F 10 44 24 14
    //       mov   ecx,[esp+0x10]      8B 4C 24 10
    //       mov   esi,[esp+0x20]      8B 74 24 20
    //       add   edi,1              83 C7 01
    //   最后那个 [esp+0x20]（文本基址）+ 紧跟 add edi,1 的组合是独有的。
    //   ★ 这里刻意**不用 ??** 通配那三个 disp ★
    //     disp 已用字节逐条核实过，写死反而能让签名唯一。
    //     若哪天引擎小版本平移了栈帧，应该是「签名失配 -> 拒绝安装」，
    //     而不是「误装到别的函数上」—— 后者会静默破坏正常代码。
    uintptr_t pMsrWalk = ScanUnique("F3 0F 10 44 24 14 8B 4C 24 10 8B 74 24 20 83 C7 01",
                                     "P8 度量推进");
    // 锚点起点是 0x685BC3（movss xmm0,[esp+0x14]），三条上文指令共
    //   6 + 4 + 4 = 14 字节，其后才是 `add edi,1`（0x685BD1）。
    //   ★ 这个 14 是算出来的，不是估的 ★
    if (pMsrWalk) pMsrWalk += 14;
    // P7 锚点：mov esi,[esp+0x4C] ; push esi ; call rel32 ; fadd [esp+0x14]
    //   末尾的 fadd [esp+0x14] 是 P7 的**回跳落点**，必须一并锁定，
    //   否则 10 字节覆盖范围会错位、把下一条指令劈开。
    uintptr_t pMsrW2   = ScanUnique("8B 74 24 4C 56 E8 ?? ?? ?? ?? D8 44 24 14",
                                     "P7 度量宽度", false);

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
    // ── P7/P8 度量路径（修「过场剧情不换行」）─────────────────────────
    //
    //   根因（字节级核实）：引擎度量循环只按**单字节**推进，且字形查询只喂
    //   单字节（sub_66FEA0 / sub_66FE10 都只收 uint8）。GBK 前导字节 >= 0x81
    //   在引擎自带 256 项表里查不到，每个汉字的两个字节各贡献一次
    //   **0.125f**（xmmword_9E6A74+8，实测 00 00 00 3E）
    //   ⇒ 一个汉字被记成 0.25 em（真实约 1.0），宽度低估到 1/4
    //   ⇒ 整行累加到头也够不着行宽阈值 ⇒ 不折行。
    //
    //   ★ 为什么不能改 sub_66FEA0/sub_66FE10（它们只有 0x22/0x3B 字节，
    //     小到看着能整体重写）★
    //     它们**只收一个字节**，拿不到 GBK 后继字节 b2，函数内部无法判断
    //     "这个 0x81 后面还有没有第二个字节"。⇒ hook 必须在循环内。
    //
    //   P7 = 宽度：jmp 挂在 0x685B21，覆盖 mov esi,[esp+4C] / push esi /
    //        call sub_66FEA0 共 10 字节，助手返回 st(0)=advance 后 jmp 回
    //        0x685B2B（引擎的 fadd 继续执行）。
    //   P8 = 推进：jmp 挂在 0x685BD1，覆盖 add edi,1 / cmp / jl 共 13 字节，
    //        助手自己推进并**自己完成 cmp + 回跳循环头**（0x685A80）。
    //
    //   ★★ 为什么用 jmp 而不是 call ★★
    //     jmp 进来的助手栈上**没有返回地址**，绝不能 ret；出口也必须是 jmp。
    //     call 会压 4 字节返回地址，P8 就必须 `add esp,4` 丢它 ——
    //     那正是 P2/P3 崩在助手内部（hta.exe0057）的手法。jmp 栈零风险。
    // P7：度量宽度点。**独立判定** —— P7/P8 是两处独立补丁，
    //   任一失配不应牵连另一处（旧写法用 if(pMsrWalk) 把两者圈在一起，
    //   P7 定位失败时会在 HexDump(0,...) 上读野地址）。
    if (pMsrW2) {
        uintptr_t pMsrW = pMsrW2;
        uintptr_t afterW = pMsrW + 10;
        Logf("pathd: [度量] P7 宽度点 @0x%08X  原字节 %s", (unsigned)pMsrW, HexDump(pMsrW, 10).c_str());
        if (g_skipP7) {
            Logf("pathd: [度量] P7 被 HTA_CHS_NO_P7 关闭，保持原样");
        } else if (rd8(pMsrW) != 0x8B || rd8(pMsrW+1) != 0x74 || rd8(pMsrW+2) != 0x24 || rd8(pMsrW+3) != 0x4C
            || rd8(pMsrW+4) != 0x56 || rd8(pMsrW+5) != 0xE8) {
            Logf("pathd:   ★原字节与预期不符（%s），拒绝安装 P7★", HexDump(pMsrW, 6).c_str());
        } else if (rd8(afterW) != 0xD8 || rd8(afterW+1) != 0x44 || rd8(afterW+2) != 0x24 || rd8(afterW+3) != 0x14) {
            Logf("pathd:   ★落点 0x%08X 应为 fadd [esp+14]（D8 44 24 14）却是 %s，拒绝安装 P7★",
                 (unsigned)afterW, HexDump(afterW, 4).c_str());
        } else {
            g_msrAfterW = (uint32_t)afterW;
            if (WriteJmpBlock(pMsrW, 10, (void*)&PathD_MsrAdvanceW, "P7 度量宽度")) ++done; else ++fail;
            Logf("pathd:   落点 g_msrAfterW=0x%08X", g_msrAfterW);
        }
    } else {
        Logf("pathd: [MISS] P7 度量宽度（中文宽度仍按 0.125em 计 → 不换行，但能显示）");
    }

    // P8：双字节推进点
    if (pMsrWalk) {
        uintptr_t pMsrB = pMsrWalk;
        // ★★ rel32 在 +9，不是 +7 ★★（2026-10-07 实测闭环）
        //   布局： +0 83 C7 01          add edi,1            (3)
        //          +3 3B 7C 24 1C       cmp edi,[esp+1C]     (4)
        //          +7 0F 8C             jl 近跳转操作码      (2)  ← 不是位移！
        //          +9 A2 FE FF FF       rel32 = -0x15E       (4)
        //   实测证据：写 +7 时读到 `0F 8C A2 FE` = -22901745，
        //     0x685BDE - 0x15D67C1 = 0xFF0AE7ED —— 与日志打印的坏值
        //     「★引擎 jl 目标 0xFF0AE7ED★」分毫不差，诊断闭环。
        //   验证：0x685BDE + (-0x15E) = 0x685A80 正是循环头。
        //
        //   顺带记一笔我自己的纠错：退出地址那边我一度以为 `EB 32` 跳去
        //   0x685BCB，那是错的 —— 我在 Python 里把 `unpack_from("<b",b,13)`
        //   读成了 0xEB **操作码**（rel8 在 +14）。0x32 = +50，
        //   0x685BE0 + 50 = 0x685C12，IDA 的解析是对的。
        uintptr_t loopHead = (uintptr_t)((int64_t)(pMsrB + 13) + (int32_t)rd32s(pMsrB + 9));
        // 引擎原本的 jl 目标就是循环头，直接沿用，不重新计算
        g_msrLoopHead = (uint32_t)loopHead;
        // 0x685BDE 是 `jmp short 0x685C12`（EB xx），落点 = pMsrB+13+2+rel8
        int8_t  exitRel  = (int8_t)rd8(pMsrB + 14);
        g_msrLoopExit   = (uint32_t)((int64_t)(pMsrB + 13 + 2) + exitRel);
        Logf("pathd: [度量] P8 推进点 @0x%08X  原字节 %s", (unsigned)pMsrB, HexDump(pMsrB, 15).c_str());
        if (g_skipP7) {
            Logf("pathd: [度量] P8 被 HTA_CHS_NO_P7 关闭，保持原样");
        } else if (rd8(pMsrB) != 0x83 || rd8(pMsrB+1) != 0xC7 || rd8(pMsrB+2) != 0x01
            || rd8(pMsrB+3) != 0x3B || rd8(pMsrB+4) != 0x7C || rd8(pMsrB+5) != 0x24 || rd8(pMsrB+6) != 0x1C
            || rd8(pMsrB+7) != 0x0F || rd8(pMsrB+8) != 0x8C) {
            Logf("pathd:   ★原字节与预期不符（%s），拒绝安装 P8★", HexDump(pMsrB, 9).c_str());
        } else if (loopHead != g_modBase + (0x685A80 - 0x400000)) {
            Logf("pathd:   ★引擎 jl 目标 0x%08X 与预期循环头 0x%08X 不符，拒绝安装 P8★",
                 (unsigned)loopHead, (unsigned)(g_modBase + (0x685A80 - 0x400000)));
        } else if (g_msrLoopExit != g_modBase + (0x685C12 - 0x400000)) {
            Logf("pathd:   ★退出地址 0x%08X 与预期 0x%08X 不符，拒绝安装 P8★",
                 g_msrLoopExit, (unsigned)(g_modBase + (0x685C12 - 0x400000)));
        } else {
            if (WriteJmpBlock(pMsrB, 13, (void*)&PathD_MsrAdvanceB, "P8 度量推进")) ++done; else ++fail;
            Logf("pathd:   循环头 g_msrLoopHead=0x%08X  退出 g_msrLoopExit=0x%08X",
                 g_msrLoopHead, g_msrLoopExit);
        }
    } else {
        Logf("pathd: [MISS] P8 度量推进（中文不换行，但能显示）");
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
            { "P7  度量宽度", pMsrW2,       10 },
            { "P8  度量推进", pMsrWalk,     13 },
        };
        for (int i = 0; i < 5; ++i) {
            if (!chk[i].at) { Logf("pathd: [自检] %-16s 未定位", chk[i].tag); continue; }
            Logf("pathd: [自检] %-16s @0x%08X  覆盖 %d 字节: %s  ‖ 边界外: %s",
                 chk[i].tag, (unsigned)chk[i].at, chk[i].n,
                 HexDump(chk[i].at, chk[i].n).c_str(),
                 HexDump(chk[i].at + chk[i].n, 4).c_str());
        }
    }

    // ── 字体加载器 hook 已在 Init 最开头装好（早期注入）──────────────
    //
    //   ★ 这里不再安装 ★ 原因见 InstallFontLoaderHookEarly 的说明：
    //     装在这里就晚了 —— 引擎字体加载发生在进程启动后约 1 秒
    //     （游戏日志实证：05:55:28 Starting up → 05:55:29 fonts loaded），
    //     而本函数里的 7 个补丁每个要挂起 50 个线程，累计十几秒。
    //     早期注入只做"算地址 + 校验 + 写 5 字节"，几微秒完成。
    //   ★ 方案 B：字体加载器 hook **已完全停用** ★
    //     它是旧方案的核心机制，靠"引擎何时调加载器"触发装配 ——
    //     那个时机不可控，实测只有约 60% 命中率。
    //     现在改由 Application::init 出口的冻结门接管（时机 100% 确定），
    //     所以这里不再安装字体加载器 hook。
    //     保留 InstallFontLoaderHook / BuildFontLoaderCave 代码以便回退。
    Logf("pathd: [路线2] GfxServer hook 抓 GfxServer（FontManager = GfxServer+0x4A4），"
         "装配时机交给冻结门");

    // ★ "补丁安装完成"这条日志已移到函数末尾（与 g_patchState 置位放在一起）★

    // ★★★ 全或无：失败必须整体回滚 ★★★
    //
    //   实测 hta.exe0004（巨卡/没字/崩溃）的根因就是这个：
    //       补丁安装完成：成功 3，失败 4
    //   P4/P4b（查表）装上了，P5/P7/P8（推进与度量）没装上。
    //   引擎于是按新逻辑查我们**没准备好**的表 —— 半成品状态
    //   比"完全不装"危险得多：完全不装只是没有汉化，半成品会崩。
    //
    //   ⇒ 任何一个补丁写入失败，就把**已写成功的全部还原成原字节**。
    //     退出"汉化模式"，游戏回到原版行为（能正常玩，只是没中文）。
    //
    //   还原用同一套 WriteBlockSafe（此时线程状态与装补丁时相同），
    //   失败概率极低；即使还原失败也有日志可查。
    if (fail > 0) {
        Logf("pathd: ★有 %d 个补丁未能安装 —— 执行**整体回滚**，避免半成品状态★", fail);
        Logf("pathd:   半成品（部分补丁生效）会让引擎查未准备好的表 → 巨卡/没字/崩溃；");
        Logf("pathd:   整体回滚后游戏按原版逻辑运行：没有汉化，但稳定可玩。");
        int rolled = 0;
        g_rollingBack = true;
        for (int i = (int)g_installedPatches.size() - 1; i >= 0; --i) {
            const InstalledPatch& ip = g_installedPatches[i];
            if (WriteBlockSafe(ip.at, ip.orig, ip.len, "回滚")) {
                ++rolled;
            } else {
                Logf("pathd:   [回滚失败] @0x%08X（%u 字节）—— 请把日志发给开发者",
                     (unsigned)ip.at, (unsigned)ip.len);
            }
        }
        g_installedPatches.clear();
        g_rollingBack = false;
        Logf("pathd: 回滚完成：还原 %d 个补丁", rolled);
        g_enabled = false;      // ← 关键：告诉上层"汉化未启用"
        // ★ 放行 cave：置 FAILED 而不是让它继续等 30 秒 ★
        //   补丁没了，cave 里的装配本来就没有意义，
        //   让它立刻返回比干等超时好得多（引擎主线程不被白扣住）。
        InterlockedExchange(&g_patchState, PATCH_STATE_FAILED);
        return false;           // ← 让 Init 知道补丁没装上
    }

    // ★ 全部补丁装好：放行 cave 去装配 ★
    //   这一行是"补丁 → 装配"这个顺序的关键：cave 里会等这个标志，
    //   所以不可能再出现"字形装好了但补丁还没写进去"的半成品状态。
    Logf("pathd: ───── 补丁安装完成：成功 %d，失败 %d ─────", done, fail);
    InterlockedExchange(&g_patchState, PATCH_STATE_OK);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 7) 入口
// ═══════════════════════════════════════════════════════════════════════════

// ★★★ 最早期安装：字体加载器 hook ★★★
//
//   动机（用户提出的架构问题）："引擎在运行，DLL 也在运行，二者时态不一致"。
//   根因不是"加载器太早"，而是**我们的 Init 太慢**：
//       历史实测：引擎字体加载于 08:06:26
//                 我们装钩子于 08:06:27.218   ← 晚了 1.2 秒
//   而慢的原因是 Init 里有大量工作：特征码扫描（要扫 6.6MB × 多次）
//   + 7 个补丁逐个安装（每个要挂起 50 个线程，累计 11 秒）。
//
//   ★ 所以把「装 hook」与「装补丁」分开 ★
//     装 hook 只需要：
//       1. 算出目标地址（**直接用 RVA**，本 exe ImageBase 固定 0x400000 且无 ASLR）
//       2. 校验字节
//       3. 写 5 字节 jmp
//     全部是几微秒级操作，不做任何扫描。
//     于是 hook 在所有慢活之前就装好了 —— 引擎调加载器时必然命中。
//
//   补丁（P4/P5/P7/P8…）仍然在 hook 之后慢慢装——顺序仍然对，
//   但**理由和当初写的相反**，务必注意：
//     ★ 当初写的是"补丁不参与字体加载，装配只写数据结构不依赖补丁，
//       所以顺序正确"。实测证明那是错的（08:35 两次对照）：
//         失败那次  装配 08:35:09~12（10 字号 2079 齐备）
//                    补丁 08:35:13~30 写了 17 秒仍未完→ 无"补丁安装完成"
//         成功那次  装配 08:35:50~53（同上，也是齐备的）
//                    补丁 08:35:54~55（1.98 秒）全部写完
//       两次**装配结果完全一致**，差别只在补丁。装配填的 64K 码表只有
//       引擎用 16 位索引去查才有意义；补丁没装 = 8 位查表 + 8 位前进量
//       → 双字节汉字被拆成两个字节 → 字叠在一起 + 乱码。
//     ⇒ 现在 PathD_HookAssemble 里加了**补丁门**（等 g_patchState），
//       把"装配"强制排在"补丁就绪"之后，这才是顺序正确的真正原因。
//     ⇒ 另：不要试图把 LoadPackage 提到 hook 之前（v23 实测是回归）：
//       Init 会因此多花约 600ms，引擎在 hook 装好前就把字体加载完，
//       cave 永不触发 → g_hookFontMgr=0 → 汉字几乎装不上。
static bool InstallFontLoaderHookEarly(uintptr_t modBase) {
    // 直接算地址：sub_8BA480 的 RVA = 0x8BA480 - 0x400000 = 0x4BA480
    const uintptr_t kRva = 0x8BA480 - 0x400000;
    uintptr_t at = modBase + kRva;

    // ★ 字节校验：不能盲信 RVA（万一别的版本不同）★
    //   E8 ?? ?? ?? ??  83 E0 01 C3
    if (rd8(at) != 0xE8 || rd8(at + 5) != 0x83 || rd8(at + 6) != 0xE0
        || rd8(at + 7) != 0x01 || rd8(at + 8) != 0xC3) {
        Logf("pathd: [早期hook] ★0x%08X 字节不符（%s），拒绝安装★",
             (unsigned)at, HexDump(at, 9).c_str());
        return false;
    }
    // call 目标必须是字体加载器 sub_8B9B60
    int32_t rel = (int32_t)rd32s(at + 1);
    uintptr_t tgt = (uintptr_t)((int64_t)(at + 5) + rel);
    uintptr_t want = modBase + (0x8B9B60 - 0x400000);
    if (tgt != want) {
        Logf("pathd: [早期hook] ★call 目标 0x%08X ≠ sub_8B9B60(0x%08X)，拒绝★",
             (unsigned)tgt, (unsigned)want);
        return false;
    }
    Logf("pathd: [早期hook] 字节与 call 目标均校验通过（call 0x%08X = sub_8B9B60）",
         (unsigned)tgt);
    return InstallFontLoaderHook(at);
}

bool Init(HMODULE game, const char* pkgPath) {
    g_modBase = (uintptr_t)game;
    InitializeCriticalSection(&g_cs);

    // ── 最早期基线快照（2026-10-07）───────────────────────────────
    //   在做任何事之前先记一次内存画像，之后每个关键节点再记一次。
    //   有了"我们还没占任何东西"这条基线，才能算出**我们到底占了多少**：
    //     我们占的 ≈ 装配完成时的提交量 − 这一刻的提交量
    //   转储里 MemoryManager 自报 mem used ≈ 108MB，但用户区只剩 19MB，
    //   差额不明——这组快照就是为了把差额拆出来。
    LogMemorySnapshot("插件启动（尚未占用）");

    // ★★★ 第一件事：装两个 hook（都只要几微秒，不做任何扫描）★★★���
    //
    //   (1) GfxServer hook @0x6843F0 —— **只为抓 GfxServer（⇒ FontManager）**
    //       路线 2（2026-10-07）。sub_6843F0 是 UI 构建总入口，
    //       由 sub_58DC50 无条件调用，入口 ecx 就是 GfxServer，
    //       而 FontManager = GfxServer + 0x4A4（IDA 0x684E48 实证）。
    //       为什么不抓 sub_8BA480（FontManager 本尊）：
    //         它只在 schema 解析成功时才被调用（0x68444E `jz` 提前跳走），
    //         实测偶发不执行 ⇒ 抓到的 FontManager 偶发为 0 ⇒ 空白字。
    //
    //   (2) Application::init 出口冻结门 @0x5AA388 —— 决定装配时机
    //       方案 F：把装配从「引擎何时调加载器」这个不可控竞争里拿出来。
    //
    //   两者都必须在所有慢活之前装好：引擎随时可能触发其中任何一个。
    DWORD tEarly0 = GetTickCount();
    bool gfxHooked = false, gateHooked = false;
    if (!g_skipPatch) {
        // ★ 顺序（2026-10-07 调整）：冻结门**先装** ★
        //   门决定装配时机，装晚了引擎就可能已经越过 0x5AA388 ⇒ 门永不触发
        //   ⇒ 没有"接管"日志、也没有弹框，静默无汉字（实测 210633 那次）。
        //   而 WriteBlockSafe 要挂起全部线程（实测 ~1.4~3.2 秒），谁先装谁就先落地，
        //   所以把最关键的门放到最前面。
        __try { gateHooked = InstallInitExitHook((uintptr_t)game); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("pathd: [冻结门] 安装异常 0x%08X", (unsigned)GetExceptionCode());
        }
        __try { gfxHooked = InstallGfxServerHook((uintptr_t)game); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("pathd: [GfxServer hook] 安装异常 0x%08X", (unsigned)GetExceptionCode());
        }
    }
    g_loaderHooked = gfxHooked;
    Logf("pathd: === 早期注入结束：GfxServer hook=%s 冻结门=%s，耗时 %u ms ===",
         gfxHooked ? "已装" : "未装", gateHooked ? "已装" : "未装",
         (unsigned)(GetTickCount() - tEarly0));

    // ★ 冻结门装不上 = 完全无法掌控装配时机 ⇒ 弹框退出（不静默降级）
    if (!gateHooked) {
        FatalInstallFailure("无法在引擎初始化出口安装同步挂钩"
                            "（汉化必须在此刻完成，否则会出现乱码或叠字）");
    }
    // ★ 路线 3：GfxServer 已改为直读引擎全局 dword_A13CC0。
    //   门触发 ⟹ 全局必然非 0（CFG 静态证明，见 CollectFontsFromManager 上方注释），
    //   所以入口 hook 装不上**不再致命**，仅降级为交叉校验。
    //   （旧行为：hook 装不上就弹框退出——那是把「唯一的抓取手段」当成了必需。）
    if (!gfxHooked) {
        Logf("pathd: [路线3] ★GfxServer hook 未装上——不影响★ "
             "已改为直读全局 dword_A13CC0（门触发时必然有效）");
    }

    // 二分定位开关（临时调试用）
    {
        char v[8];
        g_skipPatch  = GetEnvironmentVariableA("HTA_CHS_NO_PATCH",  v, sizeof(v)) > 0;
        g_skipExpand = GetEnvironmentVariableA("HTA_CHS_NO_EXPAND", v, sizeof(v)) > 0;
        g_skipP5     = GetEnvironmentVariableA("HTA_CHS_NO_P5",     v, sizeof(v)) > 0;
        g_skipP4b    = GetEnvironmentVariableA("HTA_CHS_NO_P4B",    v, sizeof(v)) > 0;
        g_skipP7     = GetEnvironmentVariableA("HTA_CHS_NO_P7",     v, sizeof(v)) > 0;
        g_skipScan   = GetEnvironmentVariableA("HTA_CHS_NO_SCAN",   v, sizeof(v)) > 0;
        Logf("pathd: 调试开关 跳过补丁=%d 跳过扩表=%d 跳过P5=%d 跳过P4b=%d 跳过P7=%d 跳过扫描=%d",
             (int)g_skipPatch, (int)g_skipExpand, (int)g_skipP5, (int)g_skipP4b,
             (int)g_skipP7, (int)g_skipScan);
    }

    Logf("pathd: ══════════ 路径 D 初始化 ══════════");

    // ★★★ 引擎分配器必须最先定位（在 LoadPackage 之前）★★★
    //
    //   原因：字体加载器 hook 一装好，引擎随时可能立刻触发装配，
    //   而装配要靠 g_engineAlloc 给 CJK 图集页表分配内存
    //   （必须用引擎堆，引擎 ~Font 的 free 才能正确配对）。
    //
    //   ★ 实测教训（07:16:26 那次"俄语乱码"）★
    //       07:16:26.207  包文件已就绪（等待 312 ms）
    //       07:16:26.253  [装配] 开始
    //       07:16:29.653  [ 12.000] 无引擎分配器，跳过挂页
    //       ...           10 个字号全部"无引擎分配器"
    //       07:16:29.986  === 准备完成：0 个字号已装入汉字 ===
    //       07:16:30.641  引擎分配器 = 0x00589410（自检通过）  ← ★ 晚了 4.4 秒
    //   ⇒ 根因：分配器定位排在 ScanUnique(vector::resize) 之后，
    //     被那次**无用的**扫描又拖慢，最后整个落在装配之后。
    //
    //   这也解释了"单核 bat 有时能治好"：单核让引擎变慢，
    //   Init 线程（分配器）可能抢在加载器之前跑完 —— 但并**不保证**，
    //   所以用户实测"单核也不一定解决"。真正的修法就是这个顺序调整。
    {
        //   sub_589410 : __fastcall(ecx=size, edx=0, stack=0)，内部 mov ecx,dword_A0A880
        //               后调 sub_748DC0；返回 retn 4。
        // ★ 这个锚点天然 2 处命中（alloc 的两个包装），设计就是取首个 +
        //   下面的运行时 probe 自检兜底，所以必须 allowMulti。
        //   我曾用 ScanUnique 统一收口，结果它变成 0 → 汉字挂不上页（回归）。
        g_engineAlloc = (EngineAllocFn)ScanFirst(SIG_ENGINE_ALLOC, "引擎分配器(sub_589410)");
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
    }

    // ★ 包文件已恢复到此处的原始调用位置（曾被临时提前到装 hook 之前，
    //   2026-10-07 v23 实测为回归：提前后 Init 多花 ~600ms，引擎在
    //   hook 装好之前就把字体加载完，cave 永不触发 → g_hookFontMgr=0 →
    //   走 SetupThread 后备路径且 FontManager 取不到 → 汉字几乎装不上。
    //   教训：hook 必须**尽��装**，任何排在它前面的耗时活都会喂饱引擎。）
    //   详细的失败日志见 commit 说明 / 空白字.log 同批日志 083117。
    if (!LoadPackage(pkgPath)) {
        // ★ 2026-10-07：包文件缺失/损坏同样属于"装不上"，
        //   以前只写日志就回退，玩家看到的是原版俄文且毫无提示。
        FatalInstallFailure("找不到或无法读取汉化字库文件 hta_chs_cjk.bin"
                            "（请确认它位于游戏 update 目录下）");
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

    // ★ 不再定位引擎的 vector::resize ★
    //
    //   它是我在特征码未命中后按 RVA **猜**出来的（0x8B6660），连调用约定
    //   都是猜的。实测调用它之后容量/指针毫无变化，说明身份就是错的。
    //   而「能返回」不等于「调用约定对」—— 若它实际是 __cdecl，被调用方不
    //   清理栈，调用者的 ESP 就会偏低若干字节，函数返回时弹出的返回地址
    //   是垃圾，直接跳到 NULL（实测 0xC0000005 @ EIP=0）。
    //   所以彻底不碰它，一律用自己分配的表。
    //
    // ★ 连扫描都删掉了 ★
    //   ScanUnique 要扫 6.6MB 约 1~2 秒，而结果根本用不上（下面直接置 0）。
    //   实测（07:16 那次）正是这段无用扫描把"引擎分配器定位"拖到
    //   字体加载器之后 → 装配时分配器还是 NULL → 0 个字号装成 → 俄文乱码。
    //   教训：**废弃的东西要连它的初始化一起删干净**，
    //        留着"反正不调用"的空转代码，一样要付时间代价。
    g_vecResize = nullptr;
    g_vecResizeOk = false;
    Logf("pathd: 不使用引擎扩容函数（身份未证实，调用它有栈错乱风险）");

    // ★ 引擎分配器已在上面（LoadPackage 之前）定位完毕 —— 不重复定位。★

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

    // ★ 安装前先把游戏线程全部挂起 ★
    //
    //   ⚠⚠ 实测教训（2026-10-07）：**长时间冻结会把装配彻底搞坏** ⚠⚠
    //     症状：每次启动结果都不一样（崩溃 / 乱码 / 文字闪烁 / 完全无字）。
    //
    //     根因：CollectAllFonts 的**锚点**来自 g_seenFonts —— 那是绘制期
    //     登记（P4 在渲染线程里执行 inc g_seenCount）。
    //     而 CollectAllFonts 第 1265 行是 `if (anchors.empty()) return;`
    //     —— 锚点为空就**直接返回，连堆扫描都不跑**。
    //     冻住渲染线程 ⇒ g_seenCount 恒为 0 ⇒ 一个字体都收集不到
    //     ⇒ 汉字表全空 ⇒ 你看到的那些症状。
    //
    //     实测日志就是铁证：
    //         [冻结中] 跳过「等绘制登记稳定」，直接收集字体
    //         [失败] 一个字体都没收集到
    //
    //     「每次不一样」也由此而来：字体能不能收到，取决于冻结那一刻
    //     引擎有没有已经画过一帧 —— 纯时序竞争。
    //
    //   ⇒ 结论：**装配必须让引擎正常跑**，冻结只能用于「写那几字节」
    //     的瞬间 —— 也就是 WriteBlockSafe 已经做的事。
    //     长时间的进程级冻结在本项目里是行不通的方案。
    //     保留开关以便你随时对照，但**默认关闭**。
    const bool kFreezeInstall = GetEnvironmentVariableA("HTA_CHS_FREEZE", nullptr, 0) > 0;
    if (kFreezeInstall) {
        Logf("pathd: [实验] HTA_CHS_FREEZE 已设 —— 冻结期间安装（注意：会导致装配失败）");
        FreezeGameThreads();
    }

    // 安装 16 位索引补丁
    //
    // ★★★ 2026-10-07 方案 B：补丁安装**移交冻结门** ★★★
    //
    //   实测（还是没冻上.log）的失败模式：
    //       10:29:42.340  Init 开始慢活（定位 + 装补丁）
    //       10:29:44.105  冻结门触发 →「补丁未就绪（状态=0），放弃装配」
    //   本函数跑在 **InitThread**，而引擎主线程完全不受影响地继续跑
    //   Application::init。两者赛跑，谁快谁慢全随机：
    //       成功那次 Init 3.38s vs 引擎 4.0s   ← 只赢了 0.6s，纯属侥幸
    //       失败那次 Init >1.77s vs 引擎 1.77s ← 输了
    //
    //   ⇒ 根治不是"等 Init 装完"（等不到，且 WriteBlockSafe 要挂起当前这个
    //     引擎主线程 ⇒ 死锁），而是**把补丁安装也放进冻结门**：
    //       Init() 只装冻结门（0.2s）并备好依赖，立刻返回；
    //       引擎走到出口触发 gate，gate 在**引擎主线程已停住**的前提下
    //       同步完成「定位 → 写补丁 → 装配」。此时没有任何竞争。
    //
    //   前置依赖（分配器/包文件/字体管理器）用 g_initDepsReady 显式交接，
    //   gate 缺它就不装补丁、不装配 —— 宁可弹框退出也不留半装状态。
    g_initDepsReady = 1;
    Logf("pathd: [方案B] 依赖已就绪，补丁安装移交冻结门"
         "（Application::init 出口同步执行）");

    // 起后台线程做后续装配
    //
    // ★★ 实测失败：CreateThread 返回 NULL，GetLastError()=8 ★★
    //   (ERROR_NOT_ENOUGH_MEMORY) —— 四种栈大小（256K/128K/64K/默认）全失败。
    //   后果极其严重：**装配线程根本没起来 → 汉字表永远为空**
    //   → P4 查不到汉字 → 引擎退回单字节查表 → GBK 字节被当西里尔字母
    //   显示（用户看到的"俄语乱码 / 所有字消失"）。
    //
    //   ★ 注意：错误 8 在实际观测里**并不代表真的内存不够** ★
    //     日志同期 `Total virtual memory available = 445 MB`，给 64KB 栈绰绰有余。
    //     32 位 + 无 LAA 的进程里，CreateThread 报 8 的常见真因是
    //     **提交量/地址空间碎片化**或线程配额，而不是真的分配不出 64KB。
    //     历史上我改过三轮（含一次回退 0fe2344）都没根治 —— 因为方向错了：
    //     问题不是"怎么把线程创建成功"，而是"**根本不需要第二个线程**"。
    //
    //   ★ 正确做法（本次）★
    //     本函数 `Init` 已经运行在 DllMain 创建的 **InitThread** 里，
    //     那本来就是个独立线程。装配直接在这里同步做即可 ——
    //     游戏在它自己的线程上照常跑，不需要我们再多开一个。
    //     ⇒ 优先仍试后台线程（让初始化尽快返回），
    //       失败就**退化为在 InitThread 里同步装配**，不再有"彻底失败"这条路。
    //
    //   ★ 关于栈 ★ SetupThread 只做线性扫描与填表，256KB 足够。
    // ═══════════════════════════════════════════════════════════════════
    // ★★ 2026-10-07 方案 F：后台装配线程**已停用**（代码保留以便回退）★★
    //
    //   原因：装配现在由「冻结门」在 Application::init 成功出口同步完成，
    //   时机确定、字体完整，不需要再靠后台线程去「事后追赶」。
    //   保留这段是为了万一方案 F 不行，可以一行注释恢复。
    //
    //   恢复方法：把下面 #if 0 改成 #if 1（或删掉 #if 0/#endif）。
    // ═══════════════════════════════════════════════════════════════════
#if 0
    HANDLE th = nullptr;
    const SIZE_T stackSizes[] = { 256 * 1024, 128 * 1024, 64 * 1024, 0 };
    for (int i = 0; i < 4 && !th; ++i) {
        th = CreateThread(nullptr, stackSizes[i], SetupThread, nullptr, 0, nullptr);
        if (!th) {
            Logf("pathd: [警告] 装配线程创建失败(栈=%u) 错误 %lu，换更小的栈重试",
                 (unsigned)(stackSizes[i] / 1024), GetLastError());
        }
    }
    if (th) {
        CloseHandle(th);
        Logf("pathd: 装配线程已启动（后台装配）");
        g_enabled = true;
        Logf("pathd: 初始化返回（补丁已生效，装配在后台进行）");
        return true;
    }

    // ── 退化路径：直接在 InitThread 里同步装配 ────────────────────────
    //   这条路**必须成功**，否则游戏里一个汉字都没有。
    //   它不需要创建任何线程，因此错误 8 不可能再拦住我们。
    Logf("pathd: [退化] 装配线程创建失败 —— 改为在初始化线程内同步装配");
    Logf("pathd:        这会延迟初始化返回，但不影响游戏自己的线程");
    SetupThread(nullptr);          // 直接调用：同步等字体 → 填表 → 开闸
    Logf("pathd: [退化] 同步装配结束");
#endif  // #if 0

    g_enabled = true;
    Logf("pathd: 初始化返回（补丁已生效；装配将在 Application::init 出口同步完成）");
    return true;
}

bool IsEnabled() { return g_enabled; }

void StopRescan() { PathD_StopRescan(); }

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
        // ★ 渲染线程自报家门（解决"启动卡 30 秒"）★
        //   P4 在绘制热路径上，执行它的线程就是渲染线程。
        //   只做一次：若 g_renderTid 还是 0，就调 GetCurrentThreadId 记下来。
        //   ★ 必须保存所有会被破坏的寄存器 ★ GetCurrentThreadId 是
        //     __stdcall 无参数，返回 eax，会破坏 eax/ecx/edx。
        //   这里处于 push ebx/esi/edi **之后**、真正逻辑之前，
        //   所以 eax/ecx/edx 还没被引擎赋予含义，可以安全使用。
        cmp   dword ptr [g_renderTid], 0
        jne   pl_tid_done
        push  eax
        push  ecx
        push  edx
        call  GetCurrentThreadId
        mov   dword ptr [g_renderTid], eax
        inc   dword ptr [g_renderTidHits]
        pop   edx
        pop   ecx
        pop   eax
    pl_tid_done:
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
        jz    pl_null_noslot               ; ★ 诊断：槽表扫完没命中 ★
        cmp   dword ptr [eax], ecx         ; slot.font == Font* ?
        je    pl_gate_hit
        add   eax, 12                     ; sizeof(CjkSlot)
        dec   edx
        jmp   pl_gate
    pl_null_noslot:
        // ★ 诊断：槽表扫完都没命中当前 Font* ★
        //   这是「tips 只显示英文」最可能的解释：tips 用的字体
        //   可能是启动后才创建的，装配阶段枚举不到 → 没有槽 → 无汉字。
        //   ★ 只做两次内存写入，不碰任何寄存器 ★
        //     （P4 是绘制热路径，破坏寄存器契约会立刻出问题；
        //      之前那版用 esi 暂存是错的，已撤掉）
        inc   dword ptr [g_plNoSlot]
        mov   dword ptr [g_plLastNoSlotFont], ecx   ; ecx = 当前 Font*
        jmp   pl_null
    pl_gate_hit:
        cmp   dword ptr [eax+8], 0        ; ★ slot.ready —— 分批开闸就在这
        jne   pl_cjk_go
        inc   dword ptr [g_plNotReady]    ; ★ 诊断：槽未就绪（装配期间属正常）★
        jmp   pl_null
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

// ═══════════════════════════════════════════════════════════════════════════
// 度量路径（sub_685990）——「过场剧情不换行」的修复
//
// ─── 根因（本次会话用 IDA 字节逐条核实）───
//   引擎度量循环只按**单字节**推进，且字形查询只喂**单字节**：
//     0x66FEA0(Font*, uint8)  = 查引擎自带 256 项表 [Font+0x40]，返回 advance
//     0x66FE10(Font*, out*, uint8) = 同上，返回 pxW/pxH
//   GBK 前导字节 >= 0x81 时，引擎的 256 项表里**根本没有这一格**，
//   于是每个汉字的两个字节各走一次「查不到」分支，各贡献
//   0.125f（= xmmword_9E6A74+8，实测字节 00 00 00 3E）。
//   ⇒ 一个汉字被记成 0.25 em，真实约 1.0 em。**宽度低估到 1/4**，
//     整行累加到头也够不着行宽阈值 -> 不折行（字仍占位，所以看着只是不换行）。
//
// ─── 为什么不能改 sub_66FEA0/sub_66FE10 ★★
//   两个函数都只有 0x22 / 0x3B 字节，看起来"小到能整体重写"，
//   但它们**只收一个字节**，拿不到 GBK 后继字节 b2 ——
//   单靠函数内部无法判断"这个 0x81 后面还有没有第二个字节"。
//   （xref 已确认：两者各自只有 sub_685990 一个调用者，
//     0x66FEA0 ← 0x685B26、0x66FE10 ← 0x685B3F。）
//   ⇒ hook 点必须落在**循环内部**。
//
// ─── 真实栈位移（★ IDA 的 stack_frame 在此函数不可信 ★）───
//   IDA 显示 `[esp+3Ch+var_28]`，但字节约 dis 是 `D8 44 24 14`，
//   即**真实 disp = IDA 显示值 − 0x34**。下面全部用字节 disp，
//   含义都是「引擎循环体内那一点的 esp 位移」：
//     [esp+0x14]  宽度累加器 var_28      （0x685B2B fadd [esp+14]）
//     [esp+0x18]  行数计数   var_20      （0x685BBE add [esp+18],1）
//     [esp+0x1C]  字符串长度 strlen      （0x685BD4 cmp edi,[esp+1C]）
//     [esp+0x20]  文本基址**指针**       （0x685B9C mov ecx,[esp+20]）
//     [esp+0x4C]  当前字符（1 字节）    （0x685B21 mov esi,[esp+4C]）
//     ecx         Font*（全程不变，0x685B5B/0x685B9C 反复用它）
//   ★ strlen 槽位经两处独立确认：0x685A61 存 strlen、0x685BD4 与之 cmp，
//     两条落在同一绝对槽 [S−0x20]。这是双字节推进能安全越界判定的依据。
//
// ─── 为什么用 jmp 而不是 call ───
//   P8 挂在 `add edi,1` 上（顺序执行流），call 会压返回地址、助手里还得
//   `add esp,4` 丢它 —— 那正是 P2/P3 崩在助手内部的手法（hta.exe0057）。
//   jmp 完全不动栈。
// ═══════════════════════════════════════════════════════════════════════════

// —— P7：度量宽度（jmp @0x685B21，覆盖 11 字节）——
//
//   覆盖引擎这三条（0x685B21..0x685B2B）：
//       0x685B21  mov esi,[esp+0x4C]   4 字节   8B 74 24 4C
//       0x685B25  push esi             1 字节   56
//       0x685B26  call sub_66FEA0      5 字节   E8 75 A3 FE FF
//       0x685B2B  fadd [esp+0x14]      ← **不在补丁内**（下一步）
//                                    合计    10 字节
//
//   出口契约（引擎紧接着 0x685B2B 就要 fadd）：
//     st(0) = 该字符的 advance；其余一切保持原样。
//     ★ esp 不能动 ★ —— 下一条 fadd 用 [esp+0x14]，一动就全错。
__declspec(naked) void __cdecl PathD_MsrAdvanceW() {
    __asm {
        ; ★★★★★ 「过场不换行」的决定性诊断 ★★★★★
        ;
        ;   引擎的折行判定（0x685B1B，sub_685990 反编译）：
        ;       if (v15 && a6 && (v13/(font[24]/font[28])) > (v15[2] + *v15))
        ;           { *a6 = v8; break; }
        ;   v15 = a4 = **可用行宽指针**，被编译器缓存在 **ebp**。
        ;   证据：0x685AF1 就是 `test ebp,ebp`（85 ED），若 ebp==0
        ;        直接跳到 0x685B21（我们的 P7 点）—— 说明 ebp 是
        ;        这个折行判定的开关。
        ;   ⇒ **只要 ebp == 0，引擎永不折行**，文字一路画到边界被裁掉，
        ;     表现就是用户看到的「长文本被截断」。
        ;   所以这里统计 ebp 为 0 / 非 0 的次数：这是"度量对不对"
        ;   之外的另一半答案，而且不需要改任何行为。
        test  ebp, ebp
        jz    mw_no_width
        inc   dword ptr [g_msrHasWidth]
        jmp   mw_width_done
    mw_no_width:
        inc   dword ptr [g_msrNoWidth]
    mw_width_done:

        movzx edx, byte ptr [esp+4Ch]      ; b1
        mov   esi, edx                     ; ★ 还原被覆盖的 mov esi,[esp+4C]
        cmp   edx, 81h
        jb    mw_ascii                     ; <0x81 -> 引擎原语义

        // ── 只有 GBK 前导字节才继续；必须确认后继字节在界内 ──
        lea   eax, [edi+1]
        cmp   eax, dword ptr [esp+1Ch]     ; b1+1 < strlen ?
        jge   mw_ascii                     ; 越界 -> 当单字节处理
        mov   eax, dword ptr [esp+20h]     ; ★ 文本基址**指针** ★
        test  eax, eax
        jz    mw_ascii                     ; 基址为空，绝不解引用
        movzx eax, byte ptr [eax+edi+1]    ; ★ b2：先取基址，再间接寻址 ★
        shl   edx, 8
        or    edx, eax                     ; edx = GBK 码
        mov   dword ptr [g_msrGbk], edx     ; ★ 落内存：ecx 要留给 Font*

        ; ★★★★★ Font* 在哪个栈槽 —— 用引擎自己的两次读取闭环确证 ★★★★★
        ;
        ; 引擎在**同一段循环**里两次读这个槽，中间隔着 push ebx / push ebp：
        ;     0x685A0B  call sub_679700        ; 返回 Font* 于 eax
        ;     0x685A10  mov  ecx, eax          ; 8B C8
        ;     0x685A12  mov  [esp+08h], ecx    ; 89 4C 24 08  ← 写入（push 之前）
        ;     0x685A5B  mov  ecx, [esp+08h]    ; 8B 4C 24 08  ← 读回（push 之前）
        ;     0x685BC9  mov  ecx, [esp+10h]    ; 8B 4C 24 10  ← 读回（push 之后，循环体内）
        ;     0x685AFD  movss xmm1,[ecx+18h]   ; F3 0F 10 49 18  当 Font* 用（取宽高比）
        ;
        ;   0x685A65 push ebx / 0x685A77 push ebp 使循环体内 esp 比上面低 8：
        ;       0x08 + 8 = 0x10   ✓ 两次读的是**同一个槽**
        ;   ⇒ 循环体内 Font* = **[esp+10h]**
        ;
        ; ★ 交叉验证同一换算（全部 +8，一一对上）★
        ;     宽度累加器 : 0x685A37 写 [esp+0Ch] → 0x685B2B 读 [esp+14h]  (D8 44 24 14)
        ;     strlen     : 0x685A61 写 [esp+14h] → 0x685BD4 读 [esp+1Ch]  (3B 7C 24 1C)
        ;   我代码里在用的 0x14 / 0x1C 与此**完全吻合** —— 说明换算本身没错，
        ;   错的只是我把 Font* 的槽位判断成了别处。
        ;
        ; ★★ 我为什么会连错三次（0x10 → 0x50 → 0x58）★★
        ;   1) 上次崩溃 hta.exe0101 现场 EAX=3、崩在 fld [eax+2Ch]。
        ;      真因是**漏了 [Font+0x40] 这层解引用** —— 旧代码把 Font* 直接
        ;      当字形表基址，于是读到 [Font + 0x45*4] = [Font+0x114] = 3。
        ;   2) 我却误判成「槽位取错了」，于是去凑 IDA 的 arg_N 标签，
        ;      推出 0x50 / 0x58 —— 全部越出本函数栈帧（只 sub esp,2Ch），
        ;      [P7诊断] 打印 Font*=0x00000000 正是读到帧外的铁证。
        ;   **教训：崩溃点是解引用时，先怀疑少了一层/多了一层间接，
        ;     而不是先去改栈槽。**
        mov   ecx, dword ptr [esp+10h]     ; ★ Font*（与引擎 0x685BC9 同一槽）★

        ; 诊断：记录 Font* 与字符，供一次性核对
        mov   dword ptr [g_msrDiagFont], ecx
        mov   eax, dword ptr [g_msrDiagCount]
        cmp   eax, 6
        jae   mw_diag_done
        inc   eax
        mov   dword ptr [g_msrDiagCount], eax
        mov   dword ptr [g_msrDiagChar], edx
    mw_diag_done:

        mov   eax, offset g_cjkSlots
        mov   edx, dword ptr [g_cjkSlotCount]
        test  edx, edx
        jz    mw_nodef
        ; ★ 硬边界护栏：g_cjkSlots 是 MAX_CJK_TABLES(16) 项 × 12 字节 = 192 字节。
        ;   循环**不能只靠计数器终止** —— 实测崩溃 hta.exe0102 就是这么来的：
        ;   edx 用 dec 递减，dec 不置 CF，于是无条件回跳；一旦这个 Font*
        ;   压根没被登记进 g_cjkSlots（tips 用的字体就没登记），
        ;   edx 归零后继续减成 0xFFFFFFFF，eax 一路 +12 走出数组，
        ;   最后 cmp [eax],ecx 读在 0x53B21000（DLL 映像尾边界）上炸掉。
        ;   —— 教训：汇编里的循环**永远要有一条独立于计数器的地址边界判据**。
    mw_gate:
        cmp   eax, offset g_cjkSlots + MAX_CJK_TABLES * 12
        jae   mw_nodef_noslot               ; ★ 地址护栏：不依赖计数器 ★
        test  edx, edx
        jz    mw_nodef_noslot               ; ★ 计数器判零（dec 不置 CF，必须显式测）★
        cmp   dword ptr [eax], ecx         ; slot.font == Font* ?（一内存一寄存器，合法）
        je    mw_gate_hit
        add   eax, 12                      ; sizeof(CjkSlot)
        dec   edx
        jmp   mw_gate
    mw_gate_hit:
        cmp   dword ptr [eax+8], 0         ; ★ slot.ready（分批开闸就在这）
        je    mw_nodef_notready
        mov   edx, dword ptr [eax+4]       ; 该字体的 64K 表
        test  edx, edx
        jz    mw_nodef_notable
        cmp   edx, 10000h
        jb    mw_nodef_notable
        cmp   edx, 7FFFFFFFh
        jae   mw_nodef_notable
        ; ★ 诊断：把真正命中的槽信息记下来（此前只记 Font*/字符，
        ;   导致日志里"表=0 字形=0"看起来像失败，其实那两个变量从未被赋值）
        mov   dword ptr [g_msrDiagTable], edx
        mov   ecx, dword ptr [g_msrGbk]    ; ★ 扫到这儿 Font* 已用完，ecx 改作下标
        mov   eax, dword ptr [edx+ecx*4]   ; 该 GBK 的字形
        mov   dword ptr [g_msrDiagGlyph], eax
        test  eax, eax
        jz    mw_nodef_noglyph
        cmp   eax, 10000h                  ; ★ 字形指针区间（P4 已验证的判据）★
        jb    mw_nodef_noglyph
        cmp   eax, 7FFFFFFFh
        jae   mw_nodef_noglyph
        fld   dword ptr [eax+2Ch]          ; ★ 真实 advance ★
        inc   dword ptr [g_msrHitCjk]      ; ★ 诊断：汉字分支成功命中次数 ★
        mov   ecx, dword ptr [g_msrAfterW] ; ★ jmp 绝对地址须先过寄存器
        jmp   ecx                         ;   → 回 0x685B2B（引擎的 fadd）

        ; ── 分类出口（只为诊断区分，语义同 mw_nodef）──
        ;   ★ 关键：g_msrFail* 是**自进程启动以来的累计值** ★
        ;     装配要跑好几秒，期间这些计数必然很大（实测"未就绪=352"），
        ;     那是装配**还没填完**时的正常历史，不代表装配后的行为。
        ;     所以额外记一份"装配完成后"的计数：装配收尾时把当前值
        ;     快照到 g_msrPost*，两个值的差才是装配后的真实分布。
    mw_nodef_noslot:
        inc   dword ptr [g_msrFailNoSlot]
        jmp   mw_nodef
    mw_nodef_notready:
        inc   dword ptr [g_msrFailNotReady]
        jmp   mw_nodef
    mw_nodef_notable:
        inc   dword ptr [g_msrFailNoTable]
        jmp   mw_nodef
    mw_nodef_noglyph:
        inc   dword ptr [g_msrFailNoGlyph]
        jmp   mw_nodef

    mw_ascii:
        // ★ 与 sub_66FEA0 同语义：ecx=Font*，两级解引用 ★
        //   sub_66FEA0 反编译：
        //       v2 = *(this + 16) + 4*a2;      // this+0x40 = 表指针
        //       if (*(_DWORD*)v2) return *(float*)(*(_DWORD*)v2 + 44);
        //   ★ 上一轮崩溃 hta.exe0101 就是漏了第一级：
        //     旧代码把 Font* 直接当表基址，于是 fld [Font + ch*4 + 0x2C]
        //     对 'E'(0x45) 读到 [Font+0x114] = 3 → fld [3+0x2C] = [0x2F] 炸。
        //   所以这里必须 **先 [Font+0x40] 取表**，再从表里取字形。
        mov   ecx, dword ptr [esp+10h]     ; Font*（与引擎 0x685BC9 同一槽）
        test  ecx, ecx
        jz    mw_nodef
        mov   eax, ecx
        cmp   eax, 10000h                  ; Font* 必须落在引擎堆区
        jb    mw_nodef
        cmp   eax, 7FFFFFFFh
        jae   mw_nodef
        mov   eax, dword ptr [ecx+40h]     ; ★ 第一级：Font->字形表（256 项 vector）★
        test  eax, eax
        jz    mw_nodef
        cmp   eax, 10000h
        jb    mw_nodef
        cmp   eax, 7FFFFFFFh
        jae   mw_nodef
        movzx edx, byte ptr [esp+4Ch]      ; 字符
        mov   ecx, dword ptr [eax+edx*4]   ; ★ 第二级：表[ch] = 字形 ★
        test  ecx, ecx
        jz    mw_nodef
        cmp   ecx, 10000h                  ; 字形指针区间（拦下 3 这种野值）
        jb    mw_nodef
        cmp   ecx, 7FFFFFFFh
        jae   mw_nodef
        fld   dword ptr [ecx+2Ch]          ; glyph+0x2C = advance
        inc   dword ptr [g_msrHitAscii]    ; ★ 诊断：ASCII 分支命中次数 ★
        mov   ecx, dword ptr [g_msrAfterW]
        jmp   ecx

    mw_nodef:
        inc   dword ptr [g_msrHitNoDef]    ; ★ 诊断：缺字出口次数 ★
        inc   dword ptr [g_msrPostNoDef]   ; ★ 诊断：装配后计数（见下方说明）
        // ★ 不是 0 ★：引擎缺字分支 fld 的是 xmmword_9E6A74+8，
        //   实测字节 00 00 00 3E = 0.125。误判成 0 会得出
        //   「宽度永不累加所以永不折行」的错误结论。
        fld   dword ptr [g_msrNoGlyphW]     ; 0.125f，与引擎缺字行为一致
        mov   ecx, dword ptr [g_msrAfterW]
        jmp   ecx
    }
}

// —— P8：度量双字节推进（jmp @0x685BD1，覆盖 13 字节）——
//
//   覆盖引擎这四条（0x685BD1..0x685BDE）：
//       0x685BD1  add edi,1        3 字节   83 C7 01
//       0x685BD4  cmp edi,[esp+1C] 4 字节   3B 7C 24 1C
//       0x685BD8  jl  0x685A80     6 字节   0F 8C A2 FE FF FF
//       0x685BDE  jmp 0x685C12     ← **不在补丁内**
//                                    合计    13 字节
//
//   ★ 覆盖范围必须含 jl（6 字节），不能只盖 add+cmp（7 字节）——
//     那样 jl 会落到补丁区中间的 NOP 上，执行结果不可控。
//   ★ 上次 7cee8cf 失败的真因：把 add/cmp/jl 一起盖成 call+13B NOP，
//     助手 ret 后落到 0x685BDE（jmp 退出循环），循环**只跑一轮**
//     => 度量只算一个字符 => 文字整体右偏 + tooltip 连尺寸都没有。
//     本助手自己完成 cmp + 回跳，**绝不依赖 fall-through**。
//
//   出口契约：edi 按 GBK 推进，然后必须自己跳回循环头或退出循环。
//     除 edi 外全部寄存器原样保留（含 ebx/bl —— 引擎契约要求）。
__declspec(naked) void __cdecl PathD_MsrAdvanceB() {
    __asm {
        // 看当前字节决定推进量：GBK 前导字节且未越界 -> 2，否则 1
        movzx eax, byte ptr [esp+4Ch]      ; 当前字符 b1（★ 栈槽 ★）
        add   edi, 1                        ; ★ 先按单字节推进（ASCII 路径不变）
        cmp   eax, 81h
        jb    mb_loop
        // 越界检查：edi（已 +1）必须 < strlen，即 b1+1 < strlen
        cmp   edi, dword ptr [esp+1Ch]
        jge   mb_loop                       // 后继字节不存在 -> 只 +1
        inc   edi                           ; ★ 双字节：+2
    mb_loop:
        cmp   edi, dword ptr [esp+1Ch]      ; edi < strlen ?
        jl    mb_go
        mov   eax, dword ptr [g_msrLoopExit] ; 退出 -> 0x685C12
        jmp   eax
    mb_go:
        mov   eax, dword ptr [g_msrLoopHead] ; 回 0x685A80（★ jmp 进入，栈上无返回地址 ⇒ 绝不能 ret）
        jmp   eax
    }
}

//   ★★★ 上次 7cee8cf 失败的真因（务必不要重犯）★★★

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
