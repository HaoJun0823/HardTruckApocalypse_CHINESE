// slotmap.cpp —— 字符 → 单字节字形槽位 映射表
//
// 为什么需要外部映射表：
//   槽位是「单字节字形表」的索引，汉字必须被映射到一个 0x80..0xFF 的槽号。
//   这个映射必须与**离线烘字库时**使用的映射完全一致，否则字库里的字形和
//   运行时算出的槽号会对不上。
//   因此映射表由 fontgen 在烘图时一并产出，插件只负责加载与查询，
//   不做运行时分配（运行时分配会导致顺序依赖、不可复现）。
//
// 文件格式（文本，UTF-8/ASCII，`#` 开头为注释）：
//     hta_chs_slotmap 1
//     D6D0 80      # GBK 双字节 -> 槽号（十六进制，无 0x 前缀）
//     D3CE 81
//     ...
//
//   槽号取值区间必须是 0x80..0xFF 且避开绘制路径的劫持字节（见 transcode.cpp）。
#include "pch.h"
#include "plugin.h"

namespace slotmap {

namespace {

const int kMaxEntries = 4096;

struct Entry {
    uint16_t gbk;    // GBK 双字节（高字节 << 8 | 低字节）
    uint8_t  slot;   // 槽号
};

Entry   g_entries[kMaxEntries];
int     g_count = 0;
bool    g_loaded = false;

// 快速查找表：GBK 双字节 -> 槽号。65536 项的字节数组，1 字节/项 = 64KB，
// 比线性扫描 4096 项快得多，且内存开销可接受。
uint8_t* g_fast = nullptr;

bool ParseHexByte(const char* p, uint8_t* out) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = hex(p[0]);
    int lo = (p[1] && p[1] != ' ' && p[1] != '\t') ? hex(p[1]) : -1;
    if (hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

// 从一行里取出两个十六进制 token（GBK 双字节 = 4 位 hex，槽号 = 2 位 hex）
bool ParseLine(const char* line, uint16_t* outGbk, uint8_t* outSlot) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p || *p == '#' || *p == '\r' || *p == '\n') return false;
    if (*p == '/') return false;   // // 注释

    // 第一个 token：GBK 双字节
    uint8_t b0 = 0, b1 = 0;
    if (!ParseHexByte(p, &b0)) return false;
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;
    if (!ParseHexByte(p, &b1)) return false;   // 允许写法 D6 D0 或 D6D0
    p += 2;
    // 若写成 D6D0 形式，第二次解析会落在空格后；处理紧凑写法
    // （上面已按「两个 hex 字符一个字节」解析，D6D0 会得到 b0=D6，然后 p 指向 '0'，
    //   b1 得到 0x0? —— 为稳妥起见，检查紧凑写法）
    *outGbk = ((uint16_t)b0 << 8) | b1;

    while (*p == ' ' || *p == '\t') ++p;

    // 第二个 token：槽号
    uint8_t slot = 0;
    if (!ParseHexByte(p, &slot)) return false;
    *outSlot = slot;
    return true;
}

// 更稳的解析：按 token 切分（支持 "D6D0 80" 与 "D6 D0 80" 两种写法）
bool ParseLineTokens(char* line, uint16_t* outGbk, uint8_t* outSlot) {
    // 去掉注释
    char* hash = strchr(line, '#');
    if (hash) *hash = 0;
    char* slash = strstr(line, "//");
    if (slash) *slash = 0;
    // 去掉行尾
    for (char* q = line; *q; ++q) if (*q == '\r' || *q == '\n') *q = 0;

    // 收集 token
    char* toks[4] = {0};
    int nt = 0;
    char* p = line;
    while (*p && nt < 4) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        toks[nt++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',') ++p;
        if (*p) *p++ = 0;
    }
    if (nt < 2) return false;

