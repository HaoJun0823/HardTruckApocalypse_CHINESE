// util.cpp —— 日志、模块/段查询、可读性缓存、INI 配置
#include "r9fix_dlc2.h"

// ===========================================================================
// 日志
// ===========================================================================
namespace {

FILE*             g_log = nullptr;
CRITICAL_SECTION  g_logCs;
bool              g_logReady = false;

void DirOf(HMODULE mod, char* out, size_t cap) {
    out[0] = 0;
    if (!GetModuleFileNameA(mod, out, (DWORD)cap)) { out[0] = 0; return; }
    char* sl = strrchr(out, '\\');
    if (sl) *sl = 0;
}

}  // namespace

void LogOpen(HMODULE self) {
    InitializeCriticalSection(&g_logCs);
    g_logReady = true;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char dir[MAX_PATH] = {0};
    char path[MAX_PATH] = {0};
    DirOf(self, dir, sizeof(dir));

    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.%04d%02d%02d_%02d%02d%02d.log", dir, DLC2MF_NAME,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_log = fopen(path, "wb");

    if (!g_log) {
        char exe[MAX_PATH] = {0};
        DirOf(NULL, exe, sizeof(exe));
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.%04d%02d%02d_%02d%02d%02d.log", exe, DLC2MF_NAME,
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_log = fopen(path, "wb");
    }

    Logf("================ %s ================", DLC2MF_NAME);
    Logf("日志: %s", g_log ? path : "(打不开，仅内存)");
}

void LogClose() {
    if (g_log) {
        Logf("================ %s 结束 ================", DLC2MF_NAME);
        fclose(g_log);
        g_log = nullptr;
    }
}

void Logf(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logCs);
}

// ===========================================================================
// 模块信息
// ===========================================================================
bool GetModInfo(HMODULE mod, ModInfo* out) {
    if (!out) return false;
    out->base = 0;
    out->size = 0;
    out->valid = false;
    if (!mod) return false;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    out->base  = (uintptr_t)mod;
    out->size  = nt->OptionalHeader.SizeOfImage;
    out->valid = true;
    return true;
}

bool GetModInfoByName(const char* name, ModInfo* out) {
    HMODULE m = GetModuleHandleA(name);
    if (!m) return false;
    return GetModInfo(m, out);
}

bool InRange(uintptr_t a, uintptr_t base, size_t size) {
    if (!base || !size) return false;
    return a >= base && a < base + size;
}

// 段属性查询：直接解析 PE 段表
namespace {

struct SecDesc {
    uintptr_t va;
    size_t    vsize;
    uint32_t  chars;
};

int SectionsOf(const ModInfo& m, SecDesc* out, int cap) {
    if (!m.valid) return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m.base;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const uint8_t*)m.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int i = 0; i < (int)nt->FileHeader.NumberOfSections && n < cap; ++i, ++sec) {
        out[n].va    = m.base + sec->VirtualAddress;
        out[n].vsize = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        out[n].chars = sec->Characteristics;
        ++n;
    }
    return n;
}

bool AddrInSection(const ModInfo& m, uintptr_t a, uint32_t require, uint32_t forbid) {
    SecDesc s[32];
    int n = SectionsOf(m, s, 32);
    for (int i = 0; i < n; ++i) {
        if (!InRange(a, s[i].va, s[i].vsize)) continue;
        if ((s[i].chars & require) != require) return false;
        if (s[i].chars & forbid) return false;
        return true;
    }
    return false;
}

}  // namespace

bool AddrInExec(const ModInfo& m, uintptr_t a) {
    return AddrInSection(m, a, IMAGE_SCN_MEM_EXECUTE, 0);
}

bool AddrInRoData(const ModInfo& m, uintptr_t a) {
    return AddrInSection(m, a, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE);
}

bool AddrInWritable(const ModInfo& m, uintptr_t a) {
    return AddrInSection(m, a, IMAGE_SCN_MEM_WRITE, 0);
}

// ===========================================================================
// 可读性
// ===========================================================================
namespace {

bool RegionReadable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    if (mbi.Protect & PAGE_NOACCESS) return false;
    switch (mbi.Protect & 0xFF) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool IsReadable(const void* p, size_t len) {
    uintptr_t a = (uintptr_t)p;
    uintptr_t end = a + len;
    if (end < a) return false;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) return false;
        if (!RegionReadable(mbi)) return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= a) return false;
        a = regionEnd;
    }
    return true;
}

