# Remove the editor mod and put back any opengl32.dll it chained (MXS Shaders).
param([string]$Game = "C:\Program Files (x86)\MX Simulator")
$ErrorActionPreference = "Stop"
if (Get-Process mx -ErrorAction SilentlyContinue) { throw "Close MX Simulator first." }

$target = Join-Path $Game "opengl32.dll"
$chained = Join-Path $Game "opengl32_mxs.dll"
if ((Test-Path $target) -and ([IO.File]::ReadAllText($target, [Text.Encoding]::ASCII) -match "mxmod_version")) {
    Remove-Item $target
    Write-Host "Removed the editor mod."
} elseif (Test-Path $target) {
    throw "$target isn't the editor mod; leaving it alone."
}
if (Test-Path $chained) {
    Rename-Item $chained "opengl32.dll"
    Write-Host "Restored opengl32_mxs.dll as opengl32.dll."
}
