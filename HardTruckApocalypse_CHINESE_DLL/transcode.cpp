// transcode.cpp —— GBK 双字节 → 引擎单字节槽位 的转换层
//
// 引擎只会「读一个字节 → 查 Font+0x40[字节] → 画字形」。汉字是 2 字节，
// 直接塞进去会被拆成两个无关字形（这就是现在乱码的原因）。
//
// 本模块把 GBK 串转成引擎能逐字节消费的序列：
//
//     ASCII (0x20..0x7F)         原样保留
//     GBK 双字节 (0x81..0xFE 起)  转成 [0x1B][槽位字节] 两个字节
//
// 为什么是 0x1B 作转义标记（关键，它让引擎原逻辑零改动即可工作）：
//   度量 sub_685990 @0x685A8B 与绘制 sub_685CA0 @0x6864E3 都有
//       cmp al, 20h / jnb ...      ; >= 0x20 才走字形路径
//   即**所有 < 0x20 的字节被静默跳过、贡献零宽度、不查字形表**。
//   于是 [0x1B][slot] 等价于「一个宽度为 0 的字符 + 一个正常单字节字符」，
//   引擎的度量与绘制都自然得到正确结果 —— **无需替换任何引擎函数**。
//
// ⚠ 槽位字节的取值禁区（已逐指令核实）：
//   0x00..0x1F  被静默吞掉 → 槽位无效
//   0x23 '#'    切状态位，跳过
//   0x24 '$'    状态位未置位时跳过
//   0x26 '&'    同 '$'
//   0x40 '@'    **8 字节 hex 颜色转义**：读 [p+1..p+8] 送 sscanf("%x")，
//               然后 add esi,8 / add edi,8 —— 会把后面 8 个字节吃掉！
//               （sub_685CA0 @0x6864F2 判定，0x68650D..0x686572 读取并跳过）
//   0x7C '|'    度量里有状态切换，按危险处理
//
// 因此槽位只取 **0x80..0xFF**：全部 >= 0x20 且避开上述全部禁区。
#include "pch.h"
#include "plugin.h"

namespace transcode {

namespace {

// 转义标记：< 0x20，引擎静默跳过
const uint8_t kEscape = 0x1B;

// 槽位字节区间
const uint8_t kSlotFirst = 0x80;
const uint8_t kSlotLast  = 0xFF;
const int     kSlotCount = kSlotLast - kSlotFirst + 1;   // 128

} // namespace

// ---------------------------------------------------------------------------
// 槽位字节合法性
// ---------------------------------------------------------------------------
bool IsUsableSlot(uint8_t b) {
    if (b == 0x00) return false;   // 串终止
    if (b < 0x20)  return false;   // 被静默吞掉
    if (b == 0x23) return false;   // '#'
    if (b == 0x24) return false;   // '$'
    if (b == 0x26) return false;   // '&'
    if (b == 0x40) return false;   // '@' 8 字节颜色转义
    if (b == 0x7C) return false;   // '|'
    return true;
}

// ---------------------------------------------------------------------------
// 快速判断：是否需要转码
//
// 用于绘制/度量的 ASCII 快速路径 —— 全 ASCII 时直接走原函数，零回归风险。
// ---------------------------------------------------------------------------
bool NeedsTranscode(const char* s) {
    if (!s) return false;
    for (const uint8_t* p = (const uint8_t*)s; *p; ++p) {
        if (*p >= 0x80) return true;    // 有 GBK 双字节
        if (*p == kEscape) return false; // 已转过码，无需再转
    }
    return false;
}

// ---------------------------------------------------------------------------
// 转换
// ---------------------------------------------------------------------------
bool GbkToSlots(const char* src, char* dst, size_t dstCap) {
    if (!src || !dst || dstCap < 2) return false;

    const uint8_t* p = (const uint8_t*)src;
    size_t di = 0;
    int unmapped = 0;

    while (*p) {
        uint8_t b = *p;

        // 已是转义序列：原样搬运（避免二次转码）
        if (b == kEscape) {
            if (di + 2 >= dstCap) { dst[di] = 0; return false; }
            dst[di++] = (char)p[0];
            if (p[1]) { dst[di++] = (char)p[1]; p += 2; }
            else      { ++p; }
            continue;
        }

        // ASCII 直接透传
        if (b < 0x80) {
            if (di + 1 >= dstCap) { dst[di] = 0; return false; }
            dst[di++] = (char)b;
            ++p;
            continue;
        }

        // 非前导字节（悬空尾字节）：按单字节透传，避免死循环
        if (!font::IsLeadByte(b)) {
            if (di + 1 >= dstCap) { dst[di] = 0; return false; }
            dst[di++] = (char)b;
            ++p;
            continue;
        }

        uint8_t t2 = p[1];
        if (!t2) {   // 串尾悬空前导字节
            if (di + 1 >= dstCap) { dst[di] = 0; return false; }
            dst[di++] = (char)b;
            ++p;
            continue;
        }

        uint16_t gbk = ((uint16_t)b << 8) | t2;
        uint8_t slot = 0;
        if (!slotmap::Lookup(gbk, &slot)) {
            ++unmapped;
            if (di + 1 >= dstCap) { dst[di] = 0; return false; }
            dst[di++] = '?';          // 无映射：兜底，不崩
            p += 2;
            continue;
        }

        if (di + 2 >= dstCap) { dst[di] = 0; return false; }
        dst[di++] = (char)kEscape;
        dst[di++] = (char)slot;
        p += 2;
    }

    dst[di] = 0;
    if (unmapped)
        Logf("transcode: %d 个汉字在 slotmap 里没有映射，显示为 '?'", unmapped);
    return true;
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------
bool SelfTest(char* errBuf, size_t errCap) {
    // 1) 转义字节必须 < 0x20，否则会被当普通字形画出来
    if (kEscape >= 0x20) {
        if (errBuf && errCap)
            _snprintf_s(errBuf, errCap, _TRUNCATE,
                        "转义字节 0x%02X 不在 <0x20 区间，会被当作字形绘制", kEscape);
        return false;
    }
    // 2) 槽位区间内每个字节都必须安全
    for (int i = 0; i < kSlotCount; ++i) {
        uint8_t b = (uint8_t)(kSlotFirst + i);
        if (!IsUsableSlot(b)) {
            if (errBuf && errCap)
                _snprintf_s(errBuf, errCap, _TRUNCATE,
                            "槽位字节 0x%02X 会被绘制路径劫持", b);
            return false;
        }
    }
    // 3) 转义字节不能落在槽位区间内（避免歧义）
    if (kEscape >= kSlotFirst && kEscape <= kSlotLast) {
        if (errBuf && errCap)
            _snprintf_s(errBuf, errCap, _TRUNCATE,
                        "转义字节 0x%02X 与槽位区间重叠", kEscape);
        return false;
    }
    return true;
}

size_t EscapeByte()   { return kEscape;    }
int    SlotsPerFont() { return kSlotCount; }

} // namespace transcode
