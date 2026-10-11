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

# CMake's GNU/Clang Windows platform files parse compiler paths as CMake
# strings. Forward slashes prevent paths such as D:\a from becoming escapes.
$llvmBin=Join-Path $llvm 'bin'
$compiler=Join-Path $llvmBin 'clang++.exe'
$resourceCompiler=Join-Path $llvmBin 'llvm-windres.exe'
$compiler=$compiler.Replace('\','/')
$resourceCompiler=$resourceCompiler.Replace('\','/')
$sdkCpp=Join-Path $sdk 'cppwinrt'
$sdkCpp=$sdkCpp.Replace('\','/')
$imgui=$imgui.Replace('\','/')
$ffmpegRoot=$ffmpegRoot.Replace('\','/')
$repoCmake=$repo.Replace('\','/')

$previousPath=$env:PATH
try {
  $env:PATH="$llvmBin;$cmake/bin;$deps/ninja;$env:PATH"
  $build=Join-Path $repo ("build-native-$Configuration" + $(if($BuildSuffix){"-$BuildSuffix"}else{''}))
  & cmake -S $repo -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_CXX_COMPILER=$compiler" `
    "-DCMAKE_RC_COMPILER=$resourceCompiler" `
    "-DCMAKE_CXX_FLAGS=-isystem $sdkCpp" `
    "-Dimgui_DIR=$repoCmake/cmake/validation-imgui" `
    "-DKLIP_IMGUI_SOURCE=$imgui" `
    "-DFFMPEG_ROOT=$ffmpegRoot" `
    '-DKLIP_USE_LIBOBS=OFF' `
    '-DKLIP_BUILD_APP=ON' `
    '-DKLIP_BUILD_TESTS=ON'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & cmake --build $build --parallel 4
  if($LASTEXITCODE){exit $LASTEXITCODE}

  # Package every FFmpeg runtime DLL, including transitive codec dependencies.
  $ffmpegBin=Join-Path $ffmpegRoot 'bin'
  $ffmpegDlls=@(Get-ChildItem $ffmpegBin -File -Filter '*.dll' -ErrorAction SilentlyContinue)
  if(-not $ffmpegDlls.Count){throw "No FFmpeg runtime DLLs found in $ffmpegBin"}
  Copy-Item -LiteralPath $ffmpegDlls.FullName -Destination $build -Force

  # llvm-mingw's C++ runtime is not part of Windows; ship it beside Klip.exe.
  foreach($runtimeName in @('libc++.dll','libunwind.dll','libwinpthread-1.dll','libgcc_s_seh-1.dll')) {
    $runtimePath=Join-Path $llvmBin $runtimeName
    if(Test-Path $runtimePath) { Copy-Item -LiteralPath $runtimePath -Destination $build -Force }
  }

  & ctest --test-dir $build --output-on-failure
  exit $LASTEXITCODE
} finally {
  $env:PATH=$previousPath
}
