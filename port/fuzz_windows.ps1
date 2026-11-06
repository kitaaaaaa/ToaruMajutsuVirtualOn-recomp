# Upstream's tools/recomp/fuzz.sh for Windows: every distinct instruction encoding in the game (at most
# 30 per mnemonic) runs through the interpreter and through its generated C; results must match.
# Args: [-Trials 200] [-Reports 60]. Run after build_windows.ps1 and the pipeline (port\work\eboot.elf).
param([int]$Trials = 200, [int]$Reports = 60)
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$llvm = "$root\tools\llvm\bin"
$bbcpu = "$port\bbcpu"
$recomp = "$port\bloodborne-recomp"
$deps = "$port\deps"
$out = "$port\out"
$work = "$port\work\recomp"
$env:INCLUDE = $null

$flags = @('-std=gnu11', '-O2', '-g', '-w', '-mcx16', '-DZYDIS_STATIC_BUILD', '-DZYCORE_STATIC_BUILD',
    '-include', "$bbcpu\src\win\win_compat.h", "-I$bbcpu\src\win\include", "-I$bbcpu\src", "-I$deps\include")
$libs = @("$out\libbbcpu.lib", "$deps\lib\Zydis.lib", "$deps\lib\Zycore.lib", "$root\tools\llvm\lib\clang\23\lib\windows\clang_rt.builtins-x86_64.lib")

function Check($what, $output) {
    if ($LASTEXITCODE -ne 0) {
        "FAILED $what"
        $output | Where-Object { $_ -match 'error|undefined' } | Select-Object -First 20 | ForEach-Object { $_.Substring(0, [Math]::Min(220, $_.Length)) }
        exit 1
    }
}

$o = & "$llvm\clang.exe" @flags -fuse-ld=lld "$bbcpu\tools\cpu\encodings.c" @libs -o "$out\encodings.exe" 2>&1 | ForEach-Object { "$_" }
Check 'encodings' $o
Set-Location "$port\work"
$o = cmd /c "`"$out\encodings.exe`" eboot.elf all 30 > recomp\encodings.txt 2>&1"
"encodings: " + (Get-Content recomp\encodings.txt | Measure-Object).Count + " lines"

$env:BB_JIT = '0'
Remove-Item Env:\BB_RECOMP_TABLES -ErrorAction SilentlyContinue
& "$out\bbrecomp.exe" --snippets recomp/encodings.txt recomp/snippets.c 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 2

$o = & "$llvm\clang.exe" -std=gnu11 -O1 -w -mcx16 -ffp-contract=off -fno-strict-aliasing -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD `
    -include "$bbcpu\src\win\win_compat.h" "-I$bbcpu\src\win\include" "-I$bbcpu\src" "-I$deps\include" -shared -fuse-ld=lld `
    "-Wl,/EXPORT:bb_recomp_init" recomp/snippets.c @libs -o recomp/libsnippets.dll 2>&1 | ForEach-Object { "$_" }
Check 'snippets' $o
"snippets dll built"

$o = & "$llvm\clang.exe" @flags -fuse-ld=lld "$recomp\tests\fuzz_recomp.c" @libs -o "$out\fuzz_recomp.exe" 2>&1 | ForEach-Object { "$_" }
Check 'fuzz_recomp' $o
"fuzz_recomp built, running"

& "$out\fuzz_recomp.exe" recomp/encodings.txt recomp/libsnippets.dll $Trials $Reports 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath recomp\fuzz.log | Select-Object -Last 5
"fuzz exit $LASTEXITCODE"
