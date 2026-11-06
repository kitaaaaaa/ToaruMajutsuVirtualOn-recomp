# Compiles the generated C (port\work\recomp\gen\*.c) in parallel with the flags of upstream's recomp.sh.
# Args: [-Opt 2]
param([int]$Opt = 2)
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$clang = "$root\tools\llvm\bin\clang.exe"
$gen = "$port\work\recomp\gen"
$objDir = "$port\work\recomp\obj"
New-Item -ItemType Directory -Force $objDir | Out-Null
$env:INCLUDE = $null

$jobs = [Math]::Max(2, [int]$env:NUMBER_OF_PROCESSORS - 2)
$running = @()
$failed = @()
$sw = [Diagnostics.Stopwatch]::StartNew()
foreach ($c in Get-ChildItem "$gen\*.c" | Sort-Object Length -Descending) {
    while (@($running | Where-Object { -not $_.HasExited }).Count -ge $jobs) { Start-Sleep -Milliseconds 300 }
    $o = "$objDir\$($c.BaseName).obj"
    $log = "$objDir\$($c.BaseName).log"
    $args = @('-std=gnu11', "-O$Opt", '-w', '-mcx16', '-ffp-contract=off', '-fno-strict-aliasing',
        '-DZYDIS_STATIC_BUILD', '-DZYCORE_STATIC_BUILD',
        '-include', "`"$port\bbcpu\src\win\win_compat.h`"", "`"-I$port\bbcpu\src\win\include`"", "`"-I$port\bbcpu\src`"", "`"-I$port\deps\include`"",
        '-c', "`"$($c.FullName)`"", '-o', "`"$o`"")
    $p = Start-Process -FilePath $clang -ArgumentList $args -NoNewWindow -PassThru -RedirectStandardError $log
    $null = $p.Handle
    $running += [pscustomobject]@{ Process = $p; Source = $c.Name; Log = $log; HasExited = $false } | Add-Member -MemberType ScriptProperty -Name HasExited -Value { $this.Process.HasExited } -Force -PassThru
}
foreach ($r in $running) { $r.Process.WaitForExit() }
foreach ($r in $running) {
    if ($r.Process.ExitCode -ne 0) {
        $failed += $r.Source
        "FAILED $($r.Source) (exit $($r.Process.ExitCode))"
        Get-Content $r.Log | Where-Object { $_ -match 'error' } | Select-Object -First 8 | ForEach-Object { $_.Substring(0, [Math]::Min(220, $_.Length)) }
    }
}
$objs = Get-ChildItem "$objDir\*.obj" -ErrorAction SilentlyContinue
"jobs=$jobs seconds=$([Math]::Round($sw.Elapsed.TotalSeconds)) compiled=$(@($objs).Count) failed=$($failed.Count) objMB=$([Math]::Round((($objs | Measure-Object Length -Sum).Sum)/1MB))"
if ($failed.Count) { exit 1 }
