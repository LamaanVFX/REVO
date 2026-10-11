param(
  [ValidateSet('Debug','Release')][string]$Configuration='Release',
  [ValidatePattern('^[a-zA-Z0-9_-]{0,50}$')][string]$BuildSuffix='native-experiment'
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Build the native WGC/D3D11/FFmpeg path separately from the shipping libobs build.
& "$PSScriptRoot/bootstrap-validation.ps1"
if($LASTEXITCODE){exit $LASTEXITCODE}

$deps=Join-Path $repo 'work/validation-tools'
$llvm=(Get-ChildItem "$deps/llvm" -Directory | Select-Object -First 1).FullName
$cmake=(Get-ChildItem "$deps/cmake" -Directory | Select-Object -First 1).FullName
$imgui=(Get-ChildItem "$deps/imgui-1.92.8" -Directory | Select-Object -First 1).FullName
$sdk=(Get-ChildItem "$deps/winsdk/c/Include" -Directory | Select-Object -First 1).FullName

$ffmpegHeader=Get-ChildItem "$deps/ffmpeg-8.1" -Recurse -File -Filter 'avcodec.h' |
  Where-Object { $_.FullName -match '[\\/]include[\\/]libavcodec[\\/]avcodec\.h$' } |
  Select-Object -First 1
if(-not $ffmpegHeader){throw 'Pinned FFmpeg headers were not found after bootstrap.'}
$ffmpegRoot=Split-Path (Split-Path (Split-Path $ffmpegHeader.FullName -Parent) -Parent) -Parent
if(-not (Test-Path (Join-Path $ffmpegRoot 'include/libavcodec/avcodec.h'))){throw "Could not resolve FFmpeg root: $ffmpegRoot"}

$previousPath=$env:PATH
try {
  $env:PATH="$llvm/bin;$cmake/bin;$deps/ninja;$env:PATH"
  $build=Join-Path $repo ("build-native-$Configuration" + $(if($BuildSuffix){"-$BuildSuffix"}else{''}))
  & cmake -S $repo -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_CXX_COMPILER=$llvm/bin/clang++.exe" `
    "-DCMAKE_RC_COMPILER=$llvm/bin/llvm-windres.exe" `
    "-DCMAKE_CXX_FLAGS=-isystem $($sdk.Replace('\','/'))/cppwinrt" `
    "-Dimgui_DIR=$repo/cmake/validation-imgui" `
    "-DKLIP_IMGUI_SOURCE=$imgui" `
    "-DFFMPEG_ROOT=$ffmpegRoot" `
    '-DKLIP_USE_LIBOBS=OFF' `
    '-DKLIP_BUILD_APP=ON' `
    '-DKLIP_BUILD_TESTS=ON'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & cmake --build $build --parallel 4
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ctest --test-dir $build --output-on-failure
  exit $LASTEXITCODE
} finally {
  $env:PATH=$previousPath
}
