# uninstall.ps1 —— 卸载 DLC1 汉化，恢复原版
param(
    [string]$Game = 'I:\LocalGames\HARD TRUCK APOCALYPSE RISE OF CLANS STEAM'
)
$ErrorActionPreference = 'Stop'

Write-Host "[1/4] 删除插件与 ASI 加载器"
Remove-Item (Join-Path $Game 'hta_chs_dlc1.asi') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Game 'winmm.dll')        -Force -ErrorAction SilentlyContinue

Write-Host "[2/4] 恢复 fonts.xml，删除 cjk_*.dds"
$fontDir   = Join-Path $Game 'data\if\fonts'
$backupXml = Join-Path $fontDir '_backup_dlc1_before_chs\fonts.xml'
if (Test-Path $backupXml) {
    Copy-Item $backupXml (Join-Path $fontDir 'fonts.xml') -Force
    Write-Host "      已从备份恢复 fonts.xml"
} else {
    Write-Host "      警告：没有备份，fonts.xml 保持原样（请从 Steam 校验文件恢复）"
}
Get-ChildItem $fontDir -File -Filter 'cjk_*.dds' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "[3/4] 删除 update\ 下的汉化文件"
$upd = Join-Path $Game 'update'
Remove-Item (Join-Path $upd 'hta_chs_cjk_dlc1.bin') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $upd 'data')                 -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Game 'hta_chs_dlc1.*.log')  -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $Game 'hta_chs_dlc1_progress.txt') -Force -ErrorAction SilentlyContinue

Write-Host "[4/4] 清理空目录"
$d = Join-Path $upd 'data'
if ((Test-Path $upd) -and -not (Get-ChildItem $upd -Force | Where-Object { $_.Name -ne 'data' })) {
    Remove-Item $upd -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "卸载完成。"
