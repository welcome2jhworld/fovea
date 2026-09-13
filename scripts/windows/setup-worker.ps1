# Creates worker\.venv and installs the worker with its torch extra. Model weights are not downloaded here.
# Torch and torchvision come from the CUDA 12.8 wheel index when nvidia-smi lists a GPU, from the CPU
# wheel index otherwise (or with -Cpu); the worker install that follows keeps them.
# Uses uv when it is on PATH, else the Python launcher (py -<version>) or python with the venv module.
param(
  [string]$PythonVersion = "3.12",
  [switch]$Cpu
)
$ErrorActionPreference = "Stop"

function Invoke-Native([string]$File, [string[]]$Arguments) {
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

$Worker = (Resolve-Path (Join-Path $PSScriptRoot "..\..\worker")).Path
$Venv = Join-Path $Worker ".venv"
$VenvPython = Join-Path $Venv "Scripts\python.exe"
$Uv = Get-Command uv -ErrorAction SilentlyContinue

function Install-Packages([string[]]$Packages) {
  if ($Uv) {
    Invoke-Native $Uv.Source (@("pip", "install", "--python", $VenvPython) + $Packages)
  } else {
    Invoke-Native $VenvPython (@("-m", "pip", "install") + $Packages)
  }
}

$Gpu = $false
if (-not $Cpu) {
  $Smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if ($Smi) {
    try {
      $GpuList = & $Smi.Source -L 2>$null
      $Gpu = ($LASTEXITCODE -eq 0) -and (@($GpuList | Where-Object { $_ -match "^GPU \d+" }).Count -gt 0)
    } catch {
      Write-Warning "nvidia-smi failed: $_"
    }
    if ($Gpu) { $GpuList | ForEach-Object { Write-Host $_ } }
  }
}
$TorchIndex = if ($Gpu) { "https://download.pytorch.org/whl/cu128" } else { "https://download.pytorch.org/whl/cpu" }

if (Test-Path $VenvPython) {
  Write-Host "reusing $Venv"
} elseif ($Uv) {
  Invoke-Native $Uv.Source @("venv", $Venv, "--python", $PythonVersion)
} else {
  $Launcher = Get-Command py -ErrorAction SilentlyContinue
  $Python = Get-Command python -ErrorAction SilentlyContinue
  if ($Launcher) {
    Invoke-Native $Launcher.Source @("-$PythonVersion", "-m", "venv", $Venv)
  } elseif ($Python) {
    Invoke-Native $Python.Source @("-m", "venv", $Venv)
  } else {
    throw "no uv, py launcher or python on PATH; install Python $PythonVersion or uv first"
  }
}
if (-not $Uv) { Invoke-Native $VenvPython @("-m", "pip", "install", "--upgrade", "pip") }

Write-Host "torch wheels: $TorchIndex"
Install-Packages @("--index-url", $TorchIndex, "torch>=2.7", "torchvision>=0.22")
Install-Packages @("-e", "$($Worker)[torch]")

$Probe = "import torch; print(torch.__version__, torch.version.cuda or 'cpu', torch.cuda.is_available())"
$TorchInfo = (& $VenvPython -c $Probe) -split " "
if ($LASTEXITCODE -ne 0) { throw "torch does not import in $Venv" }
Write-Host "torch $($TorchInfo[0]) build $($TorchInfo[1]) cuda_available $($TorchInfo[2])"
if ($Gpu -and $TorchInfo[1] -eq "cpu") {
  throw "an NVIDIA GPU is present but torch is a CPU build; the worker install replaced it. Rerun, or install torch from $TorchIndex with --force-reinstall"
}

$Exe = Join-Path $Venv "Scripts\fovea-worker.exe"
if (-not (Test-Path $Exe)) { throw "fovea-worker.exe was not created in $Venv\Scripts" }
Write-Host "fovea-worker: $Exe"
