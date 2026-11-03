# 读取板卡调试串口 UART1 的启动日志（不需要管理员权限）
# 接线：USB-TTL 的 RX 接 JM1 pin37 / W29 / IO_L17P_12（板端 TX）；
#       GND 接 pin33 或 pin35；115200 8N1。不要接到屏幕 UART0 的 AJ28(pin17)。
# 用法：powershell -ExecutionPolicy Bypass -File .\serial_capture.ps1
#       powershell -ExecutionPolicy Bypass -File .\serial_capture.ps1 -Port COM15 -Seconds 20
param(
    [string]$Port = "COM15",
    [int]$Seconds = 20,
    [int]$Baud = 115200,
    [string]$LogDir = ""
)
$ErrorActionPreference = "Stop"
if (-not $LogDir) {
    $LogDir = Join-Path (Split-Path -Parent $PSScriptRoot) "logs"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log = Join-Path $LogDir "serial_${Port}_$stamp.txt"

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
$sp.ReadTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$bytes = New-Object System.Collections.Generic.List[byte]
try {
    $sp.Open()
    Write-Host "已打开 $Port @ $Baud 8N1，采集 $Seconds 秒；按 Ctrl+C 提前结束。" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $b = $sp.ReadByte()
            if ($b -ge 0) {
                $bytes.Add([byte]$b)
                Write-Host -NoNewline ([char]$b)
            }
        } catch [System.TimeoutException] {
        }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
Write-Host ""
[IO.File]::WriteAllBytes($log, $bytes.ToArray())
Write-Host ("采集完成：{0} 字节 -> {1}" -f $bytes.Count, $log) -ForegroundColor Green
if ($bytes.Count -eq 0) {
    Write-Host "未收到任何字节。请检查：" -ForegroundColor Yellow
    Write-Host "  1) USB-TTL 的 RX 是否接 W29（JM1 pin37，板端 UART1 TX）"
    Write-Host "  2) GND 是否接 pin33/35，波特率 115200 8N1"
    Write-Host "  3) 板卡是否已重新上电（写卡后必须断电重插 SD 再上电）"
    Write-Host "  4) 是否误接在屏幕 UART0 的 AJ28（pin17）"
}