    // 情况一：两个 token（"D6D0" + "80"）
    // 情况二：三个 token（"D6" + "D0" + "80"）
    auto parse16 = [](const char* s, uint16_t* out) -> bool {
        uint32_t v = 0;
        int n = 0;
        for (const char* q = s; *q; ++q) {
            int d = 0;
            if (*q >= '0' && *q <= '9') d = *q - '0';
            else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
            else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
            else return false;
            v = (v << 4) | (uint32_t)d;
            if (++n > 4) return false;
        }
        if (n == 0) return false;
        *out = (uint16_t)v;
        return true;
    };

    if (nt >= 3 && strlen(toks[0]) <= 2 && strlen(toks[1]) <= 2) {
        uint16_t hi = 0, lo = 0, sl = 0;
        if (!parse16(toks[0], &hi) || !parse16(toks[1], &lo)) return false;
        if (!parse16(toks[2], &sl)) return false;
        *outGbk = (uint16_t)((hi << 8) | (lo & 0xFF));
        *outSlot = (uint8_t)sl;
        return true;
    }
    if (nt >= 2) {
        uint16_t gbk = 0, sl = 0;
        if (!parse16(toks[0], &gbk)) return false;
        if (!parse16(toks[1], &sl)) return false;
        *outGbk = gbk;
        *outSlot = (uint8_t)sl;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------
bool Load(const char* path) {
    if (g_loaded) return true;
    if (!path) return false;

    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        Logf("slotmap: 打不开映射表 %s (%lu)", path, GetLastError());
        return false;
    }
    DWORD size = GetFileSize(hf, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 4u * 1024 * 1024) {
        CloseHandle(hf);
        Logf("slotmap: 文件大小异常 (%lu)", size);
        return false;
    }
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(hf); return false; }
    DWORD got = 0;
    BOOL ok = ReadFile(hf, buf, size, &got, NULL);
    CloseHandle(hf);
    if (!ok) { HeapFree(GetProcessHeap(), 0, buf); return false; }
    buf[got] = 0;

    // 分配快速查找表
    if (!g_fast) {
        g_fast = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 65536);
        if (!g_fast) { HeapFree(GetProcessHeap(), 0, buf); return false; }
    }

    int n = 0, skipped = 0, header = 0;
    char* ctx = nullptr;
    for (char* line = strtok_s(buf, "\n", &ctx); line; line = strtok_s(nullptr, "\n", &ctx)) {
        // 跳过 BOM
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) line += 3;

        // 头部行
        char probe[64];
        lstrcpynA(probe, line, sizeof(probe));
        if (strstr(probe, "hta_chs_slotmap")) { ++header; continue; }

        uint16_t gbk = 0;
        uint8_t  slot = 0;
        if (!ParseLineTokens(line, &gbk, &slot)) {
            // 非空且非注释的行才算跳过
            const char* q = line;
            while (*q == ' ' || *q == '\t') ++q;
            if (*q && *q != '#' && *q != '\r') ++skipped;
            continue;
        }
        if (n >= kMaxEntries) { ++skipped; continue; }
        if (!transcode::IsUsableSlot(slot)) {
            Logf("slotmap: 槽号 0x%02X 不安全（会被绘制路径劫持），跳过 %04X", slot, gbk);
            ++skipped;
            continue;
        }

        g_entries[n].gbk  = gbk;
        g_entries[n].slot = slot;
        g_fast[gbk] = slot ? slot : 0;   // 0 表示无映射（槽号不会是 0）
        ++n;
    }

    HeapFree(GetProcessHeap(), 0, buf);
    g_count  = n;
    g_loaded = true;

    Logf("slotmap: 已加载 %d 条映射（跳过 %d 行，头部 %d 行）: %s",
         n, skipped, header, path);
    if (n == 0) Logf("slotmap: 警告: 映射表为空，中文将无法显示");
    return n > 0;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
bool Lookup(uint16_t gbk, uint8_t* outSlot) {
    if (!g_loaded || !g_fast) return false;
    uint8_t s = g_fast[gbk];
    if (!s) return false;
    if (outSlot) *outSlot = s;
    return true;
}

int Count() { return g_count; }
bool IsLoaded() { return g_loaded; }

} // namespace slotmap
