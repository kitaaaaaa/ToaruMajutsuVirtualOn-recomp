# Builds the engine: the renderer, bbcpu and the recompiler's tools, and vox.exe. No game needed.
# Run port\setup.ps1 first. The first build takes a while; later ones only rebuild what changed.
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$sw = [Diagnostics.Stopwatch]::StartNew()
foreach ($step in @(@("$port\gpu\build.ps1", @{ Target = 'voxgpu' }), @("$port\build_windows.ps1", @{}), @("$port\build_runtime.ps1", @{}))) {
    "--- $(Split-Path $step[0] -Leaf)"
    $arguments = $step[1]
    & $step[0] @arguments
    if ($LASTEXITCODE -ne 0) { "FAILED: $($step[0])"; exit 1 }
}
"engine built in $([Math]::Round($sw.Elapsed.TotalMinutes, 1)) minutes: port\out\vox.exe. Next: port\prepare_game.ps1"
