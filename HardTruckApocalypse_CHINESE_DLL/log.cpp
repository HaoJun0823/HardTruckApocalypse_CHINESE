// log.cpp —— 插件日志
#include "pch.h"
#include "plugin.h"

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
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\hta_chs.log", g_dir);

    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                         NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) {
        // 回退到系统临时目录
        char tmp[MAX_PATH];
        if (GetTempPathA(sizeof(tmp), tmp)) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%shta_chs.log", tmp);
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
