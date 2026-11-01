Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class D2 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct POINT { public int X,Y; }
  public struct RECT { public int L,T,R,B; }
}
"@
[D2]::SetProcessDPIAware() | Out-Null
$x=96; $y=289
[D2]::SetCursorPos($x,$y) | Out-Null
Start-Sleep -Milliseconds 200
$p = New-Object D2+POINT
[D2]::GetCursorPos([ref]$p) | Out-Null
$h = [D2]::WindowFromPoint($p)
$ga = [D2]::GetAncestor($h, 2)  # GA_ROOT
$sb = New-Object System.Text.StringBuilder 256
[D2]::GetWindowText($ga, $sb, 256) | Out-Null
$pid2 = 0
[D2]::GetWindowThreadProcessId($ga, [ref]$pid2) | Out-Null
$r = New-Object D2+RECT
[D2]::GetWindowRect($ga, [ref]$r) | Out-Null
Write-Output "cursor=($($p.X),$($p.Y)) hwnd=$h root=$ga title='$($sb.ToString())' pid=$pid2 rect=$($r.L),$($r.T),$($r.R),$($r.B)"
Get-Process -Id $pid2 | Select-Object Id,ProcessName,MainWindowTitle | Format-List | Out-String
