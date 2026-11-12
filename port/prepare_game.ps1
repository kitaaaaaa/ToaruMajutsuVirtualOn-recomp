# Turns your own copy of the game into something port\run.ps1 can play: extracts it, finds its functions,
# recompiles them to port\work\recomp\librecomp.dll, and builds the translation overlays. Everything it produces
# comes from the game: it all stays in git-ignored folders. Run port\build_all.ps1 first.
#
# Args: -Pkg <fake-signed .pkg> | -Game <folder with eboot.bin>   (or neither, if extracted\uroot already exists)
#       [-Tables <known_tables.tsv>]  optional jump tables, for forms bbrecomp can't detect by itself
param([string]$Pkg = '', [string]$Game = '', [string]$Tables = '')
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$work = "$port\work"
$recomp = "$work\recomp"
$out = "$port\out"
$llvm = "$root\tools\llvm\bin"
$uroot = "$root\extracted\uroot"
$elfTool = "$port\bloodborne-recomp\tools\recomp\ps4elf.py"
$env:INCLUDE = $null
$ProgressPreference = 'SilentlyContinue'
$sw = [Diagnostics.Stopwatch]::StartNew()

if (-not (Test-Path "$out\bbrecomp.exe")) { 'Run port\build_all.ps1 first.'; exit 1 }

if ($Pkg) {
    if (Test-Path "$uroot\eboot.bin") {
        "already extracted: $uroot"
    } else {
        $tool = "$root\tools\PkgTool\PkgTool.exe"
        if (-not (Test-Path $tool)) {
            [Net.ServicePointManager]::SecurityProtocol = 'Tls12'
            New-Item -ItemType Directory -Force "$root\tools\downloads" | Out-Null
            $zip = "$root\tools\downloads\PkgTool-0.2.231.zip"
            if (-not (Test-Path $zip)) { Invoke-WebRequest 'https://github.com/maxton/LibOrbisPkg/releases/download/v0.2/PkgTool-0.2.231.zip' -OutFile $zip }
            Expand-Archive $zip -DestinationPath "$root\tools\PkgTool" -Force
        }
        'extracting the pkg (several minutes)'
        & $tool pkg_extract --passcode 00000000000000000000000000000000 $Pkg "$root\extracted" | Select-Object -Last 2
        if (-not (Test-Path "$uroot\eboot.bin")) { 'FAILED extracting the pkg'; exit 1 }
    }
} elseif ($Game) {
    if (-not (Test-Path "$Game\eboot.bin")) { "No eboot.bin in $Game"; exit 1 }
    if (-not (Test-Path $uroot)) {
        New-Item -ItemType Directory -Force "$root\extracted" | Out-Null
        cmd /c mklink /J "$uroot" "$Game" | Out-Null
    }
}
if (-not (Test-Path "$uroot\eboot.bin")) { 'Pass -Pkg or -Game, or put the game in extracted\uroot.'; exit 1 }

New-Item -ItemType Directory -Force "$recomp\gen", "$recomp\obj" | Out-Null
'--- plain ELFs'
foreach ($pair in @(@("$uroot\eboot.bin", "$work\eboot.elf"), @("$uroot\sce_module\libc.prx", "$work\libc.elf"), @("$uroot\sce_module\libSceFios2.prx", "$work\libSceFios2.elf"))) {
    if (-not (Test-Path $pair[1])) {
        python -I $elfTool $pair[0] $pair[1]
        if ($LASTEXITCODE -ne 0) { "FAILED converting $($pair[0])"; exit 1 }
    }
}

Set-Location $work
'--- finding functions'
& "$out\scan.exe" eboot.elf recomp 2>&1 | Select-Object -Last 2 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$recomp\functions.tsv")) { 'FAILED scanning the game'; exit 1 }
Get-Content "$recomp\functions.tsv" | Select-Object -Skip 1 | ForEach-Object { ($_ -split "`t")[0] } | Out-File -Encoding ascii "$recomp\all.txt"
"$((Get-Content "$recomp\all.txt").Count) functions"
python -I "$port\bloodborne-recomp\tools\recomp\imports.py" "$uroot\eboot.bin" $recomp
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$recomp\imports.tsv")) { 'FAILED listing imports'; exit 1 }

'--- writing C'
Remove-Item "$recomp\gen\*.c", "$recomp\obj\*" -ErrorAction SilentlyContinue
$env:BB_JIT = '0'
if ($Tables) { $env:BB_RECOMP_TABLES = $Tables } else { Remove-Item Env:BB_RECOMP_TABLES -ErrorAction SilentlyContinue }
& "$out\bbrecomp.exe" eboot.elf recomp/functions.tsv recomp/gen/recomp.c '@recomp/all.txt' 2>&1 | Select-Object -Last 3 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { 'FAILED generating C'; exit 1 }

'--- compiling (the longest step)'
& "$port\compile_generated.ps1"
if ($LASTEXITCODE -ne 0) { exit 1 }

'--- linking librecomp.dll'
Get-ChildItem "$recomp\obj\*.obj" | ForEach-Object { '"' + ($_.FullName -replace '\\', '/') + '"' } | Out-File -Encoding ascii "$recomp\link.rsp"
Set-Location $recomp
$libs = @("$out\libbbcpu.lib", "$port\deps\lib\Zydis.lib", "$port\deps\lib\Zycore.lib", "$root\tools\llvm\lib\clang\23\lib\windows\clang_rt.builtins-x86_64.lib")
$output = & "$llvm\clang.exe" -shared -fuse-ld=lld -g0 '-Wl,/EXPORT:bb_recomp_init' '-Wl,/MAP:librecomp.map' '@link.rsp' @libs -o librecomp.dll 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { $output | Select-Object -Last 10; 'FAILED linking'; exit 1 }

'--- the -Engrish overlay, and the -English one if translation_english exists'
$env:PYTHONIOENCODING = 'utf-8'
Set-Location "$root\analysis\scripts"
python -B tl_pack.py "$root\translation" "$uroot\cpk" "$work\translation" | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { 'FAILED packing the translation'; exit 1 }
if (Test-Path "$root\translation_english") {
    python -B tl_pack.py "$root\translation_english" "$uroot\cpk" "$work\translation_english" | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { 'FAILED packing the English translation'; exit 1 }
}

"done in $([Math]::Round($sw.Elapsed.TotalMinutes, 1)) minutes. Play: port\run.ps1 -Timeout 3600 -NoBuild -English"
