# Render9Fix —— 部署脚本（把 ASI 装进游戏 / 卸下）
#
# 用法（在仓库根目录或任意位置）：
#   pwsh -File tools\deploy_render9fix.ps1                 # 安装（默认装到 update\）
#   pwsh -File tools\deploy_render9fix.ps1 -AlsoRoot       # 同时装到游戏根目录
#   pwsh -File tools\deploy_render9fix.ps1 -Revert         # 卸载（只删我们自己的文件）
#   pwsh -File tools\deploy_render9fix.ps1 -Build          # 先重新编译再安装
#
# 为什么默认装到 update\：现有 hta_chs.asi 就在那里并且确实被
# Ultimate-ASI-Loader (winmm.dll) 加载了 —— 这条路已经实证可用。
param(
    [switch]$Revert,
    [switch]$AlsoRoot,
    [switch]$Build,
    [string]$GameDir = 'I:\LocalGames\Hard Truck Apocalypse STEAM'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $repo 'Render9Fix'
$dll  = Join-Path $proj 'build\Release\Render9Fix.dll'
$ini  = Join-Path $proj 'Render9Fix.ini'

if (-not (Test-Path $GameDir)) { throw "游戏目录不存在: $GameDir" }
$update = Join-Path $GameDir 'update'

function Say($m) { Write-Host $m }

if ($Revert) {
    foreach ($d in @($update, $GameDir)) {
        foreach ($n in @('Render9Fix.asi', 'Render9Fix.ini')) {
            $p = Join-Path $d $n
            if (Test-Path $p) { Remove-Item $p -Force; Say "已删除 $p" }
        }
        # 顺带清掉我们产生的日志
        Get-ChildItem $d -Filter 'Render9Fix.*.log' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item $_.FullName -Force; Say "已删除 $($_.Name)" }
    }
    Say "`n已卸载。"
    return
}

if ($Build) {
    $msb = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msb)) {
        $msb = (Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe' |
                Select-Object -First 1).FullName
    }
    Say "编译中: $msb"
    & $msb (Join-Path $proj 'Render9Fix.vcxproj') /p:Configuration=Release /p:Platform=Win32 /v:m /nologo |
        Select-String -Pattern 'error|-> ' | ForEach-Object { Say $_.Line }
    if ($LASTEXITCODE -ne 0) { throw "编译失败" }
}

if (-not (Test-Path $dll)) { throw "找不到产物: $dll（先加 -Build 编译）" }

if (-not (Test-Path $update)) { New-Item -ItemType Directory -Path $update | Out-Null }

foreach ($d in @($update) + $(if ($AlsoRoot) { @($GameDir) } else { @() })) {
    Copy-Item $dll (Join-Path $d 'Render9Fix.asi') -Force
    Copy-Item $ini (Join-Path $d 'Render9Fix.ini') -Force
    Say "已安装: $(Join-Path $d 'Render9Fix.asi')"
    Say "         $(Join-Path $d 'Render9Fix.ini')"
}

Say @"

安装完成。接下来：
  1. 直接启动游戏（不需要调试器，不需要延迟启动器）。
  2. 进关卡（原来必崩的那一步）看是否还崩在 dxrender9+0x416D6。
  3. 把 update\Render9Fix.<时间戳>.log 发我 —— 重点看：
       * 「已知布局命中」或「选中 0x........」 —— 说明定位成功
       * 「安装完成」 —— 说明挂钩生效
       * 「统计: 导向 N 笔 ... 高地址遗留 M 笔」

如果日志里是「已知槽位 ... 未通过校验」「等待超时」，说明定位没成功，
把日志原样发我（里面带了每个候选值和拒因统计）。

卸载：pwsh -File tools\deploy_render9fix.ps1 -Revert
"@
