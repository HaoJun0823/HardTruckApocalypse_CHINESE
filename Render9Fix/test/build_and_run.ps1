# build_and_run.ps1 —— 编译并运行 Render9Fix 的端到端合成测试（不启动游戏）
#
# 做三件事：
#   1. 确保 Render9Fix.dll 存在（没有就调 MSBuild 编译）
#   2. 用 cl.exe 编译 host.exe 与假驱动 dxrender9.dll
#   3. 跑 host.exe，并打印插件刚写出的日志
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File Render9Fix\test\build_and_run.ps1
#
# 判据见同目录 README.md：所有 <= SizeMax 的分配应落在 0x60xxxxxx 低地址池。
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $here                     # Render9Fix/

function Find-First($paths) {
    foreach ($p in $paths) { if (Test-Path $p) { return $p } }
    return $null
}

# ── 工具链 ──────────────────────────────────────────────────────────────
$vc = Find-First @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
)
if (-not $vc) { throw '找不到 vcvars32.bat（需要 VS2017 或装了 v141 的 VS18）' }

# ── 1) 插件产物 ────────────────────────────────────────────────────────
$dll = Join-Path $proj 'build\Release\Render9Fix.dll'
if (-not (Test-Path $dll)) {
    $msb = Find-First @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\MSBuild\15.0\Bin\MSBuild.exe'
    )
    if (-not $msb) { throw '找不到 MSBuild.exe' }
    Write-Host "[1/3] 编译插件… ($msb)"
    & $msb (Join-Path $proj 'Render9Fix.vcxproj') /p:Configuration=Release /p:Platform=Win32 /v:m /nologo |
        Select-String -Pattern 'error|-> ' | ForEach-Object { Write-Host "      $($_.Line.Trim())" }
    if (-not (Test-Path $dll)) { throw "插件编译失败：$dll 不存在" }
} else {
    Write-Host "[1/3] 插件产物已存在：$dll"
}
Copy-Item $dll (Join-Path $here 'Render9Fix.dll') -Force

# 测试专用 ini：WaitSec 给足，让插件有时间完成扫描与安装
@"
[Render9Fix]
Enabled=1
SizeMax=65536
OnlyDxRender9=1
ArenaMB=8
ArenaBase=60000000
LogAlloc=1
WaitSec=30
"@ | Set-Content -Path (Join-Path $here 'Render9Fix.ini') -Encoding ASCII

# ── 2) 编译 host.exe 与假驱动 ──────────────────────────────────────────
Write-Host "[2/3] 编译测试程序…"
$cmd = 'call "{0}" >nul 2>&1 && cd /d "{1}" && ' +
       'cl /nologo /utf-8 /MT /O2 /EHsc host.cpp /Fe:host.exe /link /SUBSYSTEM:CONSOLE && ' +
       'cl /nologo /utf-8 /MT /O2 /EHsc /LD r9fakedx.cpp /Fe:dxrender9.dll /link /SUBSYSTEM:WINDOWS'
$cmd = $cmd -f $vc, $here
& cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw '编译测试程序失败' }

# ── 3) 运行 ────────────────────────────────────────────────────────────
Write-Host "[3/3] 运行…`n"
Push-Location $here
try { & (Join-Path $here 'host.exe') } finally { Pop-Location }

Write-Host "`n----------- 插件日志 -----------"
$log = Get-ChildItem $here -Filter 'Render9Fix.*.log' | Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) { Get-Content $log.FullName -Encoding UTF8 }
else { Write-Host "（没有生成日志？）" }
