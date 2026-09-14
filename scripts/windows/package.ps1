# Builds a self-contained Fovea folder from a Windows build tree:
#   <Out>\Fovea.cmd                       launcher
#   <Out>\bin\                            fovea.exe, fovea-core.exe, rtsp-testsrc.exe, Qt and GStreamer DLLs
#   <Out>\lib\gstreamer-1.0\             plugins
#   <Out>\libexec\gstreamer-1.0\         gst-plugin-scanner.exe
# GStreamer locates plugins relative to the directory of gstreamer-1.0-0.dll
# (<dll dir>\..\lib\gstreamer-1.0), which is why the executables live in bin.
param(
  [string]$BuildDir = "build\windows-msvc",
  [string]$Out = "dist\fovea",
  [string]$GstRoot = $env:GSTREAMER_1_0_ROOT_MSVC_X86_64,
  [string]$QtRoot = $env:QT_ROOT
)
$ErrorActionPreference = "Stop"
if (-not $GstRoot) { throw "GSTREAMER_1_0_ROOT_MSVC_X86_64 is not set" }
if (-not $QtRoot) { throw "QT_ROOT is not set" }
$GstRoot = $GstRoot.TrimEnd("\")

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$Bin = "$Out\bin"
New-Item -ItemType Directory -Force -Path $Bin, "$Out\lib\gstreamer-1.0", "$Out\libexec\gstreamer-1.0" | Out-Null

# qt_standard_project_setup places Windows executables in the top build directory; search by name.
foreach ($name in @("fovea.exe", "fovea-core.exe", "rtsp-testsrc.exe")) {
  $found = Get-ChildItem -Path $BuildDir -Recurse -Filter $name -File | Where-Object { $_.FullName -notmatch "\\CMakeFiles\\" } | Select-Object -First 1
  if (-not $found) { throw "missing $name under $BuildDir" }
  Copy-Item $found.FullName $Bin
}

& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --dir $Bin "$Bin\fovea.exe" "$Bin\fovea-core.exe"
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

Copy-Item "$GstRoot\bin\*.dll" $Bin
foreach ($tool in @("gst-inspect-1.0.exe", "gst-launch-1.0.exe")) { Copy-Item "$GstRoot\bin\$tool" $Bin }
Copy-Item "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" "$Out\libexec\gstreamer-1.0\"

$plugins = @(
  "coreelements", "app", "videoconvertscale", "videorate", "videotestsrc", "videofilter",
  "rtsp", "rtp", "rtpmanager", "udp", "tcp",
  "videoparsersbad", "libav", "isomp4", "matroska", "multifile",
  "playback", "typefindfunctions", "pbtypes", "jpeg", "pango",
  "x264", "openh264", "mediafoundation", "d3d11", "nvcodec", "rtspclientsink", "autodetect"
)
$missing = @()
$required = @("coreelements", "app", "videoconvertscale", "rtsp", "rtp", "rtpmanager", "udp", "videoparsersbad", "libav", "isomp4", "matroska", "multifile", "playback", "typefindfunctions", "jpeg", "videotestsrc")
foreach ($p in $plugins) {
  $src = "$GstRoot\lib\gstreamer-1.0\gst$p.dll"
  if (Test-Path $src) { Copy-Item $src "$Out\lib\gstreamer-1.0\" } else { $missing += $p }
}
if ($missing.Count) { Write-Warning "plugins not in this GStreamer build: $($missing -join ', ')" }
$lost = $required | Where-Object { $missing -contains $_ }
if ($lost) { throw "required plugins missing: $($lost -join ', ')" }

$crt = Get-ChildItem -Path "$env:VCToolsRedistDir\x64" -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($crt) { Copy-Item "$($crt.FullName)\*.dll" $Bin } else { Write-Warning "MSVC runtime DLLs not found; target machines need the VC++ 2015-2022 redistributable" }

foreach ($f in @("LICENSE", "THIRD_PARTY_NOTICES.md")) { if (Test-Path $f) { Copy-Item $f $Out } }

# Model worker source; Setup-Worker.cmd creates worker\.venv, which fovea-core finds by walking up from bin.
New-Item -ItemType Directory -Force -Path "$Out\worker", "$Out\scripts\windows" | Out-Null
Copy-Item "worker\pyproject.toml", "worker\README.md" "$Out\worker\"
Copy-Item -Recurse "worker\fovea_worker" "$Out\worker\fovea_worker"
Get-ChildItem -Path "$Out\worker" -Recurse -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force
Copy-Item "scripts\windows\setup-worker.ps1" "$Out\scripts\windows\"
"@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0scripts\windows\setup-worker.ps1`" %*`r`npause`r`n" | Out-File -FilePath "$Out\Setup-Worker.cmd" -Encoding ascii -NoNewline
Copy-Item "scripts\verify_m1.py" $Out
"@echo off`r`nstart `"`" `"%~dp0bin\fovea.exe`" %*`r`n" | Out-File -FilePath "$Out\Fovea.cmd" -Encoding ascii -NoNewline
@"
Fovea (development build)

Run Fovea.cmd (or bin\fovea.exe). It starts bin\fovea-core.exe in the
background; closing the window keeps recording running. Use the title bar
menu to stop the service.

Data: %LOCALAPPDATA%\Fovea (set FOVEA_DATA_DIR to change it).

Local test camera without hardware (from this folder):
  bin\rtsp-testsrc.exe --pattern ball        -> rtsp://127.0.0.1:8554/test
  bin\rtsp-testsrc.exe --webcam              -> this PC's webcam over RTSP
  bin\rtsp-testsrc.exe --list-devices
Add the URL in the console with "+ Add".

Detection, alerts and search (optional, needs Python 3.12 and internet once):
  Setup-Worker.cmd
This creates worker\.venv with PyTorch (CUDA build when an NVIDIA GPU is
present) and the RF-DETR detector, and downloads the search embedding model
(SigLIP 2, about 1.5 GB). fovea-core finds the worker automatically on the
next start. The detector weights (about 370 MB) download on first use.
Then open Alerts & Analytics, create a rule with a zone on a camera that has
"Run analytics on this camera" enabled, or open Search and ask a question in
Korean or English once the index line under the filters shows coverage.
Recordings are indexed one frame per second after each segment closes.

Self test (Python 3.10+ on PATH):
  python verify_m1.py --bin-dir bin --flat
"@ | Out-File -FilePath "$Out\README.txt" -Encoding utf8
Write-Host "packaged $Out"
