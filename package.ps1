# Build the zip to give someone the editor mod: opengl32.dll, install.bat, uninstall.bat, README.txt.
# Run build.bat first. Output: dist\mxtools-editor-mod-<version>.zip
$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\opengl32.dll"
if (-not (Test-Path $dll)) { throw "No build at $dll. Run build.bat first." }
$version = (Select-String -Path (Join-Path $PSScriptRoot "src\protocol.h") -Pattern 'MXMOD_VERSION "([^"]+)"').Matches[0].Groups[1].Value
$dist = Join-Path $PSScriptRoot "dist"
$stage = Join-Path $dist "mxtools-editor-mod"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
# The .bat files and the README with Windows line endings: cmd mishandles labels in LF-only batch files.
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot "package") -File) {
    $text = [IO.File]::ReadAllText($f.FullName) -replace "`r`n", "`n" -replace "`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $stage $f.Name), $text, [Text.Encoding]::ASCII)
}
$zip = Join-Path $dist "mxtools-editor-mod-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Built $zip"
