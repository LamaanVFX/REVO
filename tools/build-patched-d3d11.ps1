param(
  [Parameter(Mandatory)][string]$OutputDirectory
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

foreach ($required in @($duplicatorSource, $patch, $compiler, (Join-Path $obsSdk 'lib/libobs.dll.a'))) {
  if (-not (Test-Path $required)) { throw "Required input missing: $required" }
}

# Re-apply only when the expected code isn't present. This script runs for both
# Debug and Release builds, against the same pinned OBS source checkout.
$sourceText = Get-Content -LiteralPath $duplicatorSource -Raw
if ($sourceText -notmatch 'falling back to DuplicateOutput') {
  & git -C $source apply --check $patch
  if ($LASTEXITCODE -ne 0) { throw 'DXGI fallback patch does not apply cleanly to pinned OBS 32.1.2.' }
  & git -C $source apply $patch
  if ($LASTEXITCODE -ne 0) { throw 'Could not apply DXGI fallback patch to pinned OBS 32.1.2.' }
}

$bin = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$output = Join-Path $bin 'libobs-d3d11.dll'
$configDir = Join-Path $repo 'work/patched-d3d11-config'
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
@'
#pragma once
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_INSTALL_PREFIX "."
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -LiteralPath (Join-Path $configDir 'obsconfig.h') -Encoding ascii

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

# Only OBS's optional GPU-priority/HAGS diagnostics depend on the WDK-only
# d3dkmthk.h header. The capture backend does not need those diagnostics, so
# disable those optional blocks while retaining all actual D3D11 capture code.
$subsystemPath = Join-Path $source 'libobs-d3d11/d3d11-subsystem.cpp'
$subsystemText = Get-Content -LiteralPath $subsystemPath -Raw
if ($subsystemText.Contains('#include <d3dkmthk.h>')) {
  $subsystemText = [regex]::Replace($subsystemText, '(?m)^#include <d3dkmthk\.h>\r?\n', '')
  $subsystemText = [regex]::Replace($subsystemText, '(?ms)^#ifdef USE_GPU_PRIORITY\r?\n.*?^#endif\r?\n', '')
  $hagsInitStart = $subsystemText.IndexOf('/* Log HAGS status */')
  if ($hagsInitStart -lt 0) { throw 'Expected OBS HAGS initialization diagnostics were not found.' }
  $hagsInitEnd = $subsystemText.IndexOf('/* ---------------------------------------- */', $hagsInitStart)
  if ($hagsInitEnd -lt 0) { throw 'Could not identify the end of OBS HAGS initialization diagnostics.' }
  $subsystemText = $subsystemText.Remove($hagsInitStart, $hagsInitEnd - $hagsInitStart)
  $hagsStart = $subsystemText.IndexOf('struct HagsStatus {')
  if ($hagsStart -lt 0) { throw 'Expected OBS HAGS diagnostics block was not found.' }
  $checkStart = $subsystemText.IndexOf('static bool CheckFormat(', $hagsStart)
  if ($checkStart -lt 0) { throw 'Could not identify the end of the optional OBS HAGS diagnostics block.' }
  $subsystemText = $subsystemText.Remove($hagsStart, $checkStart - $hagsStart)
  $hagsLogPattern = '(?s)\s*if \(auto hags_support = GetAdapterHagsStatus\(&desc\)\) \{\s*blog\(LOG_INFO, "\\t  HAGS Status:    %s", hags_support->ToString\(\)\.c_str\(\)\);\s*\} else \{\s*blog\(LOG_WARNING, "\\t  HAGS Status:    Unknown"\);\s*\}'
  $withoutHagsLog = [regex]::Replace($subsystemText, $hagsLogPattern, '', 1)
  if ($withoutHagsLog -eq $subsystemText -or $withoutHagsLog.Contains('GetAdapterHagsStatus(&desc)')) {
    throw 'Could not safely remove the optional OBS HAGS diagnostic call.'
  }
  $subsystemText = $withoutHagsLog
  Set-Content -LiteralPath $subsystemPath -Value $subsystemText -Encoding utf8NoBOM
}
if ($subsystemText.Contains('d3dkmthk.h') -or $subsystemText.Contains('GetAdapterHagsStatus(&desc)')) {
  throw 'WDK-dependent diagnostic code remains in D3D11 backend source.'
}
if (-not $subsystemText.Contains('#include <errno.h>')) {
  $subsystemText = $subsystemText.Replace('#include <algorithm>', "#include <algorithm>`r`n#include <errno.h>")
  Set-Content -LiteralPath $subsystemPath -Value $subsystemText -Encoding utf8NoBOM
}
if ($subsystemText.Contains('ETIMEDOUT') -and -not $subsystemText.Contains('#include <errno.h>')) {
  throw 'ETIMEDOUT is used but errno.h was not included.'
}

$shaderPath = Join-Path $source 'libobs-d3d11/d3d11-shader.cpp'
$shaderText = Get-Content -LiteralPath $shaderPath -Raw
if ($shaderText.Contains('throw std::exception("Checksum mismatch");')) {
  $shaderText = $shaderText.Replace('throw std::exception("Checksum mismatch");', 'throw std::runtime_error("Checksum mismatch");')
  if (-not $shaderText.Contains('#include <stdexcept>')) {
    $shaderText = $shaderText.Replace('#include <fstream>', "#include <fstream>`r`n#include <stdexcept>")
  }
  Set-Content -LiteralPath $shaderPath -Value $shaderText -Encoding utf8NoBOM
} elseif (-not $shaderText.Contains('throw std::runtime_error("Checksum mismatch");')) {
  throw 'Expected OBS shader-cache checksum exception was not found.'
}

$includeArgs = @(
  "-I$configDir",
  "-I$(Join-Path $source 'libobs')",
  "-I$(Join-Path $source 'libobs-d3d11')",
  "-I$(Join-Path $source 'libobs/graphics')",
  "-I$(Join-Path $source 'libobs/util')",
  "-I$(Join-Path $obsSdk 'include')",
  '-isystem',
  ((Get-ChildItem (Join-Path $deps 'winsdk/c/Include') -Directory | Select-Object -First 1).FullName + '/cppwinrt')
)
$arguments = @(
  '-std=c++20',
  '-O2',
  '-shared',
  '-D_WIN32_WINNT=0x0A00',
  '-DWINVER=0x0A00',
  '-DUNICODE',
  '-D_UNICODE',
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

Write-Output 'Compiling patched Direct3D 11 backend against the pinned OBS 32.1.2 runtime...'
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "Building patched libobs-d3d11.dll failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $output) -or (Get-Item $output).Length -lt 100000) { throw 'Patched libobs-d3d11.dll is missing or unexpectedly small.' }

$readobj = Join-Path $llvm 'bin/llvm-readobj.exe'
& $readobj --file-headers --coff-exports $output | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Patched backend failed PE/export validation.' }

Write-Output "Patched backend built: $output ($((Get-Item $output).Length) bytes)"
