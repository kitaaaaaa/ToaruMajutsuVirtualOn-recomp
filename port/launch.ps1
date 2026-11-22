# One-click launcher, started by Launch.bat. The first time, it runs setup.ps1, build_all.ps1 and
# prepare_game.ps1, asking for the game if extracted\uroot doesn't have it yet, then deletes whatever tools and
# sources those scripts downloaded during this run. After that it just starts the game.
# Args: passed on to vox.exe, e.g. --english
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$vox = "$port\out\vox.exe"
$lib = "$port\work\recomp\librecomp.dll"

function Step($script, $arguments) {
    "=== $(Split-Path $script -Leaf)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $script @arguments
    if ($LASTEXITCODE -ne 0) {
        "FAILED: $(Split-Path $script -Leaf). Downloads are kept, so running Launch.bat again carries on from here."
        Read-Host 'Press Enter to close'
        exit 1
    }
}

function Fetch($url, $name) {
    $path = "$root\tools\downloads\$name"
    if (-not (Test-Path $path)) {
        Write-Host "downloading $name"
        New-Item -ItemType Directory -Force "$root\tools\downloads" | Out-Null
        Invoke-WebRequest $url -OutFile "$path.part"
        Move-Item "$path.part" $path
    }
    return $path
}

if (-not ((Test-Path $vox) -and (Test-Path $lib))) {
    # What the scripts download. Only the ones missing on the first try are deleted at the end; the list is kept
    # in a file so a run resumed after a failure still deletes them.
    $list = "$port\launch_cleanup.txt"
    if (Test-Path $list) {
        $downloaded = @(Get-Content $list)
    } else {
        $downloaded = @("$root\tools\llvm", "$root\tools\cmake", "$root\tools\ninja", "$root\tools\downloads",
            "$root\tools\src", "$root\tools\PkgTool", "$port\deps", "$port\bbcpu", "$port\bloodborne-recomp",
            "$port\gpu\externals", "$root\tools\python", "$root\tools\git", "$root\tools\xwin", "$root\tools\msvc") |
            Where-Object { -not (Test-Path $_) }
        Set-Content $list $downloaded
    }
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = 'Tls12'

    # Python 3.12+ (setup.ps1 unpacks with tarfile's filter argument). The Store's python.exe stub fails this too.
    $python = "$root\tools\python"
    if (Test-Path "$python\python.exe") { $env:PATH = "$python;$env:PATH" }
    $hasPython = $false
    if (Get-Command python -ErrorAction SilentlyContinue) {
        python -c "import sys; sys.exit(sys.version_info < (3, 12))" 2>$null
        $hasPython = $LASTEXITCODE -eq 0
    }
    if (-not $hasPython) {
        Expand-Archive (Fetch 'https://www.python.org/ftp/python/3.13.1/python-3.13.1-embed-amd64.zip' 'python-3.13.1-embed-amd64.zip') $python -Force
        # The embedded build only searches what its ._pth lists, not the script's own folder.
        Add-Content "$python\python313._pth" "$root\analysis\scripts", "$port\runtime", "$port\bloodborne-recomp\tools\recomp"
        $env:PATH = "$python;$env:PATH"
    }

    $git = "$root\tools\git"
    if (Test-Path "$git\cmd\git.exe") { $env:PATH = "$git\cmd;$env:PATH" }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Expand-Archive (Fetch 'https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip' 'MinGit-2.47.1-64-bit.zip') $git -Force
        $env:PATH = "$git\cmd;$env:PATH"
    }

    # Without Visual Studio, Microsoft's C/C++ headers and libraries come from xwin, after the builder accepts
    # Microsoft's license at its prompt. Clang reads them from INCLUDE and lld-link from LIB.
    # To replace later: llvm-mingw's open-source headers and libraries would need no Microsoft files or license.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $hasVs = (Test-Path $vswhere) -and (& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $hasVs) {
        $msvc = "$root\tools\msvc"
        if (-not (Test-Path "$msvc\crt\include")) {
            $name = 'xwin-0.10.0-x86_64-pc-windows-msvc'
            $archive = Fetch "https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/$name.tar.gz" "$name.tar.gz"
            New-Item -ItemType Directory -Force "$root\tools\xwin" | Out-Null
            tar -xzf $archive -C "$root\tools\xwin"
            & "$root\tools\xwin\$name\xwin.exe" --cache-dir "$root\tools\downloads\xwin" --arch x86_64 splat --output $msvc --disable-symlinks --include-debug-libs
            if ($LASTEXITCODE -ne 0) { 'FAILED fetching the Microsoft headers and libraries'; Read-Host 'Press Enter to close'; exit 1 }
        }
        $env:VOX_INCLUDE = "$msvc\crt\include;$msvc\sdk\include\ucrt;$msvc\sdk\include\um;$msvc\sdk\include\shared;$msvc\sdk\include\winrt"
        $env:LIB = "$msvc\crt\lib\x86_64;$msvc\sdk\lib\um\x86_64;$msvc\sdk\lib\ucrt\x86_64"
    }

    $game = @()
    if (-not (Test-Path "$root\extracted\uroot\eboot.bin")) {
        Add-Type -AssemblyName System.Windows.Forms
        $pick = New-Object System.Windows.Forms.OpenFileDialog
        $pick.Title = 'Your copy of the game: a fake-signed .pkg, or eboot.bin in an extracted folder'
        $pick.Filter = 'PS4 game (*.pkg, eboot.bin)|*.pkg;eboot.bin'
        if ($pick.ShowDialog() -ne 'OK') { 'No game chosen.'; exit 1 }
        $game = if ($pick.FileName -like '*.pkg') { @('-Pkg', $pick.FileName) } else { @('-Game', (Split-Path -Parent $pick.FileName)) }
    }

    Step "$port\setup.ps1" @()
    Step "$port\build_all.ps1" @()
    Step "$port\prepare_game.ps1" $game

    foreach ($dir in $downloaded) {
        if (Test-Path $dir) { "removing $dir"; Remove-Item -Recurse -Force $dir }
    }
    Remove-Item $list
}

& $vox @args