namespace {

struct Range {
    uintptr_t lo;
    uintptr_t hi;  // 不含
};

Range   g_ranges[1024];
int     g_rangeCount = 0;
bool    g_rangeReady = false;

}  // namespace

void BuildReadableMap() {
    g_rangeCount = 0;
    g_rangeReady = false;

    uintptr_t a = 0x10000;
    const uintptr_t kEnd = 0xFFF00000u;
    while (a < kEnd && g_rangeCount < (int)(sizeof(g_ranges) / sizeof(g_ranges[0]))) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) break;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= a) break;
        if (RegionReadable(mbi)) {
            g_ranges[g_rangeCount].lo = a;
            g_ranges[g_rangeCount].hi = regionEnd;
            ++g_rangeCount;
        }
        a = regionEnd;
    }
    g_rangeReady = true;
    Logf("可读区索引: %d 段", g_rangeCount);
}

bool ReadableCached(const void* p, size_t len) {
    if (!g_rangeReady) return IsReadable(p, len);
    uintptr_t a   = (uintptr_t)p;
    uintptr_t end = a + len;
    if (end < a) return false;

    int lo = 0, hi = g_rangeCount - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < g_ranges[mid].lo)      hi = mid - 1;
        else if (a >= g_ranges[mid].hi) lo = mid + 1;
        else { found = mid; break; }
    }
    if (found < 0) return IsReadable(p, len);
    return end <= g_ranges[found].hi;
}

// ===========================================================================
// 配置
// ===========================================================================
Config g_cfg = {true, 65536, true, 256, 0x60000000, false, 120};

namespace {

void ResolveIni(HMODULE self, char* out, size_t cap) {
    char dir[MAX_PATH] = {0};
    DirOf(self, dir, sizeof(dir));
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s.ini", dir, DLC2MF_NAME);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return;

    char exe[MAX_PATH] = {0};
    DirOf(NULL, exe, sizeof(exe));
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s.ini", exe, DLC2MF_NAME);
}

uint32_t ClampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

void LoadConfig(HMODULE self) {
    char ini[MAX_PATH] = {0};
    ResolveIni(self, ini, sizeof(ini));

    bool exists = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    Logf("配置来源: %s%s", exists ? ini : "(无 ini，用默认值)", exists ? "" : ini);

    g_cfg.enabled      = GetPrivateProfileIntA(DLC2MF_NAME, "Enabled", 1, ini) != 0;
    g_cfg.sizeMax      = (uint32_t)GetPrivateProfileIntA(DLC2MF_NAME, "SizeMax", 65536, ini);
    g_cfg.onlyHighAddr = GetPrivateProfileIntA(DLC2MF_NAME, "OnlyHighAddr", 1, ini) != 0;
    g_cfg.arenaMb      = (uint32_t)GetPrivateProfileIntA(DLC2MF_NAME, "ArenaMB", 256, ini);
    g_cfg.logAlloc     = GetPrivateProfileIntA(DLC2MF_NAME, "LogAlloc", 0, ini) != 0;
    g_cfg.waitSec      = (uint32_t)GetPrivateProfileIntA(DLC2MF_NAME, "WaitSec", 120, ini);

    char buf[32] = {0};
    GetPrivateProfileStringA(DLC2MF_NAME, "ArenaBase", "60000000", buf, sizeof(buf), ini);
    g_cfg.arenaBase = (uint32_t)strtoul(buf, nullptr, 16);

    g_cfg.sizeMax  = ClampU32(g_cfg.sizeMax, 16, 65536);
    g_cfg.arenaMb  = ClampU32(g_cfg.arenaMb, 1, 512);
    g_cfg.waitSec  = ClampU32(g_cfg.waitSec, 1, 600);
    if (g_cfg.arenaBase >= 0x80000000u || g_cfg.arenaBase < 0x10000u) g_cfg.arenaBase = 0x60000000u;
    g_cfg.arenaBase &= ~0xFFFFu;

    Logf("配置: Enabled=%d SizeMax=%u OnlyHighAddr=%d ArenaMB=%u ArenaBase=0x%08X LogAlloc=%d WaitSec=%u",
         (int)g_cfg.enabled, g_cfg.sizeMax, (int)g_cfg.onlyHighAddr, g_cfg.arenaMb, g_cfg.arenaBase,
         (int)g_cfg.logAlloc, g_cfg.waitSec);
}
