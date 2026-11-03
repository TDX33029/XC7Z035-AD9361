# 只读检查：有线/无线网卡、IPv4、默认路由（不需要管理员权限）
Write-Host "== 网卡 =="
Get-NetAdapter -Name '以太网','WLAN' -ErrorAction SilentlyContinue |
    Select-Object Name,InterfaceDescription,Status,LinkSpeed | Format-Table -AutoSize
Write-Host "== IPv4 =="
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.InterfaceAlias -in @('以太网','WLAN') } |
    Select-Object InterfaceAlias,IPAddress,PrefixLength,PrefixOrigin,SuffixOrigin | Format-Table -AutoSize
Write-Host "== 默认路由 =="
Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
    Select-Object InterfaceAlias,NextHop,RouteMetric | Format-Table -AutoSize
Write-Host "== 板卡 DHCP 记录（路由器管理页查看 MAC 00:0A:35:00:00:02） =="
