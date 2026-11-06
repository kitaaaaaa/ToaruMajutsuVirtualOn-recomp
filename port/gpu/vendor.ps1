# Copies the renderer's sources from the pristine shadPS4 clone (v0.21.0) into port\gpu\shadps4, and bbgpu's
# small replacement headers into port\gpu\shim (edited there afterwards). Run once; never overwrites.
$gpu = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $gpu)
$shad = "$root\tools\src\shadPS4\src"
$bb = "$root\tools\src\bloodborne-runtime\gpu\shim"
$dst = "$gpu\shadps4"
if (Test-Path $dst) { "shadps4 already vendored"; exit 0 }
foreach ($d in 'video_core', 'shader_recompiler', 'common') { Copy-Item "$shad\$d" "$dst\$d" -Recurse }
$lib = "core\libraries"
foreach ($d in "gnmdriver", "videoout") { Copy-Item "$shad\$lib\$d" "$dst\$lib\$d" -Recurse -Force -ErrorAction SilentlyContinue; if (-not (Test-Path "$dst\$lib\$d")) { New-Item -ItemType Directory -Force (Split-Path "$dst\$lib\$d") | Out-Null; Copy-Item "$shad\$lib\$d" "$dst\$lib\$d" -Recurse } }
New-Item -ItemType Directory -Force "$dst\$lib\kernel", "$dst\$lib\system", "$dst\core" | Out-Null
foreach ($f in 'orbis_error.h', 'posix_error.h', 'time.h', 'process.h', 'equeue.h', 'equeue.cpp') { Copy-Item "$shad\$lib\kernel\$f" "$dst\$lib\kernel\$f" }
Copy-Item "$shad\$lib\error_codes.h" "$dst\$lib\error_codes.h"
Copy-Item "$shad\core\platform.h" "$dst\core\platform.h"
foreach ($f in 'userservice.h', 'systemservice.h') { Copy-Item "$shad\$lib\system\$f" "$dst\$lib\system\$f" }
New-Item -ItemType Directory -Force "$gpu\shim" | Out-Null
foreach ($d in 'core', 'tracy', 'imgui') { Copy-Item "$bb\$d" "$gpu\shim\$d" -Recurse }
New-Item -ItemType Directory -Force "$gpu\shim\common\logging" | Out-Null
foreach ($f in 'common\assert.cpp', 'common\debug.h', 'common\logging\log.cpp', 'common\logging\log.h', 'sdl_window.h', 'window.cpp') { Copy-Item "$bb\$f" "$gpu\shim\$f" }
Copy-Item "$bb\bbgpu.cpp" "$gpu\shim\voxgpu_from_bbgpu.cpp"
"vendored: " + (Get-ChildItem -Recurse -File $dst | Measure-Object).Count + " files"
