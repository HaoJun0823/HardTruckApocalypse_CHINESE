// text_hooks.cpp —— 文本转码挂钩
//
// ═══════════════════════════════════════════════════════════════════════════
// 为什么只钩这一个点就够（关键设计）
// ═══════════════════════════════════════════════════════════════════════════
//
// 逆向确认：引擎的文本度量 sub_685990 与绘制 sub_685CA0 都是**逐字节**循环，
// 且对 <0x20 的字节一律「静默跳过、零宽度、不查字形表」：
//
//     sub_685990 @0x685A91   cmp al, 20h / jnb ...   ; >=0x20 才走字形路径
//     sub_685CA0 @0x6864E6   cmp bl, 20h / jnb ...
//
// 因此只要把 GBK 双字节序列**就地**换成 [0x1B][槽位]，引擎原有的度量与绘制
// 都自然得到正确结果 —— 无需替换任何引擎函数，也无需自建图集页。
//
// 且这个替换是**等长**的（2 字节 -> 2 字节），所以：
//   * 不需要额外缓冲
//   * 不需要改字符串长度/容量
//   * 不涉及所有权问题
//
// ═══════════════════════════════════════════════════════════════════════════
// 挂钩点选择
// ═══════════════════════════════════════════════════════════════════════════
//
// sub_406F50 = String3d::operator=(const String3d&) / (const char**)
//   它是**所有 XML 文本字段落到内存的物理必经之路**：
//   sub_414740（XML 属性 -> String3d）内部就是
//        sub_407220(tmp, attrValue);  sub_406F50(dst, &tmp);
//   而 dialog / quest / model_names / uiEditStrings 的加载器都走 sub_414740。
//
// 因此钩 sub_406F50 的「目标」侧即可覆盖全部 XML 文本，且：
//   * 它在拷贝**之后**，dst 的缓冲已按新串长度分配好（等长替换，安全）
//   * <0x20 的控制字节天然不会被文本包含，所以已转码的串不会被重复转码
//
// 为什么不钩 sub_407220：它同时搬运资源路径、节点名、map 键（"id"/"path"/
// "file"/"_localizedform_"），盲转码会**直接毁掉文件加载与查表**。
// 钩 sub_406F50 并只在「目标串含 >=0x80 字节」时处理，风险低得多。
#include "pch.h"
#include "plugin.h"

namespace texthook {

namespace {

uintptr_t g_assignTarget = 0;    // sub_406F50
void*     g_trampAssign  = nullptr;

// sub_406F50 是 __thiscall：this 在 ecx，参数 [esp+4] = const char**
typedef const char** (__fastcall *AssignFn)(void* self, void* /*edx*/, const char** src);

// 转码上限：防止异常长串或死循环
const size_t kMaxLen = 0x4000;

} // namespace

// ---------------------------------------------------------------------------
// 就地转码
//
// 把 p 指向的缓冲里的 GBK 双字节序列换成 [0x1B][槽位]。
// 等长替换，因此不需要改长度、不移动数据、不重新分配。
// 返回被替换的汉字数。
// ---------------------------------------------------------------------------
int TranscodeInPlace(char* p, size_t len) {
    if (!p || !len) return 0;
    if (len > kMaxLen) return 0;         // 异常长度，放弃（宁可不转也不要越界）

    int n = 0;
    uint8_t* s = (uint8_t*)p;
    size_t i = 0;

    while (i < len) {
        uint8_t b = s[i];
        if (b == 0) break;

        // 已经转码过的序列（含 0x1B）→ 跳过，避免二次转码
        if (b == transcode::EscapeByte()) {
            i += 2;
            continue;
        }
        // ASCII 原样
        if (b < 0x80) { ++i; continue; }

        // 需要前导字节 + 尾字节
        if (!font::IsLeadByte(b) || i + 1 >= len) { ++i; continue; }
        uint8_t t2 = s[i + 1];
        if (!t2) break;

        uint16_t gbk = ((uint16_t)b << 8) | t2;
        uint8_t slot = 0;
        if (!slotmap::Lookup(gbk, &slot)) {
            // 无映射：留原样（会显示成两个无关字形，但不会破坏长度）
            i += 2;
            continue;
        }

        s[i]     = (uint8_t)transcode::EscapeByte();
        s[i + 1] = slot;
        ++n;
        i += 2;
    }
    return n;
}

// ---------------------------------------------------------------------------
// detour
// ---------------------------------------------------------------------------
static const char** __fastcall AssignDetour(void* self, void* edx, const char** src) {
    AssignFn orig = (AssignFn)g_trampAssign;
    const char** r = orig(self, edx, src);

    // 拷贝完成后，对目标缓冲做就地转码。
    // self 是 String3d*：+0 = char* m_charPtr
    __try {
        if (self) {
            char* dst = *(char**)self;
            if (dst) {
                // 只处理确实需要转码的串（全 ASCII 直接跳过，零开销零风险）
                if (transcode::NeedsTranscode(dst)) {
                    size_t len = 0;
                    while (len <= kMaxLen && dst[len]) ++len;
                    if (len <= kMaxLen) {
                        int n = TranscodeInPlace(dst, len);
                        if (n > 0)
                            Logf("texthook: 就地转码 %d 个汉字 (%s)", n, dst);
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("texthook: 转码异常 0x%08X（已忽略）", (unsigned)GetExceptionCode());
    }
    return r;
}

// ---------------------------------------------------------------------------
// 安装
// ---------------------------------------------------------------------------
int Install(HMODULE gameModule) {
    int found = 0;

    // sub_407220（用于消歧定位 sub_406F50 的邻域）
    uintptr_t a220 = pattern::ScanModule(gameModule, "56 8B F1 57 8B 7C 24 ?? 85 FF");
    // sub_406F50：String3d = String3d
    uintptr_t a6f50 = pattern::ScanModule(gameModule, "53 8B 5C 24 ?? 56 8B F1 3B F3 57");

    // 若 sub_407220 有多个命中（ROC），取离 sub_406F50 最近的作为确认
    if (a6f50 && a220) {
        a220 = 0;   // 仅用于人工核对，这里不需要
    }
    (void)a220;

    if (a6f50) {
        ++found;
        Logf("texthook: String3d 赋值 sub_406F50 -> 0x%08X", (unsigned)a6f50);
    } else {
        Logf("texthook: 未找到 String3d 赋值函数");
        return 0;
    }

    // 有映射表才挂钩（没有映射时转码没意义）
    if (!slotmap::IsLoaded()) {
        Logf("texthook: 未加载槽位映射表，跳过挂钩");
        return found;
    }

    if (hook::Install("String3d::assign", a6f50,
                      (void*)AssignDetour, &g_trampAssign)) {
        g_assignTarget = a6f50;
        Logf("texthook: 挂钩成功，文本转码已启用");
    } else {
        Logf("texthook: 挂钩失败");
    }
    return found;
}

uintptr_t GetAssignTarget() { return g_assignTarget; }

} // namespace texthook
