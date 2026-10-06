# Build only the isolated Windows dependency package. Does not clean Game/Editor.
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')][string]$Configuration='Release',
    [switch]$CleanFirst
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Find-VsDevCmd.ps1')
$vs=Find-VsDevCmdPath
if(-not $vs){throw 'Visual Studio developer environment not found'}
$preset=if($Configuration -eq 'Debug'){'x64-debug'}else{'x64-release'}
# Main configure prepares and imports the matching dependency package.
cmd /c "`"$vs`" -arch=x64 -host_arch=x64 >nul 2>&1 && cd /d `"$root`" && cmake --preset $preset -DMIKAN_REBUILD_PC_DEPENDENCIES=ON"
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
if($CleanFirst){
    $cache=Join-Path $root "out\build\$preset\CMakeCache.txt"
    $line=[IO.File]::ReadAllLines($cache,[Text.UTF8Encoding]::new($false)) | Where-Object {$_ -like 'MIKAN_PC_DEPENDENCY_DIR:PATH=*'} | Select-Object -First 1
    if(-not $line){throw 'PC dependency package path missing from CMake cache'}
    $dir=$line.Substring($line.IndexOf('=')+1)
    $resolved=[IO.Path]::GetFullPath($dir)
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'out\pc-dependencies'))+[IO.Path]::DirectorySeparatorChar
    if(-not $resolved.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Dependency path outside the isolated package directory'}
    cmd /c "`"$vs`" -arch=x64 -host_arch=x64 >nul 2>&1 && cmake --build `"$resolved`" --clean-first --parallel 8"
    exit $LASTEXITCODE
}
Write-Host 'PC dependencies are up to date. Game/Editor were not rebuilt.'
