param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$install = Join-Path $root 'out/tools/ffmpeg'
$encoder = Join-Path $install 'bin/ffmpeg.exe'
if (Test-Path -LiteralPath $encoder) { Write-Output $encoder; exit 0 }
New-Item -ItemType Directory -Path $install -Force | Out-Null
$url = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
$archive = Join-Path $install 'ffmpeg.zip'
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
$expected = ((Invoke-WebRequest -Uri ($url + '.sha256') -UseBasicParsing).Content -split '\s+')[0]
if ($expected -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    throw 'FFmpeg archive SHA256 verification failed'
}
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $install 'package') -Force
$package = Get-ChildItem (Join-Path $install 'package') -Directory | Select-Object -First 1
Copy-Item -LiteralPath (Join-Path $package.FullName 'bin') -Destination $install -Recurse
@{url=$url;sha256=$expected;installedAt=(Get-Date -Format o)} | ConvertTo-Json | Set-Content (Join-Path $install 'download.json') -Encoding UTF8
Write-Output $encoder
