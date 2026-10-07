# null_dbg.ps1 —— 无断点空调试器（模拟"仅挂载 x32dbg"）
#
# 目的：判定 LAA 下 dxrender9 崩溃（0x9D880）为什么"x32dbg 挂载即不崩"。
# 本脚本用 Win32 Debug API 挂载 hta.exe，事件循环里只做 Continue：
#   * 不下任何断点、不改任何内存、不读异常内存
#   * 异常一律透传给游戏自己的 SEH（DBG_EXCEPTION_NOT_HANDLED），行为等同无调试器
#   * 仅消费进程启动必需的初始断点（0x80000003/0x80000004）
#
# 三档模式（A/B 判定挂载生效的机制）：
#   Fast（默认）：事件只计数、2 秒批量落盘 —— 挂载开销最小
#   Slow        ：每个事件立刻落盘 + 读取 ODS 字符串 —— 模拟 x32dbg 事件处理开销
#   -DelayMs N  ：每个事件在 Continue 之前额外挂起 N ms —— 调试器悬停游戏线程的纯时延注入
#                 （注意：事件 pending 期间游戏全部线程是被系统挂起的，所以放在 Continue 前才有效）
#
# 用法（在 64 位 pwsh/PowerShell 里直接跑，脚本会自动重启动到 32 位 PowerShell）：
#   .\tools\null_dbg.ps1                          # Spawn + Fast：直接启动游戏（推荐首跑）
#   .\tools\null_dbg.ps1 -Quality Slow            # Spawn + Slow：模拟 x32dbg 事件开销
#   .\tools\null_dbg.ps1 -DelayMs 5               # 每事件挂起 5ms
#   .\tools\null_dbg.ps1 -Mode Attach -TargetPid 1234   # 附加到已在运行的游戏
#   .\tools\null_dbg.ps1 -DetachAfterSec 20       # 20 秒后自动分离（区分"存在"与"持续存在"）
#
# 判读：
#   A. Fast 就不崩        → 仅 debug port 存在即修复 → 产品化方向：启动器以 DEBUG_ONLY_THIS_PROCESS 启动
#   B. Fast 崩、Slow 不崩 → 事件处理时序开销起作用 → 梯度找出生效的 DelayMs，在 ASI/启动器里模拟该延迟
#   C. Fast/Slow 都崩     → 纯挂载不够 → 回到 x32dbg 二分（它还做了什么别的）
#
# 前置：先关掉 x32dbg 和已运行的 hta.exe（同一进程不能被两个调试器挂）
# 日志：exceptions\nulldbg_<时间戳>.log（与游戏崩溃日志同目录）
#
# 自动取证：first-chance AV 落在 dxrender9 内时，放行前抓取（日志 [FX] 行）：
#   * effect 文件名（EffectImpl+0x0C 的 CStr）、technique 计数/数组指针/默认索引
#   * 全线程 EIP 快照 + dxrender9 内线程的栈返回地址（找并发方）
#   * 抓完放行，游戏照常走它自己的崩溃流程
#   注意：取证要用不加延迟的档（-Quality Fast）复现崩溃；加了 DelayMs 4 不崩就没有 [FX]。

param(
    [ValidateSet('Spawn', 'Attach')][string]$Mode = 'Spawn',
    [uint32]$TargetPid = 0,
    [ValidateSet('Fast', 'Slow')][string]$Quality = 'Fast',
    [int]$DelayMs = 0,
    [int]$DetachAfterSec = 0,
    [string]$GameExe = 'I:\LocalGames\Hard Truck Apocalypse STEAM\hta.exe'
)

# ---- 必须 32 位运行（游戏是 32 位，DEBUG_EVENT 结构按 x86 布局解析）----
if ([IntPtr]::Size -ne 4) {
    $syswow = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path $syswow)) { Write-Host "FATAL: 找不到 32 位 PowerShell: $syswow"; exit 3 }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
              '-Mode', $Mode, '-TargetPid', $TargetPid, '-Quality', $Quality,
              '-DelayMs', $DelayMs, '-DetachAfterSec', $DetachAfterSec, '-GameExe', $GameExe)
    & $syswow @args
    exit $LASTEXITCODE
}

