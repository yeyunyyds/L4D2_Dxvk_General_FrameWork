param(
  [Parameter(Mandatory=$true)]
  [ValidateSet('Install','Restore')]
  [string]$Action,
  [string]$Game
)

$ErrorActionPreference = 'Stop'
$PackageRoot = Split-Path -Parent $PSScriptRoot
if (Test-Path (Join-Path $PSScriptRoot 'capture-build.json')) {
  $PackageRoot = $PSScriptRoot
}
$RelativeFiles = @(
  'bin/dxvk_d3d9.dll',
  'bin/.l4d2bridge/L4D2Bridge64.exe',
  'bin/.l4d2bridge/L4D2Bridge32.exe',
  'bin/.l4d2bridge/LDBFsrBackend64.dll',
  'bin/.l4d2bridge/d3d9vk_x64.dll'
)

function FilePath([string]$Root, [string]$Relative) {
  return Join-Path $Root ($Relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
}

function Sha256([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function FindGame([string]$Requested) {
  $Candidates = @()
  if ($Requested) { $Candidates += $Requested }
  foreach ($Key in @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 550',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 550',
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 550')) {
    try {
      $Value = (Get-ItemProperty -LiteralPath $Key -Name InstallLocation -ErrorAction Stop).InstallLocation
      if ($Value) { $Candidates += $Value }
    } catch {}
  }
  try {
    $Steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
    if ($Steam) { $Candidates += (Join-Path $Steam 'steamapps\common\Left 4 Dead 2') }
  } catch {}
  foreach ($Candidate in $Candidates) {
    if ($Candidate) {
      $Resolved = [IO.Path]::GetFullPath($Candidate)
      if ((Test-Path -LiteralPath (Join-Path $Resolved 'left4dead2.exe') -PathType Leaf) -and
          (Test-Path -LiteralPath (Join-Path $Resolved 'bin') -PathType Container)) {
        return $Resolved.TrimEnd('\')
      }
    }
  }
  throw 'L4D2 directory not found. Pass it as the first argument, for example: install_fg_test.cmd "D:\SteamLibrary\steamapps\common\Left 4 Dead 2"'
}

function RequireStopped {
  $Running = @(Get-Process -Name left4dead2,L4D2Bridge64,L4D2Bridge32 -ErrorAction SilentlyContinue)
  if ($Running.Count) { throw 'Exit L4D2 and both Bridge Host processes before changing files.' }
}

$GameRoot = FindGame $Game
$BackupRoot = Join-Path $GameRoot '.ldb-fg-test-backup'
$ManifestPath = Join-Path $BackupRoot 'manifest.json'
RequireStopped

if ($Action -eq 'Install') {
  if (Test-Path -LiteralPath $BackupRoot) {
    throw "An FG test backup already exists: $BackupRoot`nRun remove_fg_test.cmd before installing another build."
  }
  $ReceiptPath = Join-Path $PackageRoot 'capture-build.json'
  if (-not (Test-Path -LiteralPath $ReceiptPath -PathType Leaf)) { throw 'capture-build.json is missing from the package.' }
  $Receipt = Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json
  $Records = @()
  foreach ($Relative in $RelativeFiles) {
    $Source = FilePath $PackageRoot $Relative
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Package file is missing: $Relative" }
    $Property = $Receipt.binaries_sha256.PSObject.Properties[$Relative]
    if (-not $Property -or (Sha256 $Source) -ne ([string]$Property.Value).ToLowerInvariant()) {
      throw "Package checksum mismatch: $Relative"
    }
    $Destination = FilePath $GameRoot $Relative
    $Exists = Test-Path -LiteralPath $Destination -PathType Leaf
    $Records += [ordered]@{
      relative = $Relative
      original_exists = $Exists
      original_sha256 = $(if ($Exists) { Sha256 $Destination } else { $null })
      package_sha256 = Sha256 $Source
    }
  }
  New-Item -ItemType Directory -Path (Join-Path $BackupRoot 'files') -Force | Out-Null
  foreach ($Record in $Records) {
    if ($Record.original_exists) {
      $Source = FilePath $GameRoot $Record.relative
      $Saved = FilePath (Join-Path $BackupRoot 'files') $Record.relative
      New-Item -ItemType Directory -Path (Split-Path -Parent $Saved) -Force | Out-Null
      Copy-Item -LiteralPath $Source -Destination $Saved
    }
  }
  $Manifest = [ordered]@{ schema=1; state='installing'; game_root=$GameRoot; build_id=$Receipt.build_id; files=$Records }
  $Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
  try {
    foreach ($Record in $Records) {
      $Source = FilePath $PackageRoot $Record.relative
      $Destination = FilePath $GameRoot $Record.relative
      New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
      Copy-Item -LiteralPath $Source -Destination $Destination -Force
      if ((Sha256 $Destination) -ne $Record.package_sha256) { throw "Installed checksum mismatch: $($Record.relative)" }
    }
  } catch {
    foreach ($Record in $Records) {
      $Destination = FilePath $GameRoot $Record.relative
      if ($Record.original_exists) {
        Copy-Item -LiteralPath (FilePath (Join-Path $BackupRoot 'files') $Record.relative) -Destination $Destination -Force
      } elseif (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Force
      }
    }
    Remove-Item -LiteralPath $BackupRoot -Recurse -Force
    throw
  }
  $Manifest.state = 'installed'
  $Manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
  Write-Host "Installed $($Receipt.build_id). Original files are stored in $BackupRoot"
  Write-Host 'Run start_fg_test.cmd for the test, then remove_fg_test.cmd to restore.'
  exit 0
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
  throw "No transactional FG backup exists: $ManifestPath"
}
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ([IO.Path]::GetFullPath([string]$Manifest.game_root).TrimEnd('\') -ne $GameRoot) {
  throw 'Backup belongs to another game directory.'
}
# Validate every installed file before changing any of them. A later manual
# update must never be overwritten by an older backup. An interrupted install
# may contain either the original or package copy and remains recoverable.
$Interrupted = $Manifest.state -eq 'installing'
if (-not $Interrupted -and $Manifest.state -ne 'installed') { throw 'Unknown backup state.' }
foreach ($Record in $Manifest.files) {
  $Current = FilePath $GameRoot $Record.relative
  $CurrentExists = Test-Path -LiteralPath $Current -PathType Leaf
  $CurrentHash = $(if ($CurrentExists) { Sha256 $Current } else { $null })
  $PackageMatch = $CurrentExists -and $CurrentHash -eq ([string]$Record.package_sha256).ToLowerInvariant()
  $OriginalMatch = $Interrupted -and
    (($Record.original_exists -and $CurrentExists -and $CurrentHash -eq ([string]$Record.original_sha256).ToLowerInvariant()) -or
     (-not $Record.original_exists -and -not $CurrentExists))
  if (-not $PackageMatch -and -not $OriginalMatch) {
    throw "Installed file changed after this FG test; refusing to overwrite it: $($Record.relative)"
  }
  if ($Record.original_exists) {
    $Saved = FilePath (Join-Path $BackupRoot 'files') $Record.relative
    if (-not (Test-Path -LiteralPath $Saved -PathType Leaf) -or
        (Sha256 $Saved) -ne ([string]$Record.original_sha256).ToLowerInvariant()) {
      throw "Backup checksum mismatch: $($Record.relative)"
    }
  }
}
foreach ($Record in $Manifest.files) {
  $Destination = FilePath $GameRoot $Record.relative
  if ($Record.original_exists) {
    Copy-Item -LiteralPath (FilePath (Join-Path $BackupRoot 'files') $Record.relative) -Destination $Destination -Force
    if ((Sha256 $Destination) -ne ([string]$Record.original_sha256).ToLowerInvariant()) {
      throw "Restore verification failed: $($Record.relative)"
    }
  } else {
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Force }
  }
}
Remove-Item -LiteralPath $BackupRoot -Recurse -Force
Write-Host "Restored the exact pre-test files for $($Manifest.build_id)."
