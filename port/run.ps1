# Builds and runs the host once. Log: port\work\run.log. Args: [-Timeout 30] [-Lines 70] [-Interp]
param([int]$Timeout = 30, [int]$Lines = 70, [switch]$Interp, [switch]$NoBuild)
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
Set-Location "$port\work"
if (-not $NoBuild) {
    $b = powershell -NoProfile -ExecutionPolicy Bypass -File "$port\build_runtime.ps1"
    if ($LASTEXITCODE -ne 0) { $b | Select-Object -Last 14 | ForEach-Object { $_.Substring(0, [Math]::Min(190, $_.Length)) }; exit 1 }
}
$env:BB_JIT = '0'
if (-not $Interp) { $env:BB_RECOMP_LIB = "$port\work\recomp\librecomp.dll" }
$sw = [Diagnostics.Stopwatch]::StartNew()
& "$port\out\vox.exe" . names.tsv --timeout $Timeout --app0 "$root\extracted\uroot" --user "$port\work\user" 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 run.log
$code = $LASTEXITCODE
$all = Get-Content run.log
$body = $all | Where-Object { $_ -notmatch '^(Names:|Module |UNRESOLVED DATA|Image at|Recompiled:)' }
$body | Select-Object -First $Lines | ForEach-Object { $_.Substring(0, [Math]::Min(175, $_.Length)) }
if (@($body).Count -gt $Lines) { "... ($(@($body).Count) lines, showing the first $Lines; last 14:)"; $body | Select-Object -Last 14 | ForEach-Object { $_.Substring(0, [Math]::Min(175, $_.Length)) } }
"--- exit $code after $([Math]::Round($sw.Elapsed.TotalSeconds, 1))s"
