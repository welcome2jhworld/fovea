# Builds a self-contained Fovea folder from a Windows build tree:
#   <Out>\fovea.exe, fovea-core.exe, rtsp-testsrc.exe, Qt and GStreamer DLLs,
#   <Out>\lib\gstreamer-1.0\ (plugins), <Out>\libexec\gstreamer-1.0\gst-plugin-scanner.exe
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
New-Item -ItemType Directory -Force -Path "$Out\lib\gstreamer-1.0", "$Out\libexec\gstreamer-1.0" | Out-Null

$exes = @(
  "$BuildDir\src\console\fovea.exe",
  "$BuildDir\src\core\fovea-core.exe",
  "$BuildDir\tools\rtsp-testsrc\rtsp-testsrc.exe"
)
foreach ($e in $exes) {
  if (-not (Test-Path $e)) { throw "missing $e" }
  Copy-Item $e $Out
}

& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --dir $Out "$Out\fovea.exe" "$Out\fovea-core.exe"
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

Copy-Item "$GstRoot\bin\*.dll" $Out
foreach ($tool in @("gst-inspect-1.0.exe", "gst-launch-1.0.exe")) { Copy-Item "$GstRoot\bin\$tool" $Out }
Copy-Item "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" "$Out\libexec\gstreamer-1.0\"

$plugins = @(
  "coreelements", "app", "videoconvertscale", "videorate", "videotestsrc", "videofilter",
  "rtsp", "rtp", "rtpmanager", "udp", "tcp",
  "videoparsersbad", "libav", "isomp4", "matroska", "multifile",
  "playback", "typefindfunctions", "pbtypes", "jpeg", "pango",
  "x264", "openh264", "mediafoundation", "d3d11", "nvcodec", "rtspclientsink", "autodetect"
)
$missing = @()
foreach ($p in $plugins) {
  $src = "$GstRoot\lib\gstreamer-1.0\gst$p.dll"
  if (Test-Path $src) { Copy-Item $src "$Out\lib\gstreamer-1.0\" } else { $missing += $p }
}
if ($missing.Count) { Write-Warning "plugins not in this GStreamer build: $($missing -join ', ')" }

$crt = Get-ChildItem -Path "$env:VCToolsRedistDir\x64" -Directory -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($crt) { Copy-Item "$($crt.FullName)\*.dll" $Out } else { Write-Warning "MSVC runtime DLLs not found; target machines need the VC++ 2015-2022 redistributable" }

foreach ($f in @("LICENSE", "THIRD_PARTY_NOTICES.md")) { if (Test-Path $f) { Copy-Item $f $Out } }
Copy-Item "scripts\verify_m1.py" $Out
@"
Fovea (development build)

Run fovea.exe. It starts fovea-core.exe in the background; closing the window
keeps recording running. Use the title bar menu to stop the service.

Data: %LOCALAPPDATA%\Fovea (set FOVEA_DATA_DIR to change it).

Local test camera without hardware:
  rtsp-testsrc.exe --pattern ball            -> rtsp://127.0.0.1:8554/test
  rtsp-testsrc.exe --webcam                  -> this PC's webcam over RTSP
  rtsp-testsrc.exe --list-devices
Add the URL in the console with "+ Add".

Self test (Python 3.10+ on PATH):
  python verify_m1.py --bin-dir . --flat
"@ | Out-File -FilePath "$Out\README.txt" -Encoding utf8
Write-Host "packaged $Out"
