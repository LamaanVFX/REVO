param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Destination) { $Destination = Join-Path $repo 'work/obs-plugin-sdk-33.0.1' }
$Destination = [IO.Path]::GetFullPath($Destination)
$work = Join-Path $repo 'work'
$source = Join-Path $work 'obs-source-33.0.1'
$runtimeExtract = Join-Path $work 'obs-runtime-33.0.1'
$archive = Join-Path $work 'OBS-Studio-33.0.1-Windows-x64.zip'
$expectedCommit = '65c320b46e1af5c7a555161aea6182cdf7a8c524'
$expectedArchiveHash = '0d9436451c2d772b33428e3fdae9776f071895c335de17283176306fb72651d3'
$runtimeUrl = 'https://cdn-fastly.obsproject.com/downloads/OBS-Studio-33.0.1-Windows-x64.zip'

$toolRoot = Join-Path $work 'validation-tools'
$llvmDir = Get-ChildItem (Join-Path $toolRoot 'llvm') -Directory | Select-Object -First 1
if (-not $llvmDir) { throw 'Pinned LLVM validation tools are missing; run tools/bootstrap-validation.ps1 first.' }
$llvmBin = Join-Path $llvmDir.FullName 'bin'

if (-not (Test-Path (Join-Path $source 'libobs/obs.h'))) {
  if (Test-Path $source) { Remove-Item $source -Recurse -Force }
  & git clone --depth 1 --branch 33.0.1 https://github.com/obsproject/obs-studio.git $source
  if ($LASTEXITCODE -ne 0) { throw 'Could not obtain OBS Studio 33.0.1 source.' }
}
$actualCommit = (& git -C $source rev-parse HEAD).Trim()
if ($actualCommit -ne $expectedCommit) {
  throw "OBS source commit mismatch: expected $expectedCommit, found $actualCommit"
}

New-Item -ItemType Directory -Force $work | Out-Null
if (-not (Test-Path $archive)) {
  & curl.exe -fL --retry 3 --silent --show-error $runtimeUrl -o $archive
  if ($LASTEXITCODE -ne 0) { throw "Could not download OBS 33.0.1 runtime: $runtimeUrl" }
}
$actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedArchiveHash) {
  throw "OBS runtime SHA-256 mismatch: expected $expectedArchiveHash, found $actualHash"
}

$obsDll = $null
if (Test-Path $runtimeExtract) {
  $candidate = Get-ChildItem $runtimeExtract -Filter obs.dll -File -Recurse | Where-Object { $_.Directory.Name -eq '64bit' } | Select-Object -First 1
  if ($candidate) { $obsDll = $candidate.FullName }
}
if (-not $obsDll) {
  if (Test-Path $runtimeExtract) { Remove-Item $runtimeExtract -Recurse -Force }
  New-Item -ItemType Directory -Force $runtimeExtract | Out-Null
  Expand-Archive -LiteralPath $archive -DestinationPath $runtimeExtract -Force
  $candidate = Get-ChildItem $runtimeExtract -Filter obs.dll -File -Recurse | Where-Object { $_.Directory.Name -eq '64bit' } | Select-Object -First 1
  if (-not $candidate) { throw 'OBS runtime archive does not contain bin/64bit/obs.dll.' }
  $obsDll = $candidate.FullName
}

if (Test-Path $Destination) { Remove-Item $Destination -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $Destination 'include/libobs'), (Join-Path $Destination 'bin/64bit'), (Join-Path $Destination 'lib') | Out-Null

Copy-Item (Join-Path $source 'libobs/*') (Join-Path $Destination 'include/libobs') -Recurse -Force
Copy-Item -LiteralPath $obsDll -Destination (Join-Path $Destination 'bin/64bit/obs.dll') -Force

$exports = @(& (Join-Path $llvmBin 'llvm-readobj.exe') --coff-exports (Join-Path $Destination 'bin/64bit/obs.dll') | ForEach-Object { if ($_ -match '^  Name: (.+)$') { $Matches[1] } })
if (-not $exports.Count) { throw 'Could not read OBS 33.0.1 exports.' }
@('LIBRARY obs.dll', 'EXPORTS') + $exports | Set-Content (Join-Path $Destination 'lib/obs.def') -Encoding ascii
& (Join-Path $llvmBin 'llvm-dlltool.exe') -m i386:x86-64 -d (Join-Path $Destination 'lib/obs.def') -l (Join-Path $Destination 'lib/libobs.dll.a')
if ($LASTEXITCODE -ne 0) { throw 'Could not generate the OBS 33.0.1 import library.' }

@{
  schema_revision = 1
  version = '33.0.1'
  source_commit = $expectedCommit
  runtime_sha256 = $expectedArchiveHash
} | ConvertTo-Json | Set-Content (Join-Path $Destination 'pin.json') -Encoding utf8

Write-Output "Pinned OBS 33.0.1 plugin SDK prepared: $Destination"
