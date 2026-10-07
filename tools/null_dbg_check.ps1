# null_dbg_check.ps1 —— 语法/编译自检 tools\null_dbg.ps1（不启动游戏）
#
# 为什么需要它：
#   null_dbg.ps1 把调试器主体写成一段**内嵌的 C#**（`$cs = @'...'@`），
#   由 PowerShell 的 Add-Type 现场编译。内嵌代码写错时，只有在真正跑起来
#   才会报错；而跑一次要等游戏启动（几十秒）。
#   这个脚本把那段 C# 抠出来单独编译，**几秒钟**就能发现语法/类型错误。
#
#   另外两个坑，它也顺手挡住了：
#     * null_dbg.ps1 必须是 **UTF-8 带 BOM**，否则 PowerShell 5.1 会按 GBK 解析中文报错
#     * 游戏是 32 位，C# 必须在 **32 位 PowerShell** 下编译（本脚本会自检位数）
#
# 用法（务必用 32 位 PowerShell，脚本会自动重启自己）：
#   powershell -ExecutionPolicy Bypass -File tools\null_dbg_check.ps1
#
# 期望输出：
#   BOM=239,187,191  parse_errors=0
#   extract=True len=NNNNN
#   COMPILE OK  bits=4  type=True

$ErrorActionPreference = 'Stop'

# 自动切到 32 位 PowerShell（游戏是 32 位，C# 里的 P/Invoke 结构体布局按 32 位写的）
if ([IntPtr]::Size -ne 4) {
    $ps32 = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path $ps32) {
        & $ps32 -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path @args
        exit $LASTEXITCODE
    }
    Write-Output "警告：找不到 32 位 PowerShell，继续用 64 位（编译结果不可信）"
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ps1  = Join-Path $here 'null_dbg.ps1'
if (-not (Test-Path $ps1)) { throw "找不到 $ps1" }

# ── 1) BOM 检查（PowerShell 5.1 对无 BOM 的 UTF-8 会按 GBK 读，中文全部报错）──
$bytes = [IO.File]::ReadAllBytes($ps1)
"BOM=$($bytes[0]),$($bytes[1]),$($bytes[2])"

# ── 2) 语法解析 ──
$err = $null
[System.Management.Automation.Language.Parser]::ParseFile($ps1, [ref]$null, [ref]$err) | Out-Null
"parse_errors=$($err.Count)"
if ($err.Count) { $err | Select-Object -First 5 | ForEach-Object { "  $($_.Extent.StartLineNumber): $($_.Message)" } }

# ── 3) 抠出内嵌 C# 并编译 ──
$src = [IO.File]::ReadAllText($ps1, [Text.Encoding]::UTF8)
$m = [regex]::Match($src, '(?s)\$cs = @''(.*?)''@')
"extract=$($m.Success) len=$($m.Groups[1].Value.Length)"
if (-not $m.Success) { throw "没能从 null_dbg.ps1 里抠出 `$cs = @'...'@ 代码块" }

try {
    Add-Type -TypeDefinition $m.Groups[1].Value
    Write-Output ("COMPILE OK  bits=" + [IntPtr]::Size + "  type=" + ([NDbg] -ne $null))
} catch {
    Write-Output ("COMPILE FAIL: " + $_.Exception.Message)
    if ($_.Exception.InnerException) { Write-Output $_.Exception.InnerException.Message }
    exit 1
}
