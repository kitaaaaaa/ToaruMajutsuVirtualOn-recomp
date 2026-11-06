# Clones bbcpu and bloodborne-recomp at the commits this port is based on and applies port\patches, on a
# local "windows" branch with the upstream remote named "upstream". Skips any that are already there.
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$forks = @(
    @{ Name = 'bbcpu'; Url = 'https://github.com/bloodborne-native/bbcpu.git'; Commit = '3f9b6c1a11d5a443ee2e556a0d1f3aec4a73f0ca' },
    @{ Name = 'bloodborne-recomp'; Url = 'https://github.com/bloodborne-native/bloodborne-recomp.git'; Commit = 'c60ccf8be752b8690f2813505c4daf9eafdb136b' }
)
foreach ($f in $forks) {
    $dir = "$port\$($f.Name)"
    if (Test-Path "$dir\.git") { "have $($f.Name)"; continue }
    git clone -q --origin upstream $f.Url $dir
    if ($LASTEXITCODE -ne 0) { "FAILED clone $($f.Name)"; exit 1 }
    git -C $dir checkout -q -b windows $f.Commit
    if ($LASTEXITCODE -ne 0) { "FAILED checkout $($f.Name)"; exit 1 }
    git -C $dir apply --whitespace=nowarn "$port\patches\$($f.Name)-windows.patch"
    if ($LASTEXITCODE -ne 0) { "FAILED patch $($f.Name)"; exit 1 }
    "ok $($f.Name)"
}
