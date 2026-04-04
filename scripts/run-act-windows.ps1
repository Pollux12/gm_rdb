param(
  [ValidateSet('x64','x86','both')]
  [string]$Arch = 'both',
  [string]$Workflow = '.github/workflows/build.yml',
  [int]$KeepRuns = 2
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outBase = Join-Path $root 'output/act-test'
$artifactBase = Join-Path $outBase 'artifacts'
$artifactRoot = Join-Path $artifactBase $stamp
$outStamp = Join-Path $outBase $stamp

New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
New-Item -ItemType Directory -Path $outStamp -Force | Out-Null

function Invoke-ActBuild([string]$arch, [string]$artifactPath) {
  act workflow_dispatch -W $Workflow -j build --matrix os:windows-latest --matrix arch:$arch -P windows-latest=-self-hosted --artifact-server-path $artifactPath
}

function Extract-Artifact([string]$arch, [string]$artifactPath, [string]$destination) {
  $zip = Get-ChildItem -Path $artifactPath -Recurse -Filter "*-$arch-RelWithDebInfo.zip" | Select-Object -First 1
  if (-not $zip) { throw "No artifact zip found for $arch in $artifactPath" }
  New-Item -ItemType Directory -Path $destination -Force | Out-Null
  Expand-Archive -Path $zip.FullName -DestinationPath $destination -Force
}

if ($Arch -in @('x64','both')) {
  $x64Artifact = Join-Path $artifactRoot 'x64'
  Invoke-ActBuild -arch 'x64' -artifactPath $x64Artifact
  Extract-Artifact -arch 'x64' -artifactPath $x64Artifact -destination (Join-Path $outStamp 'x64')
}

if ($Arch -in @('x86','both')) {
  $x86Artifact = Join-Path $artifactRoot 'x86'
  Invoke-ActBuild -arch 'x86' -artifactPath $x86Artifact
  Extract-Artifact -arch 'x86' -artifactPath $x86Artifact -destination (Join-Path $outStamp 'x86')
}

$latest = Join-Path $outBase 'latest'
if (Test-Path $latest) {
  Remove-Item -Recurse -Force $latest
}
New-Item -ItemType Directory -Path $latest -Force | Out-Null

if (Test-Path (Join-Path $outStamp 'x64')) {
  Copy-Item -Recurse -Force (Join-Path $outStamp 'x64') (Join-Path $latest 'x64')
}
if (Test-Path (Join-Path $outStamp 'x86')) {
  Copy-Item -Recurse -Force (Join-Path $outStamp 'x86') (Join-Path $latest 'x86')
}

# Remove legacy root-level act artifact folders created by older script versions.
Get-ChildItem -Path $root -Directory -Filter 'act_artifacts_*' -ErrorAction SilentlyContinue | ForEach-Object {
  Remove-Item -Recurse -Force $_.FullName
}

# Keep output tidy by pruning older timestamped runs and artifact bundles.
if ($KeepRuns -ge 0) {
  $runDirs = Get-ChildItem -Path $outBase -Directory -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -match '^\d{8}-\d{6}$'
  } | Sort-Object Name -Descending

  $artifactDirs = Get-ChildItem -Path $artifactBase -Directory -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -match '^\d{8}-\d{6}$'
  } | Sort-Object Name -Descending

  if ($runDirs.Count -gt $KeepRuns) {
    $runDirs | Select-Object -Skip $KeepRuns | ForEach-Object { Remove-Item -Recurse -Force $_.FullName }
  }
  if ($artifactDirs.Count -gt $KeepRuns) {
    $artifactDirs | Select-Object -Skip $KeepRuns | ForEach-Object { Remove-Item -Recurse -Force $_.FullName }
  }
}

Write-Host "Artifacts: $artifactRoot"
Write-Host "Output:    $outStamp"
Get-ChildItem -Path $outStamp -Recurse -Filter *.dll | Select-Object FullName, Length
