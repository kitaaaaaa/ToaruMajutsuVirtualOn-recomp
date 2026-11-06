# Builds bbcpu and the recompiler's tools for Windows with the project's clang.
# Out: port\out\libbbcpu.lib, hello.exe, test_win_sjlj.exe, scan.exe, bbrecomp.exe
$ErrorActionPreference = 'Continue'
$port =Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$llvm = "$root\tools\llvm\bin"
$bbcpu = "$port\bbcpu"
$recomp = "$port\bloodborne-recomp"
$deps = "$port\deps"
$out = "$port\out"
$obj = "$out\obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# The machine-wide INCLUDE points at Watcom's headers: this process must not see it.
$env:INCLUDE = $null

$flags = @('-std=gnu11', '-O2', '-g', '-w', '-mcx16', '-DZYDIS_STATIC_BUILD', '-DZYCORE_STATIC_BUILD',
    '-include', "$bbcpu\src\win\win_compat.h", "-I$bbcpu\src\win\include", "-I$bbcpu\src", "-I$deps\include")
$libs = @("$deps\lib\Zydis.lib", "$deps\lib\Zycore.lib", "$root\tools\llvm\lib\clang\23\lib\windows\clang_rt.builtins-x86_64.lib")
$failed = $false

function Compile($source, $object) {
    if ($source.EndsWith('.S')) {
        $output = & "$llvm\clang.exe" -c $source -o $object 2>&1 | ForEach-Object { "$_" }
    } else {
        $output = & "$llvm\clang.exe" @flags -c $source -o $object 2>&1 | ForEach-Object { "$_" }
    }
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $true
        "FAILED $source"
        $output | Where-Object { $_ -match 'error' } | Select-Object -First 12
    }
}

$sources = @('cpu.c', 'decode.c', 'interp.c', 'interp_vec.c', 'interp_x87.c', 'jit_arm64.c', 'native.c', 'recomp.c', 'record.c', 'hostcall.S') |
    ForEach-Object { "$bbcpu\src\cpu\$_" }
$sources += "$bbcpu\src\win\win_compat.c", "$bbcpu\src\win\win_sjlj.S"
$objects = @()
foreach ($s in $sources) {
    $o = "$obj\" + (Split-Path $s -Leaf) + '.obj'
    Compile $s $o
    $objects += $o
}
if ($failed) { exit 1 }
if (Test-Path "$out\libbbcpu.lib") { Remove-Item "$out\libbbcpu.lib" }
& "$llvm\llvm-ar.exe" rcs "$out\libbbcpu.lib" @objects
"libbbcpu.lib built"

function Link($name, $sources) {
    $output = & "$llvm\clang.exe" @flags -fuse-ld=lld @sources "$out\libbbcpu.lib" @libs -o "$out\$name.exe" 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $true
        "FAILED $name"
        $output | Where-Object { $_ -match 'error|undefined' } | Select-Object -First 25
    } else { "$name.exe built" }
}

Link 'test_win_sjlj' @("$bbcpu\tests\test_win_sjlj.c")
Link 'hello' @("$bbcpu\examples\hello.c")
Link 'test_win_hardware' @("$bbcpu\tests\test_win_hardware.c")
Link 'scan' @("$recomp\tools\recomp\scan.c")
Link 'bbrecomp' @("$recomp\tools\recomp\bbrecomp.c")
if ($failed) { exit 1 }
