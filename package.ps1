# Build the players' zip: opengl32.dll, install.bat, uninstall.bat. A player extracts it and double-clicks install.bat.
#   powershell -ExecutionPolicy Bypass -File package.ps1
# Output: dist\mxtools-editor-mod-<version>.zip
$ErrorActionPreference = "Stop"

# Always build first, so the zip never carries an old DLL.
& (Join-Path $PSScriptRoot "build.bat")
if ($LASTEXITCODE -ne 0) { throw "build.bat failed." }

$dll = Join-Path $PSScriptRoot "build\opengl32.dll"
if (-not ([IO.File]::ReadAllText($dll, [Text.Encoding]::ASCII) -match "mxmod_version")) {
    throw "$dll isn't the editor mod (no mxmod_version export)."
}
$version = (Select-String -Path (Join-Path $PSScriptRoot "src\protocol.h") -Pattern 'MXMOD_VERSION "([^"]+)"').Matches[0].Groups[1].Value

$dist = Join-Path $PSScriptRoot "dist"
$stage = Join-Path $dist "mxtools-editor-mod"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
# The .bat files with Windows line endings: cmd mishandles labels in LF-only batch files.
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot "package") -Filter *.bat -File) {
    $text = [IO.File]::ReadAllText($f.FullName) -replace "`r`n", "`n" -replace "`n", "`r`n"
    [IO.File]::WriteAllText((Join-Path $stage $f.Name), $text, [Text.Encoding]::ASCII)
}

$zip = Join-Path $dist "mxtools-editor-mod-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Write-Host "Built $zip"
Write-Host "SHA-256: $hash  (publish this with the release so players can check the download)"
