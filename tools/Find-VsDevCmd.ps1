# Locate the Visual Studio developer command environment without relying on a
# machine-specific installation path.
function Find-VsDevCmdPath {
    $vswherePaths = @()
    $vswhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($vswhereCommand) {
        $vswherePaths += $vswhereCommand.Source
    }

    foreach ($programRoot in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:ProgramW6432)) {
        if ([string]::IsNullOrWhiteSpace($programRoot)) { continue }
        $candidate = Join-Path $programRoot "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $vswherePaths += $candidate
        }
    }

    foreach ($vswhere in @($vswherePaths | Where-Object { $_ } | Select-Object -Unique)) {
        try {
            $installationPath = & $vswhere -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath 2>$null | Select-Object -First 1
            if ($installationPath) {
                $candidate = Join-Path ([string]$installationPath).Trim() `
                    "Common7\Tools\VsDevCmd.bat"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return $candidate
                }
            }
        } catch {
            # Try the environment and conventional installation locations below.
        }
    }

    foreach ($candidate in @(
        (if ($env:VSINSTALLDIR) { Join-Path $env:VSINSTALLDIR "Common7\Tools\VsDevCmd.bat" }),
        (if ($env:VS170COMNTOOLS) { Join-Path $env:VS170COMNTOOLS "VsDevCmd.bat" })
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    foreach ($programRoot in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:ProgramW6432)) {
        if ([string]::IsNullOrWhiteSpace($programRoot)) { continue }
        $matches = Get-ChildItem -Path (Join-Path $programRoot `
            "Microsoft Visual Studio\20*\*\Common7\Tools\VsDevCmd.bat") `
            -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending
        if ($matches) {
            return $matches[0].FullName
        }
    }

    return $null
}
