# run_and_shoot.ps1 —— 启动游戏 + 定期截图 + 回收日志（一条命令完成验证）
#
# 与 run_and_check.ps1 的区别：本脚本**在运行中截图**，能看到画面上的汉字，
# 而不是只看日志和"有没有崩"。
#
# 用法:
#   .\run_and_shoot.ps1                      # 部署 + 跑 50 秒，每 10 秒截一张
#   .\run_and_shoot.ps1 -Seconds 70 -IntervalSeconds 7
#   .\run_and_shoot.ps1 -NoDeploy -NoScan    # 不重新部署、带 HTA_CHS_NO_SCAN=1
param(
    [int]$Seconds = 50,
    [int]$IntervalSeconds = 10,
    [switch]$NoDeploy,
    [switch]$NoScan,
    [switch]$KeepLog
)

$ErrorActionPreference = 'Continue'
$game = 'I:\LocalGames\Hard Truck Apocalypse STEAM'
$dll  = 'G:\Projects\HardTruckApocalypse_CHINESE\HardTruckApocalypse_CHINESE_DLL\build\Release\hta_chs.dll'
$log  = Join-Path $game 'update\hta_chs.log'
$exc  = Join-Path $game 'exceptions'
$out  = 'G:\Projects\HardTruckApocalypse_CHINESE\_shots'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out -Force | Out-Null }

# ── 找窗口句柄 ────────────────────────────────────────────────────────────
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Find(uint want) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == want) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

# 记录跑之前的异常日志
$before = @(Get-ChildItem $exc -Filter 'hta.exe*.log' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch 'game' } | Select-Object -ExpandProperty Name)

Get-Process hta -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if (-not $NoDeploy) { Copy-Item $dll (Join-Path $game 'update\hta_chs.asi') -Force }
if (-not $KeepLog) { Remove-Item $log -Force -ErrorAction SilentlyContinue }

# 环境变量：HTA_CHS_NO_SCAN=1 时跳过堆扫描（基线条件）
if ($NoScan) { $env:HTA_CHS_NO_SCAN = '1' } else { Remove-Item Env:\HTA_CHS_NO_SCAN -ErrorAction SilentlyContinue }

Write-Host "=== 启动 hta.exe（跑 $Seconds 秒，每 $IntervalSeconds 秒截图）===" -ForegroundColor Cyan
Write-Host "    HTA_CHS_NO_SCAN = $(if ($env:HTA_CHS_NO_SCAN) { $env:HTA_CHS_NO_SCAN } else { '<未设>' })"
$p = Start-Process -FilePath (Join-Path $game 'hta.exe') -WorkingDirectory $game -PassThru
Write-Host "    PID = $($p.Id)"

# ── 主循环：边跑边截图 ────────────────────────────────────────────────────
$n = 0
$elapsed = 0
$dead = $false
while ($elapsed -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
    if ($p.HasExited) { $dead = $true; break }

    if ([math]::Abs($elapsed % $IntervalSeconds) -lt 0.25 -or ($n -eq 0 -and $elapsed -ge 3)) {
        $hwnd = [W]::Find([uint32]$p.Id)
        if ($hwnd -ne [IntPtr]::Zero) {
            [void][W]::ShowWindow($hwnd, 9)          # SW_RESTORE
            [void][W]::SetForegroundWindow($hwnd)
            Start-Sleep -Milliseconds 400
            $r = New-Object W+RECT
            if ([W]::GetWindowRect($hwnd, [ref]$r)) {
                $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
                if ($w -gt 100 -and $h -gt 100) {
                    $bmp = New-Object System.Drawing.Bitmap $w, $h
                    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
                    $gfx.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
                    $gfx.Dispose()
                    $n++
                    $path = Join-Path $out ("run_{0:d3}_t{1:d2}s.png" -f $n, [int]$elapsed)
                    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                    $bmp.Dispose()
                    Write-Host "  [截图] $path  ${w}x${h}" -ForegroundColor DarkGray
                }
            }
        }
    }
}

if ($dead) {
    Write-Host "  进程已退出（约 $([math]::Round($elapsed,1)) 秒）→ 崩溃" -ForegroundColor Red
} else {
    Write-Host "  进程仍存活（$elapsed 秒）→ 未崩溃" -ForegroundColor Green
    Get-Process hta -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "=== 插件日志 ===" -ForegroundColor Cyan
if (Test-Path $log) {
    Get-Content $log -Encoding UTF8 |
        Select-String '填充|收集|登记|装配|就绪|失败|异常|包文件|调试|rel32|补丁|自检|汉字表' |
        ForEach-Object { '  ' + $_.Line.Trim() }
} else { Write-Host "  (无日志)" }

$after = @(Get-ChildItem $exc -Filter 'hta.exe*.log' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'game' } | Select-Object -ExpandProperty Name)
$new = $after | Where-Object { $before -notcontains $_ }
if ($new) {
    Write-Host ""
    Write-Host "=== 新崩溃日志: $($new -join ', ') ===" -ForegroundColor Red
    foreach ($nm in $new) {
        Get-Content (Join-Path $exc $nm) -Encoding UTF8 |
            Select-String 'Exception|EIP|EAX|EBX|ECX|EDX|ESI|EDI|ESP' |
            ForEach-Object { '  ' + $_.Line.Trim() }
    }
} else {
    Write-Host ""
    Write-Host "=== 本次没有新的崩溃日志 ===" -ForegroundColor Green
}
