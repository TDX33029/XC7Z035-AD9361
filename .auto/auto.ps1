# Automation toolkit for USART HMI (Win32 SendInput based). Run: powershell -NoProfile -ExecutionPolicy Bypass -File auto.ps1 <action> [args...]
param([Parameter(Mandatory=$true)][string]$Action, [string]$P1, [string]$P2, [string]$P3, [string]$P4)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetActiveWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
[W32]::SetProcessDPIAware() | Out-Null

function Get-HmiWindow {
  $found = [IntPtr]::Zero
  $cb = [W32+EnumProc]{ param($h, $lp)
    if ([W32]::IsWindowVisible($h)) {
      $sb = New-Object System.Text.StringBuilder 256
      [W32]::GetWindowText($h, $sb, 256) | Out-Null
      $t = $sb.ToString()
      if ($t -match 'USART\s*HMI') { $script:found = $h; return $false }
    }
    return $true
  }
  [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  return $script:found
}

switch ($Action) {
  'focus' {
    $h = Get-HmiWindow
    if ($h -eq [IntPtr]::Zero) { Write-Output 'NOWINDOW'; exit 1 }
    [W32]::ShowWindow($h, 3) | Out-Null   # maximize
    Start-Sleep -Milliseconds 250
    # alt-tap trick to allow SetForegroundWindow from background process
    [W32]::keybd_event(0x12,0,0,[UIntPtr]::Zero)
    [W32]::SetForegroundWindow($h) | Out-Null
    [W32]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    if ([W32]::GetForegroundWindow() -ne $h) {
      [System.Windows.Forms.SendKeys]::SendWait('%')
      Start-Sleep -Milliseconds 100
      [W32]::SetForegroundWindow($h) | Out-Null
      Start-Sleep -Milliseconds 150
    }
    $fg = [W32]::GetForegroundWindow()
    Write-Output ("FG={0} TARGET={1}" -f $fg, $h)
    $r = New-Object W32+RECT; [W32]::GetWindowRect($h, [ref]$r) | Out-Null
    $c = New-Object W32+RECT; [W32]::GetClientRect($h, [ref]$c) | Out-Null
    $p = New-Object W32+POINT; $p.X = 0; $p.Y = 0; [W32]::ClientToScreen($h, [ref]$p) | Out-Null
    Write-Output ("WIN L={0} T={1} R={2} B={3} CLIENT {4}x{5} AT {6},{7}" -f $r.L,$r.T,$r.R,$r.B,$c.R,$c.B,$p.X,$p.Y)
  }
  'shot' {
    $out = if ($P1) { $P1 } else { "$PSScriptRoot\shot.png" }
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "SAVED $out $([System.Windows.Forms.SystemInformation]::VirtualScreen.Width)x$([System.Windows.Forms.SystemInformation]::VirtualScreen.Height)"
  }
  'crop' { # crop saved shot: auto.ps1 crop <x> <y> <w> <h>
    $src = "$PSScriptRoot\shot.png"
    $img = [System.Drawing.Image]::FromFile($src)
    $bmp = New-Object System.Drawing.Bitmap ([int]$P3), ([int]$P4)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0,0,$P3,$P4), (New-Object System.Drawing.Rectangle $P1,$P2,$P3,$P4), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose(); $bmp.Save("$PSScriptRoot\crop.png",[System.Drawing.Imaging.ImageFormat]::Png)
    $g2 = $bmp.Clone(); $bmp.Dispose(); $img.Dispose()
    Write-Output "CROPPED $P1,$P2 ${P3}x${P4}"
  }
  'move'   { [W32]::SetCursorPos([int]$P1, [int]$P2) | Out-Null; Write-Output "MOVED" }
  'click'  { # click x y [count]
    [W32]::SetCursorPos([int]$P1, [int]$P2) | Out-Null
    Start-Sleep -Milliseconds 60
    $n = if ($P3) { [int]$P3 } else { 1 }
    for ($i=0; $i -lt $n; $i++) {
      [W32]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [W32]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
      Start-Sleep -Milliseconds 70
    }
    Write-Output "CLICKED $P1,$P2 x$n"
  }
  'rclick' {
    [W32]::SetCursorPos([int]$P1, [int]$P2) | Out-Null
    Start-Sleep -Milliseconds 60
    [W32]::mouse_event(8,0,0,0,[UIntPtr]::Zero); [W32]::mouse_event(16,0,0,0,[UIntPtr]::Zero)
    Write-Output "RCLICKED $P1,$P2"
  }
  'drag' { # drag x1 y1 x2 y2
    [W32]::SetCursorPos([int]$P1, [int]$P2) | Out-Null; Start-Sleep -Milliseconds 80
    [W32]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    $steps = 12
    for ($i=1; $i -le $steps; $i++) {
      $x = [int]($P1 + ($P3-$P1)*$i/$steps); $y = [int]($P2 + ($P4-$P2)*$i/$steps)
      [W32]::SetCursorPos($x,$y) | Out-Null; Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Milliseconds 60
    [W32]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    Write-Output "DRAG $P1,$P2 -> $P3,$P4"
  }
  'setclip' { # setclip <text>
    [System.Windows.Forms.Clipboard]::SetText($P1, [System.Windows.Forms.TextDataFormat]::UnicodeText)
    Write-Output 'CLIPSET'
  }
  'paste' { # paste <text> at current focus via Ctrl+V
    [System.Windows.Forms.Clipboard]::SetText($P1, [System.Windows.Forms.TextDataFormat]::UnicodeText)
    Start-Sleep -Milliseconds 80
    [W32]::keybd_event(0x11,0,0,[UIntPtr]::Zero)
    [System.Windows.Forms.SendKeys]::SendWait('^v')
    [W32]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
    Write-Output 'PASTED'
  }
  'keys' { # keys <sendkeys expression> e.g. '{ENTER}' '^a' '{ESC}' '^s'
    [System.Windows.Forms.SendKeys]::SendWait($P1)
    Write-Output "KEYS $P1"
  }
  'type' { # type <text> via clipboard+paste then optional enter: type text enter?
    [System.Windows.Forms.Clipboard]::SetText($P1, [System.Windows.Forms.TextDataFormat]::UnicodeText)
    Start-Sleep -Milliseconds 80
    [System.Windows.Forms.SendKeys]::SendWait('^v')
    Start-Sleep -Milliseconds 60
    if ($P2 -eq 'enter') { [System.Windows.Forms.SendKeys]::SendWait('{ENTER}') }
    Write-Output "TYPED"
  }
  'selall' {
    [System.Windows.Forms.SendKeys]::SendWait('^a')
    Write-Output 'SELALL'
  }
  'info' {
    $vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
    Write-Output ("SCREEN {0}x{1}" -f $vs.Width, $vs.Height)
    $h = Get-HmiWindow
    if ($h -ne [IntPtr]::Zero) { Write-Output "HMIHWND $h" } else { Write-Output 'NOWINDOW' }
  }
  default { Write-Output "UNKNOWN ACTION $Action" }
}
