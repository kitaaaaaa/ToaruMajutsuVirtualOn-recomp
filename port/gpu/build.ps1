# Builds port\gpu\build\lib\voxgpu.lib, its dependency libraries and build\bin\voxgpu_test.exe.
# Usage: build.ps1 [-Target <ninja target>] [-Jobs <n>]
param([string]$Target = 'all', [int]$Jobs = 0)
$ErrorActionPreference = 'Continue'
$gpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $gpu)
$llvm = "$root\tools\llvm\bin" -replace '\\', '/'
$cmake = "$root\tools\cmake\bin\cmake.exe"
$ninja = "$root\tools\ninja\ninja.exe"
$build = "$gpu\build"

# Machine-wide it points at Watcom's headers, which break the MSVC ones.
$env:INCLUDE = $null

if (-not (Test-Path "$build\build.ninja")) {
    & $cmake -S $gpu -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')" `
        "-DCMAKE_C_COMPILER=$llvm/clang.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang++.exe" `
        "-DCMAKE_AR=$llvm/llvm-ar.exe" "-DCMAKE_RANLIB=$llvm/llvm-ranlib.exe" `
        "-DCMAKE_RC_COMPILER=$llvm/llvm-rc.exe" "-DCMAKE_MT=$llvm/llvm-mt.exe" `
        '-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld' '-DCMAKE_BUILD_TYPE=RelWithDebInfo' 2>&1 |
        Where-Object { $_ -match 'CMake Error|CMake Warning|Configuring|Generating' }
    if ($LASTEXITCODE -ne 0) { 'configure failed'; exit 1 }
}

$jobArgs = @()
if ($Jobs -gt 0) { $jobArgs = @('-j', $Jobs) }
& $ninja -C $build -k 0 @jobArgs $Target 2>&1 | Where-Object { $_ -match 'error:|FAILED|fatal|undefined symbol|ninja: ' } | ForEach-Object { "$_".Substring(0, [Math]::Min(400, "$_".Length)) }
if ($LASTEXITCODE -ne 0) { 'build failed'; exit 1 }
'build ok'
