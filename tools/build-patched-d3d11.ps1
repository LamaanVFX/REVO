param(
  [Parameter(Mandatory)][string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = Join-Path $repo 'work/obs-source-32.1.2'
$obsSdk = Join-Path $repo 'work/obs-sdk-32.1.2'
$deps = Join-Path $repo 'work/validation-tools'
$llvm = (Get-ChildItem (Join-Path $deps 'llvm') -Directory | Select-Object -First 1).FullName
$compiler = Join-Path $llvm 'bin/clang++.exe'
$patch = Join-Path $repo 'patches/klip-dxgi-fallback.patch'
$duplicatorSource = Join-Path $source 'libobs-d3d11/d3d11-duplicator.cpp'

if (-not (Test-Path $duplicatorSource)) { throw "Pinned OBS source missing: $duplicatorSource" }
if (-not (Test-Path $patch)) { throw "DXGI fallback patch missing: $patch" }
if (-not (Test-Path $compiler)) { throw "Pinned clang++ missing: $compiler" }
if (-not (Test-Path (Join-Path $obsSdk 'lib/libobs.dll.a'))) { throw 'Pinned OBS import library missing; run tools/prepare-obs.ps1 first.' }

$sourceText = Get-Content -LiteralPath $duplicatorSource -Raw
if ($sourceText -notmatch 'falling back to DuplicateOutput') {
  & git -C $source apply --check $patch
  if ($LASTEXITCODE -ne 0) { throw 'DXGI fallback patch does not apply cleanly to the pinned OBS source.' }
  & git -C $source apply $patch
  if ($LASTEXITCODE -ne 0) { throw 'Could not apply DXGI fallback patch to the pinned OBS source.' }
}

$build = (Resolve-Path $BuildDirectory).Path
$bin = Join-Path $build 'bin/64bit'
if (-not (Test-Path $bin)) { throw "Build output directory missing: $bin" }
$output = Join-Path $bin 'libobs-d3d11.dll'

$cpp = @(
  'd3d11-duplicator.cpp',
  'd3d11-indexbuffer.cpp',
  'd3d11-rebuild.cpp',
  'd3d11-samplerstate.cpp',
  'd3d11-shader.cpp',
  'd3d11-shaderprocessor.cpp',
  'd3d11-stagesurf.cpp',
  'd3d11-subsystem.cpp',
  'd3d11-texture2d.cpp',
  'd3d11-texture3d.cpp',
  'd3d11-vertexbuffer.cpp',
  'd3d11-zstencilbuffer.cpp'
) | ForEach-Object { Join-Path (Join-Path $source 'libobs-d3d11') $_ }

$includeArgs = @(
  "-I$(Join-Path $source 'libobs')",
  "-I$(Join-Path $source 'libobs-d3d11')",
  "-I$(Join-Path $source 'libobs/graphics')",
  "-I$(Join-Path $source 'libobs/util')",
  "-I$(Join-Path $obsSdk 'include')"
)
$arguments = @(
  '-std=c++17',
  '-O2',
  '-shared',
  '-D_WIN32_WINNT=0x0A00',
  '-DWINVER=0x0A00',
  '-DWIN32_LEAN_AND_MEAN',
  '-DNOMINMAX'
) + $includeArgs + $cpp + @(
  (Join-Path $obsSdk 'lib/libobs.dll.a'),
  '-ld3d9',
  '-ld3d11',
  '-ld3dcompiler',
  '-ldxgi',
  '-lshcore',
  '-lole32',
  '-luuid',
  '-luser32',
  '-lgdi32',
  '-o',
  $output
)

Write-Output 'Building patched OBS Direct3D 11 backend using the pinned Klip #72 toolchain...'
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "Building patched libobs-d3d11.dll failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $output) -or (Get-Item $output).Length -lt 100000) { throw 'Patched libobs-d3d11.dll is missing or unexpectedly small.' }

$readobj = Join-Path $llvm 'bin/llvm-readobj.exe'
if (Test-Path $readobj) {
  & $readobj --coff-exports $output | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Patched backend failed PE export validation.' }
}
Write-Output "Patched DXGI backend built: $output ($((Get-Item $output).Length) bytes)"
