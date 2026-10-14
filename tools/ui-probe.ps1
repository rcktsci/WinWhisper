<#
    Opens the WinWhisper overlay with its global hotkey and captures just the
    panel into a PNG, so the visuals can be compared with the built-in Win+H bar.

    Usage:
        pwsh -File tools\ui-probe.ps1 [-Out <png path>] [-KeepOpen]
#>

param(
    [string]$Out = 'D:\RocketScience\AI\win-whisper\.acl-recovery\panel.png',
    [switch]$KeepOpen,
    [string]$Exe = 'D:\RocketScience\AI\win-whisper\x64\Release\WinWhisper.exe'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WwProbe {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
}
"@ -ErrorAction SilentlyContinue

Get-Process WinWhisper -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Start-Process -FilePath $Exe | Out-Null
Start-Sleep -Seconds 2

# Park the cursor on the primary screen so the panel anchors there.
[WwProbe]::SetCursorPos(960, 420) | Out-Null
Start-Sleep -Milliseconds 150

[WwProbe]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
[WwProbe]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero)
[WwProbe]::keybd_event(0x48, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[WwProbe]::keybd_event(0x48, 0, 2, [UIntPtr]::Zero)
[WwProbe]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
[WwProbe]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 120

# Grab it while it is up: the panel dismisses itself on an outside click, so
# waiting a fixed second can easily photograph an empty desktop.
$handle = [IntPtr]::Zero
for ($i = 0; $i -lt 40; $i++) {
    $handle = [WwProbe]::FindWindow('WinWhisperOverlayWindow', $null)
    if ($handle -ne [IntPtr]::Zero -and [WwProbe]::IsWindowVisible($handle)) { break }
    Start-Sleep -Milliseconds 50
}
if ($handle -eq [IntPtr]::Zero) { throw 'overlay window not found' }
if (-not [WwProbe]::IsWindowVisible($handle)) { throw 'overlay window is not visible' }

$rect = New-Object WwProbe+RECT
[void][WwProbe]::GetWindowRect($handle, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw 'overlay window has no size' }

# Grab a margin around the panel so the outline is visible against the desktop.
$margin = 14
$srcX = [Math]::Max(0, $rect.Left - $margin)
$srcY = [Math]::Max(0, $rect.Top - $margin)
$srcW = [Math]::Min([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width - $srcX, $w + $margin * 2)
$srcH = [Math]::Min([System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height - $srcY, $h + $margin * 2)

$bmp = New-Object System.Drawing.Bitmap $srcW, $srcH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($srcX, $srcY, 0, 0, (New-Object System.Drawing.Size($srcW, $srcH)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Host ("panel rect=({0},{1})-({2},{3}) saved to {4}" -f $rect.Left, $rect.Top, $rect.Right, $rect.Bottom, $Out)

if (-not $KeepOpen) {
    Get-Process WinWhisper -ErrorAction SilentlyContinue | Stop-Process -Force
}