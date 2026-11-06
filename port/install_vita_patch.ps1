# Builds a private English overlay from your own copy of a fan translation for the PS Vita version: a rePatch
# folder, or a .rar/.7z/.zip holding one. Play it with: port\run.ps1 -UserTl (vox.exe --user-tl)
# Text only: the Vita patch's textures aren't in the PS4's format. Nothing here is ever published, and it stays
# in git-ignored folders, because that translation is its authors' work.
# Args: -Patch <path>
param([Parameter(Mandatory = $true)][string]$Patch)
$ErrorActionPreference = 'Continue'
$port = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $port
$folder = $Patch
if (-not (Test-Path -LiteralPath $Patch)) { "No such file or folder: $Patch"; exit 1 }
if (-not (Get-Item -LiteralPath $Patch).PSIsContainer) {
    $sevenZip = @('C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $sevenZip) { 'Needs 7-Zip to read an archive. Extract it yourself and pass the folder instead.'; exit 1 }
    $folder = "$root\extracted\vita_patch"
    New-Item -ItemType Directory -Force $folder | Out-Null
    & $sevenZip x -y "-o$folder" $Patch '*.cpk' -r | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { 'Could not extract the archive.'; exit 1 }
}
$env:PYTHONIOENCODING = 'utf-8'
Push-Location "$root\analysis\scripts"
python -B tl_pack.py --vita $folder "$root\extracted\uroot\cpk" "$port\work\translation_user"
$code = $LASTEXITCODE
Pop-Location
if ($code -eq 0) { 'Installed. Play with: port\run.ps1 -UserTl' } else { 'Finished with problems, listed above.' }
exit $code
