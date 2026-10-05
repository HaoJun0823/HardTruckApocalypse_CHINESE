// dllmain.cpp —— ASI 插件入口
//
// 由 Ultimate-ASI-Loader (winmm.dll) 加载。加载时机在游戏主模块映射之后、
// 主逻辑之前，因此此时可以安全地做特征码扫描与挂钩。
#include "pch.h"
#include "plugin.h"

namespace {

HMODULE g_self = nullptr;
bool    g_installed = false;

DWORD WINAPI InitThread(LPVOID) {
    // 在独立线程里初始化，避免在 DllMain 的 loader lock 内做重活
    // （VirtualProtect / 分配 / 文件 IO 在 loader lock 下都可能死锁）
    __try {
        Logf("=== hta_chs 插件启动 ===");

        HMODULE game = GetModuleHandleA(NULL);
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(game, exePath, sizeof(exePath));
        Logf("宿主进程 exe: %s", exePath);
        Logf("主模块基址: 0x%08X", (unsigned)(uintptr_t)game);

        // 0) 转码层自检：槽位字节必须避开绘制路径的转义/控制字节。
        //    这一步能在启动时就暴露取值区间错误，而不是等实机花屏。
        {
            char err[256] = {0};
            if (transcode::SelfTest(err, sizeof(err))) {
                Logf("transcode: 自检通过（转义 0x%02X，槽位 %d 个，区间 0x80..0xFF）",
                     (unsigned)transcode::EscapeByte(), transcode::SlotsPerFont());
            } else {
                Logf("transcode: 自检失败 -> %s", err);
            }
        }

        // 1) 定位字体引擎函数
        int n = font::InstallHooks(game);
        if (n > 0) {
            g_installed = true;
            Logf("字库挂钩已安装 (%d 个目标)", n);
        } else {
            Logf("警告: 未定位到任何字体函数，插件不生效");
        }

        // 2) 定位渲染器原语
        int r = render::Locate(game);
        Logf("渲染器原语定位: %d/2", r);

        // 3) 加载槽位映射表（必须与离线烘的字库配套）
        {
            char selfDir[MAX_PATH] = {0};
            HMODULE self = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&InitThread, &self);
            GetModuleFileNameA(self, selfDir, sizeof(selfDir));
            char* sl = strrchr(selfDir, '\\');
            if (sl) *sl = 0;

            char gameDir[MAX_PATH] = {0};
            GetModuleFileNameA(game, gameDir, sizeof(gameDir));
            char* gl = strrchr(gameDir, '\\');
            if (gl) *gl = 0;

            const char* names[] = { "hta_chs_slotmap.txt", "slotmap.txt" };
            bool ok = false;
            char path[MAX_PATH];
            for (int i = 0; i < 2 && !ok; ++i) {
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", gameDir, names[i]);
                ok = slotmap::Load(path);
                if (!ok) {
                    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", selfDir, names[i]);
                    ok = slotmap::Load(path);
                }
            }
            Logf("slotmap: 加载%s，共 %d 条映射",
                 ok ? "成功" : "失败（中文将显示为 ?）", slotmap::Count());
        }

        // 4) 安装文本转码挂钩
        texthook::Install(game);

        Logf("=== 初始化完成 ===");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("初始化异常: 0x%08X", (unsigned)GetExceptionCode());
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        LogOpen(hModule);
        // 起线程做后续初始化
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
            if (h) CloseHandle(h);
            else Logf("CreateThread 失败 (%lu)", GetLastError());
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_installed) {
            hook::UninstallAll();
            g_installed = false;
        }
        Logf("=== 插件卸载 ===");
        LogClose();
        break;
    }
    return TRUE;
}
