$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..\work\validation-tools'
New-Item -ItemType Directory -Force $root | Out-Null
$root = (Resolve-Path $root).Path

function Expand-PinnedPackage($name, $url, $sha256) {
  $zip = Join-Path $root "$name.zip"
  $dest = Join-Path $root $name
  $marker = Join-Path $dest '.complete'

  if (Test-Path $marker) {
    if (-not (Test-Path $zip)) { throw "Cached package $name has no source archive; refusing to trust it." }
    $cachedHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($cachedHash -ne $sha256) { throw "Cached package $name checksum mismatch: expected $sha256, found $cachedHash." }
    $markerHash = Get-Content -LiteralPath $marker -Raw -ErrorAction SilentlyContinue
    if ($null -eq $markerHash) { $markerHash = '' }
    $markerHash = $markerHash.Trim()
    if ($markerHash -and $markerHash -ne $sha256) { throw "Cached package $name marker mismatch: expected $sha256, found $markerHash." }
    if (-not $markerHash) { Set-Content -LiteralPath $marker -Value $sha256 -NoNewline }
    Write-Output "$name verified from pinned cache ($sha256)"
    return
  }

  $download = "$zip.$([guid]::NewGuid().ToString('N')).download"
  & curl.exe -fL --retry 3 --silent --show-error $url -o $download
  if ($LASTEXITCODE -ne 0) { throw "Download failed: $url" }
  $actualHash = (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actualHash -ne $sha256) {
    Remove-Item -LiteralPath $download -Force
    throw "Checksum mismatch: $name expected $sha256, found $actualHash"
  }

  Move-Item -LiteralPath $download -Destination $zip -Force
  Write-Output "$name sha256:$actualHash $url"
  Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
  Set-Content -LiteralPath $marker -Value $sha256 -NoNewline
}


# Keep tool versions and archive hashes fixed so clean-machine validation is repeatable.
Expand-PinnedPackage 'llvm' 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip' 'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666'
Expand-PinnedPackage 'cmake' 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip' '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab'
Expand-PinnedPackage 'ninja' 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip' '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
Expand-PinnedPackage 'ffmpeg-8.1' 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip' 'ec30bde63b8e36ebdc7ba5b37140a43a2d2afe884650269fadeb033a57098cfc'
Expand-PinnedPackage 'winsdk' 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.28000.2705/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg' 'a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807'
Expand-PinnedPackage 'imgui-1.92.8' 'https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip' '27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870'

# OBS 32.1.2's D3D11 backend includes d3dkmthk.h for optional HAGS status
# reporting. The SDK package alone omits this WDK header, so acquire the official
# versioned WDK NuGet package and verify it against NuGet's SHA-512 sidecar.
$wdkVersion = '10.0.28000.2526'
$wdkZip = Join-Path $root "wdk-$wdkVersion.zip"
$wdkDest = Join-Path $root "wdk-$wdkVersion"
$wdkShaUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$wdkVersion/microsoft.windows.wdk.x64.$wdkVersion.nupkg.sha512"
$wdkUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$wdkVersion/microsoft.windows.wdk.x64.$wdkVersion.nupkg"
if (-not (Test-Path $wdkZip)) {
  & curl.exe -fL --retry 3 --silent --show-error $wdkUrl -o $wdkZip
  if ($LASTEXITCODE -ne 0) { throw "WDK package download failed: $wdkUrl" }
}
$wdkShaFile = "$wdkZip.sha512"
& curl.exe -fL --retry 3 --silent --show-error $wdkShaUrl -o $wdkShaFile
if ($LASTEXITCODE -ne 0) { throw "WDK SHA-512 metadata download failed: $wdkShaUrl" }
$expectedWdkSha512 = (Get-Content -LiteralPath $wdkShaFile -Raw).Trim()
$actualWdkSha512 = [Convert]::ToBase64String([Convert]::FromHexString((Get-FileHash -LiteralPath $wdkZip -Algorithm SHA512).Hash))
if ($actualWdkSha512 -ne $expectedWdkSha512) { throw "WDK package SHA-512 mismatch for $wdkVersion" }
$wdkMarker = Join-Path $wdkDest '.complete'
if (-not (Test-Path $wdkMarker)) {
  New-Item -ItemType Directory -Force $wdkDest | Out-Null
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::ExtractToDirectory($wdkZip, $wdkDest)
  Set-Content -LiteralPath $wdkMarker -Value $expectedWdkSha512 -NoNewline
}
$wdkHeader = Get-ChildItem -LiteralPath $wdkDest -Filter 'd3dkmthk.h' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $wdkHeader) { throw "WDK package $wdkVersion did not contain d3dkmthk.h" }
Write-Output "Verified Microsoft WDK $wdkVersion (SHA512 sidecar); header: $($wdkHeader.FullName)"
