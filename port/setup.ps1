# Fetches everything the build needs, at pinned versions, into tools\ and port\. Safe to run again: it skips
# what's already there. Needs Git, Python 3, and Visual Studio's "Desktop development with C++" (clang uses its
# C++ libraries and the Windows SDK). Takes about 10 GB.
# Args: [-Cache <folder of already-downloaded archives>]
param([string]$Cache = '')
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$tools = "$root\tools"
$downloads = "$tools\downloads"
New-Item -ItemType Directory -Force $downloads, "$tools\src" | Out-Null
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = 'Tls12'

foreach ($need in 'git', 'python') {
    if (-not (Get-Command $need -ErrorAction SilentlyContinue)) { "Needs $need on PATH."; exit 1 }
}

function Fetch($url, $name) {
    $path = "$downloads\$name"
    if (Test-Path $path) { return $path }
    if ($Cache -and (Test-Path "$Cache\$name")) {
        Copy-Item "$Cache\$name" $path
        return $path
    }
    "downloading $name"
    Invoke-WebRequest $url -OutFile "$path.part"
    Move-Item "$path.part" $path
    return $path
}

# Python's extractor: the system tar has hung on this archive, for an hour.
function Unpack-TarXz($archive, $into) {
    python -I -c "import sys,tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter='fully_trusted')" $archive $into
    if ($LASTEXITCODE -ne 0) { "FAILED unpacking $archive"; exit 1 }
}

if (-not (Test-Path "$tools\llvm\bin\clang.exe")) {
    $name = 'clang+llvm-23.1.5-x86_64-pc-windows-msvc'
    $archive = Fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/$name.tar.xz" "$name.tar.xz"
    'unpacking LLVM (several minutes)'
    Unpack-TarXz $archive $tools
    Move-Item "$tools\$name" "$tools\llvm"
}
"llvm: $(& "$tools\llvm\bin\clang.exe" --version | Select-Object -First 1)"

if (-not (Test-Path "$tools\cmake\bin\cmake.exe")) {
    $name = 'cmake-4.4.5-windows-x86_64'
    $archive = Fetch "https://github.com/Kitware/CMake/releases/download/v4.4.5/$name.zip" "$name.zip"
    Expand-Archive $archive -DestinationPath $tools -Force
    Move-Item "$tools\$name" "$tools\cmake"
}
"cmake: $(& "$tools\cmake\bin\cmake.exe" --version | Select-Object -First 1)"

if (-not (Test-Path "$tools\ninja\ninja.exe")) {
    $archive = Fetch 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' 'ninja-1.12.1-win.zip'
    Expand-Archive $archive -DestinationPath "$tools\ninja" -Force
}
"ninja: $(& "$tools\ninja\ninja.exe" --version)"

# A source tree at an exact commit, without its history.
function Pin($dir, $url, $commit) {
    if (Test-Path "$dir\.git") { return }
    New-Item -ItemType Directory -Force $dir | Out-Null
    git -C $dir init -q
    git -C $dir remote add origin $url
    git -C $dir fetch -q --depth 1 origin $commit 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { "FAILED fetching $url"; exit 1 }
    git -C $dir -c advice.detachedHead=false checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { "FAILED checking out $url"; exit 1 }
    "pinned $(Split-Path $dir -Leaf) at $($commit.Substring(0, 10))"
}

# shadPS4 v0.21.0: its symbol names table (aerolib.inl). Its renderer is already vendored in port\gpu\shadps4.
Pin "$tools\src\shadPS4" 'https://github.com/shadps4-emu/shadPS4.git' '11557f2eb60b3b00712c8887c30852de7615d6df'

# Zydis v4.1.1 and its Zycore, installed into port\deps.
if (-not ((Test-Path "$port\deps\lib\Zydis.lib") -and (Test-Path "$port\deps\lib\Zycore.lib") -and (Test-Path "$port\deps\include\Zycore\Defines.h"))) {
    $zydis = "$tools\src\zydis"
    Pin $zydis 'https://github.com/zyantific/zydis.git' 'a2278f1d254e492f6a6b39f6cb5d1f5d515659dc'
    git -C $zydis submodule update -q --init --depth 1
    if ($LASTEXITCODE -ne 0) { 'FAILED fetching Zycore'; exit 1 }
    $env:INCLUDE = $null # machine-wide INCLUDE variables can point at other compilers' headers
    $llvm = "$tools\llvm\bin" -replace '\\', '/'
    $install = "$port\deps" -replace '\\', '/'
    # Zycore first: installing Zydis doesn't install it, though Zydis's headers and users need it.
    foreach ($part in @(@('zycore', "$zydis\dependencies\zycore"), @('zydis', $zydis))) {
        $configure = & "$tools\cmake\bin\cmake.exe" -S $part[1] -B "$port\deps\build\$($part[0])" -G Ninja `
            "-DCMAKE_MAKE_PROGRAM=$($tools -replace '\\', '/')/ninja/ninja.exe" "-DCMAKE_C_COMPILER=$llvm/clang.exe" `
            "-DCMAKE_AR=$llvm/llvm-ar.exe" "-DCMAKE_RANLIB=$llvm/llvm-ranlib.exe" "-DCMAKE_RC_COMPILER=$llvm/llvm-rc.exe" `
            "-DCMAKE_MT=$llvm/llvm-mt.exe" '-DCMAKE_C_FLAGS=-fuse-ld=lld -D_CRT_SECURE_NO_WARNINGS' '-DCMAKE_BUILD_TYPE=Release' `
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
            "-DCMAKE_INSTALL_PREFIX=$install" '-DZYCORE_BUILD_SHARED_LIB=OFF' '-DZYCORE_BUILD_EXAMPLES=OFF' '-DZYCORE_BUILD_TESTS=OFF' `
            '-DZYDIS_BUILD_SHARED_LIB=OFF' '-DZYDIS_BUILD_TOOLS=OFF' '-DZYDIS_BUILD_EXAMPLES=OFF' `
            '-DZYDIS_BUILD_TESTS=OFF' '-DZYDIS_BUILD_DOXYGEN=OFF' 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { $configure | Select-Object -Last 15; "FAILED configuring $($part[0])"; exit 1 }
        $built = & "$tools\cmake\bin\cmake.exe" --build "$port\deps\build\$($part[0])" --target install 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { $built | Select-Object -Last 15; "FAILED building $($part[0])"; exit 1 }
    }
    'zydis and zycore built'
}

& "$port\fetch_forks.ps1"
if ($LASTEXITCODE -ne 0) { exit 1 }
& "$port\gpu\fetch_externals.ps1"
if ($LASTEXITCODE -ne 0) { exit 1 }
'setup done. Next: port\build_all.ps1'
