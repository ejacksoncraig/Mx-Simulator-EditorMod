# Copy the editor mod into the MX Simulator folder.
# If another opengl32.dll is there (MXS Shaders), it's renamed to opengl32_mxs.dll so the mod chain-loads it.
# Never overwrites anything it didn't put there. Undo with uninstall.ps1.
param(
    [string]$Game = "C:\Program Files (x86)\MX Simulator",
    [string]$Dll = (Join-Path $PSScriptRoot "build\opengl32.dll")
)
$ErrorActionPreference = "Stop"

function Test-OurDll($path) {
    # Ours has the extra export "mxmod_version"; look for the name in the file.
    return (Test-Path $path) -and ([IO.File]::ReadAllText($path, [Text.Encoding]::ASCII) -match "mxmod_version")
}

if (-not (Test-Path $Dll)) { throw "No build at $Dll. Run build.bat first." }
if (-not (Test-Path (Join-Path $Game "mx.exe"))) { throw "mx.exe not found in $Game" }
if (Get-Process mx -ErrorAction SilentlyContinue) { throw "Close MX Simulator first." }

$target = Join-Path $Game "opengl32.dll"
$chained = Join-Path $Game "opengl32_mxs.dll"

if ((Test-Path $target) -and -not (Test-OurDll $target)) {
    if (Test-Path $chained) { throw "Both $target and $chained exist and neither is the editor mod. Sort them out by hand." }
    Rename-Item $target "opengl32_mxs.dll"
    Write-Host "Kept the existing opengl32.dll (MXS Shaders?) as opengl32_mxs.dll; the mod loads it."
}

Copy-Item $Dll $target -Force
Write-Host "Installed $target"
Write-Host "Offline only: start the editor with  mx.exe --editor. Log: $env:LOCALAPPDATA\MX Simulator\mxtools\log.txt"
