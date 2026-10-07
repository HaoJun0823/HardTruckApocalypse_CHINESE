// Render9Fix.cpp —— ASI 插件入口
//
// 由 Ultimate-ASI-Loader (winmm.dll) 加载。本插件**不挂钩任何代码**，只：
//   1. 抢一段 2GB 以下的地址空间，建低地址分配池
//   2. 等 dxrender9.dll 出现、引擎 Kernel 对象就绪
//   3. 把 Kernel+0x18/0x1C/0x20 的 g_mar.AllocMem/ReallocMem/FreeMem 换成我们的
//
// 详细原理与安全设计见 r9fix.h 顶部注释。
#include "r9fix.h"

namespace {

HMODULE g_self = nullptr;

DWORD WINAPI InitThread(LPVOID) {
    __try {
        LogOpen(g_self);

        // 单实例防护：万一 asi 被加载了两份（例如游戏根目录与 update\ 各一份），
        // 第二份会把我们的钩子当成「原函数」再包一层 → 无限递归。用命名互斥体挡掉。
        HANDLE once = CreateMutexA(NULL, FALSE, "Render9Fix_SingleInstance_v1");
        if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
            Logf("本进程已有 Render9Fix 实例在运行 —— 本份直接退出");
            return 0;
        }

        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        Logf("宿主 exe: %s", exe);
        Logf("本插件: %s = 把 dxrender9 的小对象分配导向 2GB 以下", R9FIX_NAME);

        LoadConfig(g_self);

        if (!g_cfg.enabled) {
            Logf("Enabled=0 —— 直接退出，什么都不做");
            return 0;
        }

        // 尽早抢地址空间：越早，2GB 以下越空，越容易拿到一个漂亮的连续块
        if (!lowheap::Init(g_cfg.arenaBase, g_cfg.arenaMb * 1024u * 1024u)) {
            Logf("低地址池建不起来 —— 不安装（游戏不受影响）");
            return 0;
        }
        lowheap::SelfTest();

        if (!memhook::InstallWhenReady(g_cfg.waitSec * 1000u)) {
            Logf("挂钩未安装 —— 游戏行为与安装前完全一致");
            return 0;
        }

        Logf("挂钩已生效。之后每 5 秒输出一次统计（只在有变化时）。");
        Logf("判读方法：");
        Logf("  * 「导向」条数应随游戏加载资源增长；");
        Logf("  * 「高地址遗留」= dxrender9 发起但没被导向、且落在 >=2GB 的分配");
        Logf("    —— 若这个数字很大，把 ini 里的 SizeMax 调大再看；");
        Logf("  * 若进关卡不再崩在 dxrender9+0x416D6，即为修复成功。");

        for (;;) {
            Sleep(5000);
            memhook::LogSummary();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("初始化异常: 0x%08X —— 插件已停止工作，游戏不受影响", GetExceptionCode());
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);

        // 在独立线程里初始化：DllMain 的 loader lock 下不能做文件 IO / 分配
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }

    // 注意：DLL_PROCESS_DETACH 时**故意不还原**那三个函数指针。
    // 原因：还原之后引擎会用自己的 FreeMem 去释放我们已经发出去的块，
    // 而它会读到我们的块头并因魔数不符而 __debugbreak（= int3 崩溃）。
    // ASI 插件在进程退出前不会被卸载，保持钩子是最安全的做法。

    return TRUE;
}
