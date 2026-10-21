# Builds the .spk in Docker on Windows.
# Usage: .\build.ps1 [-Arch x86_64|aarch64|armv7] [-SpkArch VALUE]
param(
    [ValidateSet('x86_64', 'aarch64', 'armv7')]
    [string]$Arch = 'x86_64',

    [string]$SpkArch = ''
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$image = 'ch340ups-builder'

docker build -t $image .
if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }

$dockerArgs = @(
    'run', '--rm',
    '-v', "$($PWD.Path):/src",
    '-e', "TARGET_ARCH=$Arch"
)
if ($SpkArch) { $dockerArgs += @('-e', "SPK_ARCH=$SpkArch") }
$dockerArgs += $image

docker @dockerArgs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
