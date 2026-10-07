# deploy.ps1 —— 把 DLC1 汉化 + 内存修复装到 HARD TRUCK APOCALYPSE RISE OF CLANS STEAM
#
# 布局依据本体（hta.exe）的实测部署方式 + 113 的实证：
#   · 译文 XML          -> 游戏 update\data\        （引擎的覆盖层）
#   · 烘好的 fonts.xml  -> 游戏 data\if\fonts\      （★ 本体就是这个位置，不是 update）
#   · cjk_*.dds         -> 游戏 data\if\fonts\
#   · hta_chs_cjk_dlc1.bin -> 游戏 update\
#   · winmm.dll         -> 游戏根目录（Ultimate ASI Loader）
#   · *.asi             -> 游戏 update\（加载器会扫这个目录；本体也是这么放的）
#
# 幂等：重复运行不会累积垃圾；fonts.xml 只在第一次运行时备份。
param(
    [string]$Game = 'I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM'
)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot

if (-not (Test-Path (Join-Path $Game 'Meridian113.exe'))) {
    throw "目标目录里没有 Meridian113.exe：$Game"
}

Write-Host "[1/5] 备份原有 fonts.xml（只备一次）"
$fontDir   = Join-Path $Game 'data\if\fonts'
$fontXml   = Join-Path $fontDir 'fonts.xml'
$backupDir = Join-Path $fontDir '_backup_dlc1_before_chs'
$backupXml = Join-Path $backupDir 'fonts.xml'
if ((Test-Path $fontXml) -and -not (Test-Path $backupXml)) {
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    Copy-Item $fontXml $backupXml -Force
    Write-Host "      已备份 -> $backupXml"
} elseif (Test-Path $backupXml) {
    Write-Host "      备份已存在，跳过"
} else {
    Write-Host "      警告：找不到 $fontXml"
}

Write-Host "[2/5] 安装 ASI 加载器（游戏根目录）"
Copy-Item (Join-Path $src 'winmm.dll') (Join-Path $Game 'winmm.dll') -Force

Write-Host "[3/5] 安装两个插件（update\）"
$upd = Join-Path $Game 'update'
New-Item -ItemType Directory -Force -Path $upd | Out-Null
Copy-Item (Join-Path $src 'hta_chs_dlc1.asi') (Join-Path $upd 'hta_chs_dlc1.asi') -Force
Copy-Item (Join-Path $src 'DLC1_MemFix.asi')  (Join-Path $upd 'DLC1_MemFix.asi')  -Force
Copy-Item (Join-Path $src 'DLC1_MemFix.ini')  (Join-Path $upd 'DLC1_MemFix.ini')  -Force

Write-Host "[4/5] 安装字库（data\if\fonts）"
New-Item -ItemType Directory -Force -Path $fontDir | Out-Null
Copy-Item (Join-Path $src 'data\if\fonts\fonts.xml') $fontXml -Force
Get-ChildItem (Join-Path $src 'data\if\fonts') -File -Filter 'cjk_*.dds' |
    ForEach-Object { Copy-Item $_.FullName $fontDir -Force }
Write-Host ("      fonts.xml + {0} 个 cjk_*.dds" -f (Get-ChildItem $fontDir -File -Filter 'cjk_*.dds').Count)

Write-Host "[5/5] 安装字库包与译文（update\）"
Copy-Item (Join-Path $src 'update\hta_chs_cjk_dlc1.bin') $upd -Force
robocopy (Join-Path $src 'update\data') (Join-Path $upd 'data') /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy 失败，退出码 $LASTEXITCODE" }

Write-Host ""
Write-Host "安装完成。启动 Meridian113.exe（建议配合 单核运行游戏.bat），然后看："
Write-Host "  汉化日志: $Game\update\hta_chs_dlc1.<时间戳>.log"
Write-Host "  修复日志: $Game\update\DLC1_MemFix.<时间戳>.log"
Write-Host "  进度文件: $Game\update\hta_chs_dlc1_progress.txt"
