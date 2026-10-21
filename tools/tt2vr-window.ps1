# Captures the TT2 window's client area as it appears on the desktop (what the player sees, after the mod's desktop
# mirror), and prints the window and monitor geometry. Usage: tools/tt2vr-window.ps1 <out.png>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TT2Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
'@
[TT2Win]::SetProcessDPIAware() | Out-Null
$h = (Get-Process TT2).MainWindowHandle
$wr = New-Object TT2Win+RECT; [TT2Win]::GetWindowRect($h, [ref]$wr) | Out-Null
$cr = New-Object TT2Win+RECT; [TT2Win]::GetClientRect($h, [ref]$cr) | Out-Null
$pt = New-Object TT2Win+POINT; [TT2Win]::ClientToScreen($h, [ref]$pt) | Out-Null
"window {0},{1} {2}x{3}; client at {4},{5} {6}x{7}" -f $wr.L, $wr.T, ($wr.R - $wr.L), ($wr.B - $wr.T), $pt.X, $pt.Y, $cr.R, $cr.B
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
    "monitor {0} {1}x{2} at {3},{4}{5}" -f $_.DeviceName, $_.Bounds.Width, $_.Bounds.Height, $_.Bounds.X, $_.Bounds.Y, $(if ($_.Primary) { ' (primary)' } else { '' })
}
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $cr.R)), ([Math]::Max(1, $cr.B))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out"
