# run_and_shoot_dlc1.ps1 —— 启动 Meridian113.exe + 边跑边截图 + 回收日志（DLC1 版）
#
# 用法:
#   .\run_and_shoot_dlc1.ps1                       # 跑 60 秒，每 8 秒截一张
#   .\run_and_shoot_dlc1.ps1 -Seconds 80 -NoDeploy
param(
    [int]$Seconds = 60,
    [int]$IntervalSeconds = 8,
    [switch]$NoDeploy
)

$ErrorActionPreference = 'Continue'
$game = 'I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM'
$dll  = 'G:\Projects\HardTruckApocalypse_CHINESE\HardTruckApocalypse_CHINESE_DLL_DLC1\build\Release\hta_chs_dlc1.dll'
$out  = 'G:\Projects\HardTruckApocalypse_CHINESE\_shots_dlc1'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out -Force | Out-Null }

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

Get-Process Meridian113 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

if (-not $NoDeploy) {
    Copy-Item $dll (Join-Path $game 'hta_chs_dlc1.asi') -Force
    Write-Host "已重新部署 hta_chs_dlc1.asi" -ForegroundColor DarkGray
}
# 清掉旧日志，只看本次
Get-ChildItem $game -Filter 'hta_chs_dlc1.*.log' -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item (Join-Path $game 'hta_chs_dlc1_progress.txt') -Force -ErrorAction SilentlyContinue

Write-Host "=== 启动 Meridian113.exe（跑 $Seconds 秒，每 $IntervalSeconds 秒截图）===" -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $game 'Meridian113.exe') -WorkingDirectory $game -PassThru
Write-Host "    PID = $($p.Id)"

$n = 0; $elapsed = 0.0; $dead = $false
while ($elapsed -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
    if ($p.HasExited) { $dead = $true; break }

    if (([math]::Abs(($elapsed % $IntervalSeconds)) -lt 0.25) -or ($n -eq 0 -and $elapsed -ge 3)) {
        $hwnd = [W]::Find([uint32]$p.Id)
        if ($hwnd -ne [IntPtr]::Zero) {
            [void][W]::ShowWindow($hwnd, 9)
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
                    $path = Join-Path $out ("dlc1_{0:d3}_t{1:d2}s.png" -f $n, [int]$elapsed)
                    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                    $bmp.Dispose()
                    Write-Host "  [截图] $path  ${w}x${h}" -ForegroundColor DarkGray
                }
            }
        }
    }
}

if ($dead) {
    Write-Host "  进程已退出（约 $([math]::Round($elapsed,1)) 秒）→ 可能崩溃或主动退出" -ForegroundColor Red
    Write-Host "  ExitCode = $($p.ExitCode)"
} else {
    Write-Host "  进程仍存活（$elapsed 秒）→ 未崩溃" -ForegroundColor Green
    Get-Process Meridian113 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 600
}

Write-Host ""
Write-Host "=== 插件日志 ===" -ForegroundColor Cyan
$logs = @(Get-ChildItem $game -Filter 'hta_chs_dlc1.*.log' -ErrorAction SilentlyContinue | Sort-Object Name)
if ($logs.Count -eq 0) {
    Write-Host "  (无日志 —— 说明 ASI 没被加载！)" -ForegroundColor Red
} else {
    foreach ($lg in $logs) {
        Write-Host "  --- $($lg.Name)  ($($lg.Length) 字节) ---" -ForegroundColor Yellow
        Get-Content $lg.FullName -Encoding UTF8 | ForEach-Object { '    ' + $_.TrimEnd() }
    }
}

Write-Host ""
Write-Host "=== exmachina.log 尾部（引擎自己）===" -ForegroundColor Cyan
$em = Join-Path $game 'exmachina.log'
if (Test-Path $em) { Get-Content $em -Tail 12 }
