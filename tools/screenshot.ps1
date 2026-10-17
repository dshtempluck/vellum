<#
.SYNOPSIS
  Launches vellum with a document and captures its window to a PNG.

.DESCRIPTION
  Used to check the viewer renders correctly without a person at the
  keyboard. Waits for the window to appear and settle, optionally sends
  keystrokes, then captures the window with PrintWindow.

.EXAMPLE
  .\screenshot.ps1 -Document ..\samples\djvu3spec.djvu -Out shot.png
  .\screenshot.ps1 -Document a.pdf -Out shot.png -Keys '^2','{F4}'
#>
param(
  [Parameter(Mandatory = $true)][string[]]$Document,
  [Parameter(Mandatory = $true)][string]$Out,
  [int]$SettleMs = 2500,
  [string[]]$Keys = @(),
  [int[]]$Commands = @(),
  [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"

# DPI awareness has to be set before anything else, and certainly before
# System.Windows.Forms loads: that assembly fixes the process's awareness on
# load, after which this call is refused and every coordinate read back is
# silently scaled -- which shows up as a capture that clips the window.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool alt);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
if (-not [Win]::SetProcessDpiAwarenessContext([IntPtr](-4))) {   # PER_MONITOR_AWARE_V2
  Write-Warning "could not set DPI awareness; the capture may be clipped"
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$exe = Join-Path $PSScriptRoot "..\build\vellum.exe"
if (-not (Test-Path $exe)) { throw "vellum.exe not built" }
$Out = [System.IO.Path]::GetFullPath($Out)

# Several documents open as several tabs, which is how the tab strip is
# exercised without driving the file dialog.
$quoted = ($Document | ForEach-Object { '"' + $_ + '"' }) -join ' '
$proc = Start-Process -FilePath $exe -ArgumentList $quoted -PassThru
try {
  $deadline = (Get-Date).AddSeconds(20)
  while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
  }
  if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw "no window appeared" }
  # Capture the handle now: opening a menu can make the process report a
  # popup as its main window.
  $hwnd = $proc.MainWindowHandle

  # Windows refuses SetForegroundWindow to a process that does not own the
  # foreground; SwitchToThisWindow is the reliable way in from a script.
  [Win]::SwitchToThisWindow($hwnd, $true)
  [Win]::SetForegroundWindow($hwnd) | Out-Null
  Start-Sleep -Milliseconds $SettleMs

  if ($Keys.Count -gt 0) {
    if ([Win]::GetForegroundWindow() -ne $hwnd) {
      throw "vellum is not the foreground window; keystrokes would go elsewhere"
    }
    foreach ($k in $Keys) {
      [System.Windows.Forms.SendKeys]::SendWait($k)
      Start-Sleep -Milliseconds 1000
    }
    Start-Sleep -Milliseconds $SettleMs
  }

  # Commands can also be driven directly, which exercises the command
  # handlers without depending on keyboard focus.
  foreach ($c in $Commands) {
    [Win]::SendMessageW($hwnd, 0x8002, [IntPtr]$c, [IntPtr]::Zero) | Out-Null  # kMsgCommand
    Start-Sleep -Milliseconds 900
  }
  if ($Commands.Count -gt 0) { Start-Sleep -Milliseconds $SettleMs }

  $r = New-Object Win+RECT
  [Win]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $w = $r.Right - $r.Left
  $h = $r.Bottom - $r.Top
  if ($w -le 0 -or $h -le 0) { throw "window has no size" }

  # PrintWindow renders the window itself, so anything sitting on top of it
  # cannot end up in the capture.
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  $ok = [Win]::PrintWindow($hwnd, $dc, 2)   # PW_RENDERFULLCONTENT
  $g.ReleaseHdc($dc)
  if (-not $ok) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
  $g.Dispose()
  $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host "captured $Out ($w x $h)"
}
finally {
  if (-not $KeepOpen -and -not $proc.HasExited) {
    $proc.CloseMainWindow() | Out-Null
    Start-Sleep -Milliseconds 600
    if (-not $proc.HasExited) { $proc.Kill() }
  }
}
