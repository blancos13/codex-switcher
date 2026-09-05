param([switch]$Run)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    cmake --preset windows
    if($LASTEXITCODE -ne 0){throw 'CMake configuration failed.'}
    cmake --build --preset windows --parallel
    if($LASTEXITCODE -ne 0){throw 'Build failed.'}
    $executable=Join-Path $projectRoot 'build\windows\bin\CodexSwitcher.exe'
    Write-Host "Built: $executable"
    if($Run){Start-Process -FilePath $executable -WorkingDirectory (Split-Path -Parent $executable)}
} finally { Pop-Location }
