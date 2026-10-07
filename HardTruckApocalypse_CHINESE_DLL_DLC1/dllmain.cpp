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
        Logf("=== hta_chs_dlc1 插件启动（目标 Meridian113.exe）===");

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

        // 3) 选择方案：路径 D（16 位索引）优先；没有包文件则回退路径 C（单字节槽位）
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

            // ── 路径 D：汉字包文件 ──
            // 放在游戏根目录、update\ 或 asi 同目录都能找到。
            bool pathD = false;
            char pkg[MAX_PATH];
            const char* dirs[3] = { gameDir, selfDir, nullptr };
            _snprintf_s(pkg, sizeof(pkg), _TRUNCATE, "%s\\update", gameDir);
            dirs[2] = pkg;
            for (int i = 0; i < 3 && !pathD; ++i) {
                if (!dirs[i]) continue;
                _snprintf_s(pkg, sizeof(pkg), _TRUNCATE,
                            "%s\\hta_chs_cjk_dlc1.bin", dirs[i]);
                pathD = pathd::Init(game, pkg);
            }

            if (pathD) {
                Logf("=== 使用路径 D（16 位字形索引，汉字上限 65536）===");
                Logf("=== 注意：路径 D 下**不做**字符串转码，GBK 原样交给引擎 ===");
            } else {
                Logf("=== 使用路径 C（单字节槽位，汉字上限约 72/字号）===");
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
                    if (!ok) {
                        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\update\\%s", gameDir, names[i]);
                        ok = slotmap::Load(path);
                    }
                }
                Logf("slotmap: 加载%s，共 %d 条映射",
                     ok ? "成功" : "失败（中文将显示为 ?）", slotmap::Count());
                // 只有路径 C 才需要转码
                texthook::Install(game);
            }
        }

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
        pathd::StopRescan();  // 先叫停补装配线程，再拆钩子（只置标志，不等待）
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
