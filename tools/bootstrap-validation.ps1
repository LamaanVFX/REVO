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

# Pin a dated FFmpeg release and verify both the manifest and the exact archive.
function Get-PinnedReleaseAssetSha256($repository, $releaseTag, $assetName, $manifestSha256) {
  $manifestUrl = "https://github.com/$repository/releases/download/$releaseTag/checksums.sha256"
  $download = Join-Path $root "ffmpeg-release-checksums.$([guid]::NewGuid().ToString('N')).download"
  & curl.exe -fL --retry 3 --silent --show-error $manifestUrl -o $download
  if ($LASTEXITCODE -ne 0) { throw "FFmpeg checksum-manifest download failed: $manifestUrl" }
  $actualManifestHash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actualManifestHash -ne $manifestSha256) {
    Remove-Item -LiteralPath $download -Force
    throw "FFmpeg checksum-manifest mismatch: expected $manifestSha256, found $actualManifestHash"
  }
  $line = Get-Content -LiteralPath $download | Where-Object { $_.Contains($assetName) } | Select-Object -First 1
  Remove-Item -LiteralPath $download -Force
  if (-not $line) { throw "Pinned FFmpeg checksum manifest does not contain $assetName" }
  $parts = $line.Trim() -split ' +'
  if ($parts.Count -lt 2 -or $parts[0] -notmatch '^[0-9a-fA-F]{64}$') {
    throw "Could not parse SHA-256 entry for $assetName from FFmpeg manifest"
  }
  $assetHash = $parts[0].ToLowerInvariant()
  $entryName = $parts[$parts.Count - 1].TrimStart('*')
  if ($entryName -ne $assetName) { throw "FFmpeg manifest entry mismatch: expected $assetName, found $entryName" }
  return $assetHash
}
# BtbN's build releases moved to the FFmpeg-Builds organization. Pin the
# current FFmpeg 8.1 LGPL shared package by the checksums manifest digest and
# verify the archive hash from that manifest before expanding it.
$ffmpegRepository = 'FFmpeg-Builds/FFmpeg-Builds'
$ffmpegRelease = 'latest'
$ffmpegArchive = 'ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip'
$ffmpegManifestSha256 = '1cf206aaa1d2fd0eef3ed2efc41ee24a520defa0c870a8b88b5f8575bbca0d08'
$ffmpegSha256 = Get-PinnedReleaseAssetSha256 $ffmpegRepository $ffmpegRelease $ffmpegArchive $ffmpegManifestSha256
Expand-PinnedPackage 'ffmpeg-8.1' "https://github.com/$ffmpegRepository/releases/download/$ffmpegRelease/$ffmpegArchive" $ffmpegSha256
Expand-PinnedPackage 'winsdk' 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.28000.2705/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg' 'a74ca8f9af98bd61925d9e2932ad98f746306123167b8f0bba99cd9ea9f03807'
Expand-PinnedPackage 'imgui-1.92.8' 'https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip' '27765c56ab27ce47472d0bea43cf1e3301c726362ce585e99a059e3b37616870'