# ---- 环境检查 ----
if (-not (Test-Path -LiteralPath $GameExe)) { Write-Host "FATAL: 找不到游戏: $GameExe"; exit 3 }
if (Get-Process -Name hta -ErrorAction SilentlyContinue) {
    Write-Host 'FATAL: 已有 hta.exe 在运行（且可能被 x32dbg 挂着）。先关闭它和 x32dbg。'
    exit 3
}
if ($Mode -eq 'Attach' -and $TargetPid -eq 0) {
    Write-Host 'FATAL: Attach 模式需要 -TargetPid'; exit 3
}
$gameDir = Split-Path -Parent $GameExe
$logDir = Join-Path $gameDir 'exceptions'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$logPath = Join-Path $logDir "nulldbg_$stamp.log"

# ---- Win32 调试 API（C#5 语法，兼容 PowerShell 5.1 内置编译器）----
$cs = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class NDbg
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFOW
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string appName, StringBuilder cmdLine,
        IntPtr procAttr, IntPtr threadAttr, bool inherit, uint flags,
        IntPtr env, string curDir, ref STARTUPINFOW si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DebugActiveProcess(uint pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DebugActiveProcessStop(uint pid);

    [DllImport("kernel32.dll")]
    static extern bool DebugSetProcessKillOnExit(bool kill);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WaitForDebugEvent(IntPtr evt, uint ms);

    [DllImport("kernel32.dll")]
    static extern bool ContinueDebugEvent(uint pid, uint tid, uint status);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    // ---- 取证用：模块名识别 + 全线程快照 ----
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetFinalPathNameByHandleW(IntPtr hFile, StringBuilder path, uint cch, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint th32ProcessID);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool Thread32First(IntPtr snap, ref THREADENTRY32 te);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool Thread32Next(IntPtr snap, ref THREADENTRY32 te);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenThread(uint access, bool inherit, uint tid);

    [DllImport("kernel32.dll")]
    static extern uint SuspendThread(IntPtr h);

    [DllImport("kernel32.dll")]
    static extern uint ResumeThread(IntPtr h);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetThreadContext(IntPtr h, byte[] ctx);

    [StructLayout(LayoutKind.Sequential)]
    struct THREADENTRY32
    {
        public uint dwSize, cntUsage, th32ThreadID, th32OwnerProcessId;
        public int tpBasePri, tpDeltaPri;
        public uint dwFlags;
    }

    const uint DEBUG_ONLY_THIS_PROCESS = 0x2;
    const uint DBG_CONTINUE = 0x00010002;
    const uint DBG_EXCEPTION_NOT_HANDLED = 0x80010001;

    // DEBUG_EVENT codes (x86 layout: code@0 pid@4 tid@8 union@12)
    const uint EV_EXCEPTION = 1, EV_CREATE_THREAD = 2, EV_CREATE_PROCESS = 3,
               EV_EXIT_THREAD = 4, EV_EXIT_PROCESS = 5, EV_LOAD_DLL = 6,
               EV_UNLOAD_DLL = 7, EV_ODS = 8, EV_RIP = 9;

    static StreamWriter _w;
    static StringBuilder _acc = new StringBuilder();
    static DateTime _start;
    static DateTime _lastFlush;
    static string _quality;
    static uint DxBase;                       // dxrender9.dll 运行时基址（LOAD_DLL 时按文件名识别）
    const uint DxSize = 0x1B8000;             // dxrender9 PE SizeOfImage
    const uint HtaBase = 0x400000;            // hta.exe 基址
    const uint HtaSize = 0x700000;

    static void Log(string line)
    {
        _acc.Append('[').Append((DateTime.Now - _start).ToString("hh\\:mm\\:ss\\.fff"))
            .Append("] ").Append(line).Append("\r\n");
    }

    static void FlushIfDue(bool force)
    {
        if (_acc.Length == 0) return;
        if (force || (DateTime.Now - _lastFlush).TotalSeconds >= 2 || _acc.Length > 65536)
        {
            _w.Write(_acc.ToString());
            _acc.Length = 0;
            _w.Flush();
            _lastFlush = DateTime.Now;
        }
    }

    static string ReadOds(IntPtr hProc, uint addr, ushort len, bool unicode)
    {
        if (hProc == IntPtr.Zero) return "(no hProc) len=" + len;
        try
        {
            int bytes = unicode ? len * 2 : len;
            if (bytes > 4096) bytes = 4096;
            byte[] buf = new byte[bytes + 2];
            int got;
            if (!ReadProcessMemory(hProc, P(addr), buf, bytes, out got)) return "(ReadProcessMemory failed)";
            return unicode ? Encoding.Unicode.GetString(buf, 0, got) : Encoding.Default.GetString(buf, 0, got);
        }
        catch (Exception e) { return "(read error: " + e.Message + ")"; }
    }

    // ================= 崩溃现场取证（仅在 AV 落在 dxrender9 内时触发） =================
    // uint(可能 ≥2GB，LAA 场景) → IntPtr 安全转换：
    // 直接 (IntPtr)addr 会走 IntPtr(long) 构造，32 位下 >0x7FFFFFFF 抛 OverflowException；
    // 走 (IntPtr)(int) 构造无此问题（负 int 表示高地址，位模式不变）。
    static IntPtr P(uint a) { return new IntPtr(unchecked((int)a)); }

    static string ReadStr(IntPtr hp, uint addr)
    {
        if (addr == 0 || addr == 0xFFFFFFFF) return "(null)";
        try
        {
            byte[] b = new byte[260];
            int got;
            if (!ReadProcessMemory(hp, P(addr), b, 259, out got) || got <= 0) return "(unreadable 0x" + addr.ToString("X8") + ")";
            int n = 0;
            while (n < got && b[n] != 0) n++;
            return Encoding.Default.GetString(b, 0, n);
        }
        catch (Exception e) { return "(err " + e.Message + ")"; }
    }

    static uint ReadU32(IntPtr hp, uint addr)
    {
        byte[] b = new byte[4];
        int got;
        if (!ReadProcessMemory(hp, P(addr), b, 4, out got) || got < 4) return 0xFFFFFFFF;
        return BitConverter.ToUInt32(b, 0);
    }

    static bool GetThreadCtx(uint t, out uint eip, out uint esp, out uint esi)
    {
        eip = 0; esp = 0; esi = 0;
        IntPtr h = OpenThread(0x0002 | 0x0008 | 0x0040, false, t);  // SUSPEND_RESUME|GET_CONTEXT|QUERY_INFO
        if (h == IntPtr.Zero) return false;
        try
        {
            if (SuspendThread(h) == 0xFFFFFFFF) return false;
            byte[] ctx = new byte[0x2CC];                            // x86 CONTEXT
            BitConverter.GetBytes(0x0001000B).CopyTo(ctx, 0);        // CONTEXT_i386|FULL
            bool ok = GetThreadContext(h, ctx);
            ResumeThread(h);
            if (!ok) return false;
            esi = BitConverter.ToUInt32(ctx, 0xA0);                   // 结构偏移：Edi@9C Esi@A0 ... Eip@B8 Esp@C4
            eip = BitConverter.ToUInt32(ctx, 0xB8);
            esp = BitConverter.ToUInt32(ctx, 0xC4);
            return true;
        }
        finally { CloseHandle(h); }
    }

    static void RunForensics(IntPtr hp, uint pid, uint tid, uint excAddr)
    {
        Log("[FX] ===== forensics: AV at dxrender9+0x" + (excAddr - DxBase).ToString("X6") +
            " tid=" + tid + " =====");

        // 1) 崩溃线程现场
        uint eip = 0, esp = 0, esi = 0;
        GetThreadCtx(tid, out eip, out esp, out esi);
        Log("[FX] crash thread: EIP=0x" + eip.ToString("X8") + " ESP=0x" + esp.ToString("X8") +
            " ESI=0x" + esi.ToString("X8"));

        // 2) ValidateEffect 现场（RVA 0x41140-0x41708 内时，读 EffectImpl 状态）
        if (eip >= DxBase + 0x41140 && eip < DxBase + 0x41708 && esi > 0x10000)
        {
            uint namePtr = ReadU32(hp, esi + 0x0C);      // CStr { char* m_charPtr@0 } → 文件名字指针
            uint count = ReadU32(hp, esi + 0x9F0);       // technique 计数
            uint arr = ReadU32(hp, esi + 0x9FC);         // m_techDescs 指针
            uint dflt = ReadU32(hp, esi + 0xA08);        // 默认 technique 索引（-1=没找到）
            Log("[FX] effect file   = '" + ReadStr(hp, namePtr) + "'");
            Log("[FX] techCount     = " + count);
            Log("[FX] techArr       = 0x" + arr.ToString("X8"));
            Log("[FX] defaultIdx    = " + (dflt == 0xFFFFFFFF ? "-1 (无默认)" : dflt.ToString()));
            if (count != 0 && count < 1000 && arr > 0x10000 && arr != 0xFFFFFFFF)
            {
                Log("[FX] techArr[0]   = 0x" + ReadU32(hp, arr).ToString("X8"));
            }
        }

        // 3) 全线程快照：谁在 dxrender9 里？谁在 hta.exe 里？
        IntPtr snap = CreateToolhelp32Snapshot(4 /*TH32CS_SNAPTHREAD*/, 0);
        if (snap != (IntPtr)(-1))
        {
            THREADENTRY32 te = new THREADENTRY32();
            te.dwSize = 28;
            int nAll = 0, nDx = 0;
            if (Thread32First(snap, ref te))
            {
                do
                {
                    if (te.th32OwnerProcessId != pid) continue;
                    uint t = te.th32ThreadID;
                    uint teip, tesp, tesi;
                    if (!GetThreadCtx(t, out teip, out tesp, out tesi)) continue;
                    nAll++;
                    string cls;
                    bool inDx = teip >= DxBase && teip < DxBase + DxSize;
                    if (inDx)
                    {
                        cls = "dxrender9+0x" + (teip - DxBase).ToString("X6");
                        nDx++;
                    }
                    else if (teip >= HtaBase && teip < HtaBase + HtaSize)
                        cls = "hta+0x" + (teip - HtaBase).ToString("X6");
                    else
                        cls = "0x" + teip.ToString("X8");
                    Log("[FX] thread tid=" + t + " EIP=" + cls + " ESP=0x" + tesp.ToString("X8") +
                        " ESI=0x" + tesi.ToString("X8"));

                    // 停在 dxrender9 内的线程：扫栈上的 dxrender9 返回地址（调用链线索）
                    if (inDx && tesp > 0x10000)
                    {
                        byte[] stk = new byte[0x400];
                        int got;
                        if (ReadProcessMemory(hp, P(tesp), stk, 0x400, out got))
                        {
                            int sc = 0;
                            for (int i = 0; i + 4 <= got && sc < 8; i += 4)
                            {
                                uint v = BitConverter.ToUInt32(stk, i);
                                if (v >= DxBase && v < DxBase + DxSize)
                                {
                                    Log("[FX]   tid=" + t + " stack -> dxrender9+0x" + (v - DxBase).ToString("X6"));
                                    sc++;
                                }
                            }
                        }
                    }
                } while (Thread32Next(snap, ref te));
            }
            CloseHandle(snap);
            Log("[FX] threads=" + nAll + "  inDxRender9=" + nDx);
        }
        FlushIfDue(true);
    }

    static void Detach(uint pid, bool quiet)
    {
        if (DebugActiveProcessStop(pid))
        {
            if (!quiet) Console.WriteLine("detached pid=" + pid);
            Log("detached pid=" + pid);
        }
        else
        {
            int err = Marshal.GetLastWin32Error();
            Log("detach FAILED err=" + err);
            if (!quiet) Console.WriteLine("detach FAILED err=" + err);
        }
        FlushIfDue(true);
    }

    public static int Run(string exePath, string workDir, string mode, uint targetPid,
                           string quality, int delayMs, int detachAfterSec, string logPath)
    {
        IntPtr evtBuf = IntPtr.Zero;
        try
        {
            DebugSetProcessKillOnExit(false);   // 脚本退出绝不杀游戏
            _start = DateTime.Now;
            _lastFlush = _start;
            _quality = quality;
            _w = new StreamWriter(new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.Read),
                                   Encoding.UTF8);
            _w.AutoFlush = false;
            Log("null_dbg start mode=" + mode + " quality=" + quality +
                " delayMs=" + delayMs + " detachAfterSec=" + detachAfterSec + " exe=" + exePath);
            Console.WriteLine("null_dbg: mode=" + mode + " quality=" + quality +
                              " delayMs=" + delayMs + "  log=" + logPath);

            evtBuf = Marshal.AllocHGlobal(1024);
            uint mainPid = targetPid;
            IntPtr hProc = IntPtr.Zero;

            if (mode == "Attach")
            {
                if (!DebugActiveProcess(targetPid))
                {
                    int err = Marshal.GetLastWin32Error();
                    Console.WriteLine("FATAL: DebugActiveProcess(" + targetPid + ") err=" + err +
                                      "（进程可能已被别的调试器挂载）");
                    return 2;
                }
                Log("attached pid=" + targetPid);
                Console.WriteLine("attached pid=" + targetPid);
            }
            else
            {
                STARTUPINFOW si = new STARTUPINFOW();
                si.cb = Marshal.SizeOf(typeof(STARTUPINFOW));
                PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
                StringBuilder cmd = new StringBuilder("\"" + exePath + "\"");
                if (!CreateProcess(null, cmd, IntPtr.Zero, IntPtr.Zero, false,
                                   DEBUG_ONLY_THIS_PROCESS, IntPtr.Zero, workDir, ref si, out pi))
                {
                    Console.WriteLine("FATAL: CreateProcess err=" + Marshal.GetLastWin32Error());
                    return 2;
                }
                mainPid = (uint)pi.dwProcessId;
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                Log("spawned pid=" + mainPid);
                Console.WriteLine("spawned pid=" + mainPid);
            }

            long nEvt = 0, nFirstAv = 0, nSecondAv = 0, nOds = 0, nDll = 0, nOtherExc = 0, nBp = 0;
            bool done = false;
            bool detachDue = detachAfterSec > 0;

            while (!done)
            {
                if (!WaitForDebugEvent(evtBuf, 200))
                {
                    // 超时：检查是否到分离时刻（此时没有 pending 事件，可安全分离）
                    if (detachDue && (DateTime.Now - _start).TotalSeconds >= detachAfterSec)
                    { Detach(mainPid, false); return 0; }
                    continue;
                }

                byte[] raw = new byte[1024];
                Marshal.Copy(evtBuf, raw, 0, 1024);
                uint code = BitConverter.ToUInt32(raw, 0);
                uint pid = BitConverter.ToUInt32(raw, 4);
                uint tid = BitConverter.ToUInt32(raw, 8);
                uint cont = DBG_CONTINUE;
                nEvt++;
                string line = null;

                if (code == EV_EXCEPTION)
                {
                    uint exc = BitConverter.ToUInt32(raw, 12);
                    uint excAddr = BitConverter.ToUInt32(raw, 24);
                    uint first = BitConverter.ToUInt32(raw, 92);
                    if (exc == 0x80000003u || exc == 0x80000004u)
                    {
                        // 进程启动的初始断点（调试器必须消费，否则进程死）
                        cont = DBG_CONTINUE;
                        nBp++;
                        line = "INIT-BP consumed tid=" + tid;
                    }
                    else
                    {
                        cont = DBG_EXCEPTION_NOT_HANDLED;   // 透传给游戏 SEH —— 和无调试器完全一致
                        if (exc == 0xC0000005u)
                        {
                            if (first != 0) nFirstAv++; else nSecondAv++;
                            line = "AV " + (first != 0 ? "1st" : "2nd") + " chance addr=0x" +
                                   excAddr.ToString("X8") + " tid=" + tid;
                            // 取证：AV 落在 dxrender9 内 → 抓 effect 状态 + 全线程快照
                            if (first != 0 && DxBase != 0 &&
                                excAddr >= DxBase && excAddr < DxBase + DxSize)
                                RunForensics(hProc, pid, tid, excAddr);
                        }
                        else if (exc != 0xE06D7363u && exc != 0xE0434352u)  // C++/CLR 异常不落盘（量太大=意外减速）
                        {
                            nOtherExc++;
                            line = "EXC 0x" + exc.ToString("X8") + " addr=0x" + excAddr.ToString("X8") +
                                   " " + (first != 0 ? "1st" : "2nd") + " tid=" + tid;
                        }
                    }
                }
                else if (code == EV_CREATE_PROCESS)
                {
                    hProc = (IntPtr)BitConverter.ToInt32(raw, 16);       // hProcess
                    IntPtr hFile = (IntPtr)BitConverter.ToInt32(raw, 12);
                    if (hFile != IntPtr.Zero) CloseHandle(hFile);
                    uint baseAddr = BitConverter.ToUInt32(raw, 24);
                    line = "CREATE_PROCESS pid=" + pid + " base=0x" + baseAddr.ToString("X8");
                }
                else if (code == EV_LOAD_DLL)
                {
                    IntPtr hFile = (IntPtr)BitConverter.ToInt32(raw, 12);
                    uint baseAddr = BitConverter.ToUInt32(raw, 16);
                    if (hFile != IntPtr.Zero)
                    {
                        if (DxBase == 0)
                        {
                            StringBuilder pth = new StringBuilder(520);
                            if (GetFinalPathNameByHandleW(hFile, pth, 520, 0) > 0 &&
                                pth.ToString().EndsWith("dxrender9.dll", StringComparison.OrdinalIgnoreCase))
                            {
                                DxBase = baseAddr;
                                Log("[FX] dxrender9 base = 0x" + DxBase.ToString("X8"));
                            }
                        }
                        CloseHandle(hFile);
                    }
                    nDll++;
                    // Fast 档也记录基址：崩溃地址 - 模块基址 = RVA，用于对齐 IDA
                    line = "LOAD_DLL base=0x" + baseAddr.ToString("X8") + " tid=" + tid;
                }
                else if (code == EV_ODS)
                {
                    nOds++;
                    if (quality == "Slow")
                    {
                        ushort fU = BitConverter.ToUInt16(raw, 16);
                        ushort len = BitConverter.ToUInt16(raw, 18);
                        uint ptr = BitConverter.ToUInt32(raw, 12);
                        line = "ODS: " + ReadOds(hProc, ptr, len, fU != 0)
                                    .Replace("\r", "\\r").Replace("\n", "\\n");
                    }
                }
                else if (code == EV_EXIT_PROCESS)
                {
                    uint exitCode = BitConverter.ToUInt32(raw, 12);
                    line = "EXIT_PROCESS code=" + exitCode;
                    done = true;
                }
                else if (code == EV_UNLOAD_DLL)
                {
                    if (quality == "Slow") line = "UNLOAD_DLL tid=" + tid;
                }
                else if (code == EV_CREATE_THREAD || code == EV_EXIT_THREAD)
                {
                    if (quality == "Slow") line = (code == EV_CREATE_THREAD ? "CREATE_THREAD " : "EXIT_THREAD ") + tid;
                }
                else if (code == EV_RIP)
                {
                    line = "RIP tid=" + tid;
                }

                if (line != null) Log(line);
                if (line != null && (line.StartsWith("AV ") || line.StartsWith("EXC ")))
                    Console.WriteLine(line);

                // 关键顺序：先挂起等待（Sleep 在 Continue 之前 = 调试器悬停游戏线程的时延注入），再 Continue
                if (delayMs > 0) Thread.Sleep(delayMs);
                ContinueDebugEvent(pid, tid, cont);
                FlushIfDue(quality == "Slow" && line != null);

                if (detachDue && !done && (DateTime.Now - _start).TotalSeconds >= detachAfterSec)
                { Detach(mainPid, false); return 0; }
            }

            string summary = "SUMMARY events=" + nEvt + " 1stChanceAV=" + nFirstAv +
                             " 2ndChanceAV=" + nSecondAv + " otherExc=" + nOtherExc +
                             " initBP=" + nBp + " ods=" + nOds + " dll=" + nDll;
            Log(summary);
            FlushIfDue(true);
            Console.WriteLine(summary);
            Console.WriteLine("log: " + logPath);
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine("FATAL: " + ex);
            try { if (_w != null) { _w.Write("FATAL: " + ex + "\r\n"); _w.Flush(); } } catch { }
            return 9;
        }
        finally
        {
            if (evtBuf != IntPtr.Zero) Marshal.FreeHGlobal(evtBuf);
            if (_w != null) { _w.Close(); _w = null; }
        }
    }
}
'@

try { Add-Type -TypeDefinition $cs -ErrorAction Stop }
catch { Write-Host "FATAL: 编译失败:`n$($_.Exception.Message)"; exit 9 }

# ---- 运行 ----
[NDbg]::Run($GameExe, $gameDir, $Mode, $TargetPid, $Quality, $DelayMs, $DetachAfterSec, $logPath)
