# run_and_check.ps1 —— 自动跑游戏并回收日志（调试循环用）
#
# 用法:
#   .\run_and_check.ps1               # 部署 + 跑 25 秒 + 报告
#   .\run_and_check.ps1 -Seconds 40   # 跑更久
#   .\run_and_check.ps1 -NoDeploy     # 只跑，不重新部署
param(
    [int]$Seconds = 25,
    [switch]$NoDeploy,
    [switch]$KeepLog
)
$ErrorActionPreference = 'Continue'
$game = 'I:\LocalGames\Hard Truck Apocalypse STEAM'
$dll  = 'G:\Projects\HardTruckApocalypse_CHINESE\HardTruckApocalypse_CHINESE_DLL\build\Release\hta_chs.dll'
$log  = Join-Path $game 'update\hta_chs.log'
$exc  = Join-Path $game 'exceptions'

# 记录跑之前的异常日志清单，用来判断这次有没有产生新的
$before = @(Get-ChildItem $exc -Filter 'hta.exe*.log' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch 'game' } | Select-Object -ExpandProperty Name)

Get-Process hta -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

if (-not $NoDeploy) {
    Copy-Item $dll (Join-Path $game 'update\hta_chs.asi') -Force
}
if (-not $KeepLog) { Remove-Item $log -Force -ErrorAction SilentlyContinue }

Write-Host "=== 启动 hta.exe（最多等 $Seconds 秒）===" -ForegroundColor Cyan
$p = Start-Process -FilePath (Join-Path $game 'hta.exe') -WorkingDirectory $game -PassThru
$crashed = $false
$elapsed = 0
while ($elapsed -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $elapsed += 0.5
    if ($p.HasExited) { $crashed = $true; break }
}
if (-not $crashed) {
    Write-Host "  进程仍存活（$elapsed 秒）→ 未崩溃" -ForegroundColor Green
    Get-Process hta -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
} else {
    Write-Host "  进程已退出（约 $([math]::Round($elapsed,1)) 秒）→ 崩溃" -ForegroundColor Red
}

Write-Host ""
Write-Host "=== 插件日志（最后 30 行）===" -ForegroundColor Cyan
if (Test-Path $log) { Get-Content $log -Encoding UTF8 | Select-Object -Last 30 }
else { Write-Host "  (无日志)" }

$after = @(Get-ChildItem $exc -Filter 'hta.exe*.log' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'game' } | Select-Object -ExpandProperty Name)
$new = $after | Where-Object { $before -notcontains $_ }
if ($new) {
    Write-Host ""
    Write-Host "=== 新崩溃日志: $($new -join ', ') ===" -ForegroundColor Red
    foreach ($n in $new) {
        Get-Content (Join-Path $exc $n) -Encoding UTF8 |
            Select-String 'Exception|EIP|EAX|EBX|ECX|EDX|ESI|EDI|ESP|n_SetWeather' |
            ForEach-Object { '  ' + $_.Line.Trim() }
    }
} else {
    Write-Host ""
    Write-Host "=== 本次没有新的崩溃日志 ===" -ForegroundColor Green
}
