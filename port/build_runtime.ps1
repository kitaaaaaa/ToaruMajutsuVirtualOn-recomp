# Builds the Windows host (port\out\vox.exe) and prepares its inputs in port\work.
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$llvm = "$root\tools\llvm\bin"
$bbcpu = "$port\bbcpu"
$deps = "$port\deps"
$out = "$port\out"
$work = "$port\work"
$tools = "$port\bloodborne-recomp\tools\recomp"
$env:INCLUDE = $null

New-Item -ItemType Directory -Force "$out\obj", $work | Out-Null
python -I "$port\runtime\gen_names.py" "$root\tools\src\shadPS4\src\core\aerolib\aerolib.inl" "$work\names.tsv"
if ($LASTEXITCODE -ne 0) { 'FAILED generating names'; exit 1 }

$flags = @('-std=gnu11', '-O2', '-g', '-w', '-mcx16', '-D_CRT_RAND_S', '-DZYDIS_STATIC_BUILD', '-DZYCORE_STATIC_BUILD',
    '-include', "$bbcpu\src\win\win_compat.h", "-I$bbcpu\src\win\include", "-I$bbcpu\src", "-I$deps\include", "-I$port\runtime", "-I$port\gpu", "-I$port\gpu\externals\sdl3\include")
$L = "$port\gpu\build\lib"
$absl = 'raw_hash_set', 'cord', 'cordz_info', 'cord_internal', 'cordz_functions', 'cordz_handle', 'crc_cord_state', 'crc32c', 'crc_internal', 'crc_cpu_detect',
    'str_format_internal', 'hash', 'city', 'low_level_hash', 'hashtablez_sampler', 'exponential_biased', 'synchronization', 'stacktrace', 'graphcycles_internal',
    'kernel_timeout_internal', 'symbolize', 'malloc_internal', 'debugging_internal', 'demangle_internal', 'demangle_rust', 'decode_rust_punycode', 'utf8_for_code_point',
    'tracing_internal', 'time', 'strings', 'strings_internal', 'string_view', 'int128', 'base', 'spinlock_wait', 'civil_time', 'time_zone', 'throw_delegate',
    'raw_logging_internal', 'log_severity' | ForEach-Object { "$L\absl_$_.lib" }
$libs = @("$out\libbbcpu.lib", "$L\voxgpu.lib", "$L\fmt.lib", "$L\SDL3-static.lib", "$L\sirit.lib", "$L\miniz.lib", "$L\xxhash.lib", "$deps\lib\Zydis.lib", "$deps\lib\Zycore.lib") + $absl +
    @("$root\tools\llvm\lib\clang\23\lib\windows\clang_rt.builtins-x86_64.lib",
      '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lxinput', '-lhid', '-ldinput8', '-lmincore', '-lws2_32', '-lmswsock', '-lwinmm', '-limm32', '-lversion', '-lsetupapi', '-lole32', '-loleaut32', '-luuid', '-lgdi32',
      '-luser32', '-lshell32', '-ladvapi32', '-lcfgmgr32', '-lkernel32', '-lwinspool', '-lcomdlg32', '-loldnames')

$objects = @()
foreach ($c in Get-ChildItem "$port\runtime\*.c") {
    $o = "$out\obj\runtime_$($c.BaseName).obj"
    $output = & "$llvm\clang.exe" @flags -c $c.FullName -o $o 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        "FAILED $($c.Name)"
        $output | Where-Object { $_ -match 'error' } | Select-Object -First 15 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
        exit 1
    }
    $objects += $o
}
$shim = "$out\obj\runtime_import_shim.obj"
& "$llvm\clang.exe" -c "$port\runtime\import_shim.S" -o $shim
if ($LASTEXITCODE -ne 0) { exit 1 }
$objects += $shim

# 256 MiB host stack: recompiled guest calls nest on it.
$output = & "$llvm\clang++.exe" -fuse-ld=lld -g "-Wl,/STACK:268435456" @objects @libs -o "$out\vox.exe" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) {
    "FAILED link"
    $output | Where-Object { $_ -match 'error|undefined' } | Select-Object -First 20 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
    exit 1
}
"vox.exe built"
