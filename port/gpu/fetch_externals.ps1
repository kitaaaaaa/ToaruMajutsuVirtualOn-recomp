# Fetches the renderer's dependencies at the exact commits shadPS4 v0.21 pins, shallow, into port\gpu\externals,
# and a portable CMake into tools\cmake.
$ErrorActionPreference = 'Continue'
$gpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $gpu)
$shad = "$root\tools\src\shadPS4"
$out = "$gpu\externals"
New-Item -ItemType Directory -Force $out | Out-Null

$wanted = 'fmt', 'vulkan-headers', 'vma', 'glslang', 'robin-map', 'xbyak', 'magic_enum', 'toml11', 'sirit', 'xxhash', 'ext-boost', 'half', 'sdl3', 'imgui', 'tracy', 'spdlog', 'date', 'pugixml', 'json', 'zlib-ng', 'zstd'
foreach ($name in $wanted) {
    $path = "externals/$name"
    $url = (git -C $shad config -f .gitmodules "submodule.$path.url")
    if (-not $url) {
        $key = (git -C $shad config -f .gitmodules --get-regexp 'path$' | Where-Object { $_ -match " $([regex]::Escape($path))$" }) -replace '\.path .*$', '.url'
        if ($key) { $url = git -C $shad config -f .gitmodules $key }
    }
    $sha = (git -C $shad ls-tree HEAD $path) -replace '^\d+ commit (\w+)\s.*$', '$1'
    if (-not $url -or -not $sha) { "SKIP $name (url '$url' sha '$sha')"; continue }
    $dir = "$out\$name"
    if (Test-Path "$dir\.git") { "have $name"; continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    git -C $dir init -q
    git -C $dir remote add origin $url
    git -C $dir fetch -q --depth 1 origin $sha 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { "FAILED fetch $name from $url"; continue }
    git -C $dir -c advice.detachedHead=false checkout -q FETCH_HEAD
    "ok $name $($sha.Substring(0,10))"
}

$cmake = "$root\tools\cmake"
if (-not (Test-Path "$cmake\bin\cmake.exe")) {
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = 'Tls12'
    $zip = "$root\tools\downloads\cmake-4.4.5-windows-x86_64.zip"
    Invoke-WebRequest 'https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath "$root\tools\cmake-tmp" -Force
    Move-Item "$root\tools\cmake-tmp\cmake-4.4.5-windows-x86_64" $cmake
    Remove-Item "$root\tools\cmake-tmp" -Recurse
    "cmake installed"
}
& "$cmake\bin\cmake.exe" --version | Select-Object -First 1
"DONE"
