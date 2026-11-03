# 将以太网网卡恢复为 DHCP（需要管理员权限）
# 用法：右键“以管理员身份运行”PowerShell，执行本脚本；或 powershell -ExecutionPolicy Bypass -File .\restore_ethernet_dhcp.ps1
param([string]$Alias = "以太网")
$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "请以管理员身份运行本脚本。" -ForegroundColor Red
    exit 1
}
$adapter = Get-NetAdapter -Name $Alias -ErrorAction Stop
Write-Host ("网卡: {0} | {1} | {2}" -f $adapter.Name, $adapter.InterfaceDescription, $adapter.Status)
netsh interface ipv4 set address name="$Alias" source=dhcp | Out-Host
netsh interface ipv4 set dns name="$Alias" source=dhcp | Out-Host
ipconfig /flushdns | Out-Null
Start-Sleep -Seconds 4
Write-Host "当前 IPv4："
Get-NetIPAddress -InterfaceAlias $Alias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Select-Object InterfaceAlias,IPAddress,PrefixLength,PrefixOrigin,SuffixOrigin | Format-Table -AutoSize
Write-Host "说明：直连板卡且无 DHCP 服务器时，Windows 会得到 169.254.x.x；这是链路本地地址，板卡回退 169.254.1.2，仍可自动发现。"
