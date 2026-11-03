# 将所选 BOOT 镜像写入 SD 卡并校验（不需要管理员权限）
# 用法：powershell -ExecutionPolicy Bypass -File .\write_sd_boot.ps1 -Drive E
#       powershell -ExecutionPolicy Bypass -File .\write_sd_boot.ps1 -Drive E -Image BOOT_PROTECTED.BIN
param([string]$Drive = "", [string]$Image = "BOOT.BIN")
$ErrorActionPreference = "Stop"
$outRoot = Split-Path -Parent $PSScriptRoot
$Image = Split-Path -Leaf $Image
$src  = Join-Path $outRoot "sdboot_deploy\$Image"
$safe = Join-Path $outRoot "sdboot_deploy\BOOT_SAFE_v3.BIN"
if (-not (Test-Path $src)) { throw "缺少 $src" }
if (-not $Drive) {
    Write-Host "可移动磁盘："
    Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter } |
        Format-Table DriveLetter,FileSystemLabel,FileSystem,Size,SizeRemaining
    $Drive = Read-Host "请输入 SD 卡盘符（例如 E）"
}
$letter = $Drive.TrimEnd(':', '\', '/')
$vol = Get-Volume -DriveLetter $letter -ErrorAction Stop
if ($vol.DriveType -ne 'Removable') { throw "盘符 $letter 不是可移动磁盘，已中止。" }
if ($vol.FileSystem -ne 'FAT32') { Write-Warning "文件系统为 $($vol.FileSystem)；Zynq BootROM 通常需要 FAT32。" }
$dst = "${letter}:\BOOT.BIN"
if (Test-Path $dst) {
    $backup = "${letter}:\BOOT_OLD_$(Get-Date -Format yyyyMMdd_HHmmss).BIN"
    Copy-Item $dst $backup
    Write-Host "已备份旧 BOOT.BIN -> $backup"
}
Write-Host "写入镜像: $Image -> ${letter}:\BOOT.BIN"
Copy-Item $src $dst -Force
Copy-Item $safe "${letter}:\BOOT_SAFE_v3.BIN" -Force
$expect = (Get-FileHash $src -Algorithm SHA256).Hash
$actual = (Get-FileHash $dst -Algorithm SHA256).Hash
if ($actual -ne $expect) { throw "BOOT.BIN 校验失败：$actual != $expect" }
Write-Host ("BOOT.BIN  {0}" -f $actual) -ForegroundColor Green
Write-Host ("回退镜像 {0}" -f (Get-FileHash "${letter}:\BOOT_SAFE_v3.BIN" -Algorithm SHA256).Hash)
Write-Host "写卡完成并校验通过。安全弹出后插回板卡上电，等待约15秒。" -ForegroundColor Green
