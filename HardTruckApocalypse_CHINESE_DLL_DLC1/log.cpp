// log.cpp —— 插件日志
#include "pch.h"
#include "plugin.h"
#include <algorithm>
#include <vector>

namespace {

HANDLE  g_file    = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_cs;
bool    g_csInit  = false;
char    g_dir[MAX_PATH] = {0};

void Lock()   { if (g_csInit) EnterCriticalSection(&g_cs); }
void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }

// 取插件自身所在目录（也是日志与数据文件的落点）
bool ResolvePluginDir(HMODULE self, char* out, size_t outLen) {
    if (!GetModuleFileNameA(self, out, (DWORD)outLen)) return false;
    char* slash = strrchr(out, '\\');
    if (!slash) return false;
    *slash = '\0';
    return true;
}

// ★ 轮转日志：保留最近 kKeepLogs 份，删掉更旧的 ★
//
//   为什么需要轮转：
//   日志文件按时间戳命名（见 LogOpen），每次启动一个新文件。
//   不清理的话 update\ 目录会无限堆 .log，迟早磁盘满 —— 而磁盘满会连带
//   影响游戏自己的资源加载。
//
//   匹配规则："hta_chs." 打头、".log" 结尾、中间是 15 位时间戳。
//   只删严格符合这个名字的，绝不碰同目录下的其它文件（用户的备份等）。
void RotateLogs(const char* dir) {
    const int kKeepLogs = 10;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(dir, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    // 收集（时间戳, 完整路径）
    struct Entry { char path[MAX_PATH]; char stamp[16]; };
    std::vector<Entry> found;
    do {
        const char* name = fd.cFileName;
        size_t n = strlen(name);
        // 前缀 "hta_chs_dlc1." = 13 字符，后缀 ".log" = 4 字符 → 时间戳 15 字符
        if (n != 13 + 15 + 4) continue;
        if (memcmp(name, "hta_chs_dlc1.", 13) != 0) continue;
        if (memcmp(name + n - 4, ".log", 4) != 0) continue;
        Entry e;
        memcpy(e.stamp, name + 13, 15);
        e.stamp[15] = '\0';
        _snprintf_s(e.path, sizeof(e.path), _TRUNCATE, "%s\\%s", dir, name);
        found.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if ((int)found.size() <= kKeepLogs) return;

    // 时间戳是 "YYYYMMDD_HHMMSS"，字典序 == 时间序，直接按字符串排
    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b) { return strcmp(a.stamp, b.stamp) < 0; });

    size_t toRemove = found.size() - kKeepLogs;
    for (size_t i = 0; i < toRemove; ++i) {
        // 不打日志：此刻日志文件还没打开（LogOpen 正在创建它），
        // 而且轮转是正常维护行为，不需要留下痕迹。
        DeleteFileA(found[i].path);
    }
}

} // namespace

void LogOpen(HMODULE self) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    if (g_file != INVALID_HANDLE_VALUE) return;

    if (!ResolvePluginDir(self, g_dir, sizeof(g_dir))) {
        lstrcpynA(g_dir, ".", sizeof(g_dir));
    }

    char path[MAX_PATH];
    // 日志与游戏 exe / 加载器 DLL 同目录，便于排查
    //
    // ★ 文件名带时间戳，不再覆盖上一次 ★
    //   原来固定写 hta_chs.log + CREATE_ALWAYS，每次启动都把上一次的抹掉。
    //   调试"随机问题"时这等于把证据全毁了 —— 07:24 那次就是因为旧日志
    //   还在，才能看出"装配和 LoadPackage 在赛跑、15.625 被漏掉"。
    //   随机 bug 往往要跑十几次才复现，覆盖式日志会让你永远抓不到。
    char dirWithSlash[MAX_PATH];
    _snprintf_s(dirWithSlash, sizeof(dirWithSlash), _TRUNCATE, "%s\\", g_dir);
    RotateLogs(dirWithSlash);

    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%shta_chs_dlc1.%04d%02d%02d_%02d%02d%02d.log",
                dirWithSlash,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                         NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) {
        // 回退到系统临时目录（同样带时间戳）
        char tmp[MAX_PATH];
        if (GetTempPathA(sizeof(tmp), tmp)) {
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%shta_chs_dlc1.%04d%02d%02d_%02d%02d%02d.log",
                        tmp, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            lstrcpynA(g_dir, tmp, sizeof(g_dir));
            g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
}

void LogClose() {
    Lock();
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    Unlock();
}

void Logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2200];
    int m = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[%02d:%02d:%02d.%03d] %s\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (m < 0) return;

    Lock();
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, (DWORD)m, &written, NULL);
        FlushFileBuffers(g_file);
    }
    Unlock();

    // 不再调用 OutputDebugStringA：
    //   1) 它在 x64dbg 里会被「Break on debug strings」拦下，调试时每写一行日志
    //      就中断一次，完全没法用（ASI 加载器 winmm.dll 自己也会大量输出，
    //      那个只能靠调试器设置关掉，但我们至少不该再火上浇油）；
    //   2) Logf 在热路径上会被频繁调用，走一次调试输出很拖速度。
}
