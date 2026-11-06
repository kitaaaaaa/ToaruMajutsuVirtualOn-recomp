# Saves pictures of vox.exe's own window with PrintWindow (only that window, never the rest of the screen).
# Args: -Delays "10,20,30" (seconds from now) -Prefix shot
param([string]$Delays = '20', [string]$Prefix = 'shot')
$times = @($Delays -split '[, ]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class VoxWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$start = Get-Date
foreach ($d in $times) {
    $wait = $d - ((Get-Date) - $start).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    $p = Get-Process vox -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { "no vox window at ${d}s"; continue }
    $r = New-Object VoxWin+RECT
    [void][VoxWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { "empty window at ${d}s"; continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    $ok = [VoxWin]::PrintWindow($p.MainWindowHandle, $dc, 2) # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc); $g.Dispose()
    $path = "$port\work\$Prefix-$d.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $sample = 0; $seen = @{}
    for ($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 40))) { for ($y = 0; $y -lt $h; $y += [Math]::Max(1, [int]($h / 40))) { $seen[$bmp.GetPixel($x, $y).ToArgb()] = 1; $sample++ } }
    $bmp.Dispose()
    "captured ${d}s: ${w}x${h}, PrintWindow=$ok, $($seen.Count) distinct colours in $sample samples -> $path"
}
