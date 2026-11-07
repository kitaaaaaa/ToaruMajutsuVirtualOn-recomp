# Fetches the renderer's dependencies at the commits shadPS4 v0.21 pins, shallow, into port\gpu\externals.
$ErrorActionPreference = 'Continue'
$gpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = "$gpu\externals"
New-Item -ItemType Directory -Force $out | Out-Null

$pins = @(
    @('abseil-cpp', 'https://github.com/abseil/abseil-cpp.git', '76bb24329e8bf5f39704eb10d21b9a80befa7c81'),
    @('date', 'https://github.com/HowardHinnant/date.git', 'a45ea7c17b4a7f320e199b71436074bd624c9e15'),
    @('ext-boost', 'https://github.com/shadps4-emu/ext-boost.git', 'ca6f230e67be7cc45fc919057f07b2aee64dadc1'),
    @('fmt', 'https://github.com/shadps4-emu/ext-fmt.git', 'ec73fb72477d80926c758894a3ab2cb3994fd051'),
    @('glslang', 'https://github.com/KhronosGroup/glslang.git', 'ba1640446f3826a518721d1f083f3a8cca1120c3'),
    @('half', 'https://github.com/ROCm/half.git', '1ddada225144cac0de8f6b5c0dd9acffd99a2e68'),
    @('imgui', 'https://github.com/shadexternals/imgui.git', '7e1b65d26d52e9dd199d889c148c72184de647b4'),
    @('json', 'https://github.com/nlohmann/json.git', '55f93686c01528224f448c19128836e7df245f72'),
    @('magic_enum', 'https://github.com/Neargye/magic_enum.git', 'a413fcc9c46a020a746907136a384c227f3cd095'),
    @('miniz', 'https://github.com/richgel999/miniz', '174573d60290f447c13a2b1b3405de2b96e27d6c'),
    @('pugixml', 'https://github.com/zeux/pugixml.git', 'caade5a28aad86b92a4b5337a9dc70c4ba73c5eb'),
    @('robin-map', 'https://github.com/Tessil/robin-map.git', '4ec1bf19c6a96125ea22062f38c2cf5b958e448e'),
    @('sdl3', 'https://github.com/shadexternals/sdl3.git', 'd5af35e3fbb5bb6555ed00e69740d52af2a4e877'),
    @('sirit', 'https://github.com/shadps4-emu/sirit.git', 'fc2b42d5e57ee2f638676c0c02f7fd71934b596a'),
    @('sirit\externals\SPIRV-Headers', 'https://github.com/KhronosGroup/SPIRV-Headers', '2acb319af38d43be3ea76bfabf3998e5281d8d12'),
    @('spdlog', 'https://github.com/gabime/spdlog.git', 'b8944a4bcd478ee03375c9c50dc8d6c741f43f7b'),
    @('toml11', 'https://github.com/ToruNiina/toml11.git', 'a01fe3b4c14c6d7b99ee3f07c9e80058c6403097'),
    @('tracy', 'https://github.com/shadps4-emu/tracy.git', '143a53d1985b8e52a7590a0daca30a0a7c653b42'),
    @('vma', 'https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git', 'f378e7b3f18f6e2b06b957f6ba7b1c7207d2a536'),
    @('vulkan-headers', 'https://github.com/KhronosGroup/Vulkan-Headers.git', 'ee3b5caaa7e372715873c7b9c390ee1c3ca5db25'),
    @('xbyak', 'https://github.com/herumi/xbyak.git', '44a72f369268f7d552650891b296693e91db86bb'),
    @('xxhash', 'https://github.com/Cyan4973/xxHash.git', '953a09abc39096da9e216b6eb0002c681cdc1199'),
    @('zlib-ng', 'https://github.com/shadps4-emu/ext-zlib-ng.git', 'fd0d263cedab1a136f40d65199987e3eaeecfcbd'),
    @('zstd', 'https://github.com/shadexternals/zstd.git', '5c7b7bad26808e6b40ac3b3d0075466e27738a9d')
)
foreach ($pin in $pins) {
    $name, $url, $sha = $pin
    $dir = "$out\$name"
    if (Test-Path "$dir\.git") { continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    git -C $dir init -q
    git -C $dir remote add origin $url
    git -C $dir fetch -q --depth 1 origin $sha 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { "FAILED fetching $name from $url"; exit 1 }
    git -C $dir -c advice.detachedHead=false checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { "FAILED checking out $name"; exit 1 }
    "fetched $name $($sha.Substring(0, 10))"
}
'externals ready'
