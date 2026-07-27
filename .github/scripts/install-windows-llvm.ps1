# Installs the exact LLVM development distribution used by Windows CI.
#
# The official archive is a tar stream compressed with xz. Windows Server's
# bundled bsdtar can stall indefinitely while expanding this particular large
# archive, even though the download and SHA-256 verification complete in
# seconds. GitHub's Windows image also supplies 7-Zip; expanding the xz and tar
# layers explicitly gives each phase a visible boundary and avoids that bsdtar
# failure mode.
#
# Inputs are the upstream version, its independently pinned SHA-256, and an
# absent destination below RUNNER_TEMP. On success the destination is a complete
# LLVM prefix whose lib/cmake/llvm/LLVMConfig.cmake is present. Temporary
# archives are removed so the subsequent bootstrap build retains disk headroom.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $Version,

  [Parameter(Mandatory = $true)]
  [string] $ArchiveSha256,

  [Parameter(Mandatory = $true)]
  [string] $Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
  throw "RUNNER_TEMP is required"
}

$archive = Join-Path $env:RUNNER_TEMP "llvm-$Version.tar.xz"
$tarArchive = Join-Path $env:RUNNER_TEMP "llvm-$Version.tar"
$unpacked = Join-Path $env:RUNNER_TEMP "llvm-$Version-unpacked"
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$Version/clang%2Bllvm-$Version-x86_64-pc-windows-msvc.tar.xz"

# A cache miss always starts from a fresh hosted runner, but removing these
# exact scratch paths also makes a manually retried step deterministic.
foreach ($path in @($archive, $tarArchive, $unpacked, $Destination)) {
  if (Test-Path -LiteralPath $path) {
    Remove-Item -LiteralPath $path -Recurse -Force
  }
}

Write-Host "Downloading LLVM $Version at $((Get-Date).ToUniversalTime().ToString('o'))"
& curl.exe --fail --location --retry 5 --retry-all-errors `
  --output $archive $url
if ($LASTEXITCODE -ne 0) {
  throw "LLVM archive download failed with exit code $LASTEXITCODE"
}

Write-Host "Verifying LLVM archive at $((Get-Date).ToUniversalTime().ToString('o'))"
$actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
$expected = $ArchiveSha256.ToLowerInvariant()
if ($actual -ne $expected) {
  throw "LLVM archive SHA-256 mismatch: expected $expected, got $actual"
}

$sevenZip = (Get-Command 7z.exe -ErrorAction Stop).Source
Write-Host "Expanding the xz layer at $((Get-Date).ToUniversalTime().ToString('o'))"
& $sevenZip x $archive "-o$env:RUNNER_TEMP" -y
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $tarArchive)) {
  throw "7-Zip failed to expand the LLVM xz layer"
}

New-Item -ItemType Directory -Path $unpacked | Out-Null
Write-Host "Expanding the tar layer at $((Get-Date).ToUniversalTime().ToString('o'))"
& $sevenZip x $tarArchive "-o$unpacked" -y
if ($LASTEXITCODE -ne 0) {
  throw "7-Zip failed to expand the LLVM tar layer"
}

# The upstream archive owns exactly one top-level versioned directory. Move
# that directory as a unit rather than copying a multi-gigabyte tree or relying
# on a tool-specific strip-components option.
$rootDirectories = @(Get-ChildItem -LiteralPath $unpacked -Directory -Force)
$rootFiles = @(Get-ChildItem -LiteralPath $unpacked -File -Force)
if ($rootDirectories.Count -ne 1 -or $rootFiles.Count -ne 0) {
  throw "LLVM archive does not contain exactly one top-level directory"
}
Move-Item -LiteralPath $rootDirectories[0].FullName -Destination $Destination

$llvmConfig = Join-Path $Destination "lib\cmake\llvm\LLVMConfig.cmake"
if (-not (Test-Path -LiteralPath $llvmConfig)) {
  throw "LLVM development archive has no CMake package"
}

Remove-Item -LiteralPath $archive, $tarArchive -Force
Remove-Item -LiteralPath $unpacked -Force
Write-Host "LLVM $Version is ready at $((Get-Date).ToUniversalTime().ToString('o'))"
