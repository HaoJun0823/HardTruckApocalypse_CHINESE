# 抓取游戏窗口截图，保存到 _shots\
# 用法: powershell -File capture.ps1 -Seconds 90 -IntervalSeconds 10
param(
    [int]$Seconds = 90,
    [int]$IntervalSeconds = 10,
    [string]$OutDir = "G:\Projects\HardTruckApocalypse_CHINESE\_shots"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static string Title(IntPtr h) {
        int n = GetWindowTextLength(h);
        if (n <= 0) return "";
        var sb = new System.Text.StringBuilder(n + 2);
        GetWindowText(h, sb, sb.Capacity);
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing, System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Proc {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
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

$proc = Get-Process -Name hta -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output "[截图] 未找到 hta 进程，请先启动游戏"; exit 1 }

$hwnd = [Proc]::Find([uint32]$proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Output "[截图] 找不到窗口句柄"; exit 1 }

Write-Output "[截图] hta PID=$($proc.Id) HWND=0x$($hwnd.ToInt64().ToString('X')) 标题='$([Win]::Title($hwnd))'"

# 拉起并置前，避免抓到黑屏
[void][Win]::ShowWindow($hwnd, 9)   # SW_RESTORE
[void][Win]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 800

$n = 0
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    $r = New-Object Win+RECT
    if ([Win]::GetWindowRect($hwnd, [ref]$r)) {
        $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
        if ($w -gt 100 -and $h -gt 100) {
            $bmp = New-Object System.Drawing.Bitmap $w, $h
            $gfx = [System.Drawing.Graphics]::FromImage($bmp)
            $gfx.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
            $gfx.Dispose()
            $n++
            $path = Join-Path $OutDir ("shot_{0:d3}_{1:HHmmss}.png" -f $n, (Get-Date))
            $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            $bmp.Dispose()
            Write-Output "[截图] $path  ${w}x${h}"
        }
    }
    Start-Sleep -Seconds $IntervalSeconds
}
Write-Output "[截图] 共 $n 张，结束"