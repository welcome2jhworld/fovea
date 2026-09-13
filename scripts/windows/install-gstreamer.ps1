# Installs the official GStreamer MSVC x86_64 runtime and development packages
# into C:\gstreamer and prints the root. Checksums are pinned per version.
param(
  [string]$Version = "1.26.11",
  [string]$InstallDir = "C:\gstreamer"
)
$ErrorActionPreference = "Stop"
$hashes = @{
  "1.26.11" = @{
    "runtime" = "31cbc21fa0950b5c1e79c80959b2799805cb05a7a35953a13a9f790776137605"
    "devel"   = "af0a0720692052aaf5fb0cf2847ef34a87016ef0f914328d516f049e6c86f587"
  }
}
if (-not $hashes.ContainsKey($Version)) { throw "no pinned checksums for GStreamer $Version" }
$base = "https://gstreamer.freedesktop.org/data/pkg/windows/$Version/msvc"
$tmp = Join-Path $env:TEMP "gst-$Version"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
foreach ($kind in @("runtime", "devel")) {
  $name = if ($kind -eq "runtime") { "gstreamer-1.0-msvc-x86_64-$Version.msi" } else { "gstreamer-1.0-devel-msvc-x86_64-$Version.msi" }
  $msi = Join-Path $tmp $name
  if (-not (Test-Path $msi)) {
    Write-Host "downloading $name"
    Invoke-WebRequest -Uri "$base/$name" -OutFile $msi -UseBasicParsing
  }
  $actual = (Get-FileHash -Algorithm SHA256 $msi).Hash.ToLower()
  if ($actual -ne $hashes[$Version][$kind]) { throw "checksum mismatch for $name ($actual)" }
  Write-Host "installing $name"
  $p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList @("/i", "`"$msi`"", "/qn", "/norestart", "ADDLOCAL=ALL", "INSTALLDIR=`"$InstallDir`"")
  if ($p.ExitCode -ne 0) { throw "msiexec failed for $name with $($p.ExitCode)" }
}
$dll = Get-ChildItem -Path $InstallDir, "C:\Program Files\gstreamer" -Recurse -Filter "gstreamer-1.0-0.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dll) { throw "gstreamer-1.0-0.dll not found after install" }
$root = Split-Path -Parent (Split-Path -Parent $dll.FullName)
Write-Host "GStreamer root: $root"
if ($env:GITHUB_ENV) {
  "GSTREAMER_1_0_ROOT_MSVC_X86_64=$root\" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
  "$root\bin" | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
}
$root
