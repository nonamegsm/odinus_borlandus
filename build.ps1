[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$Bds = '',
    [string]$Python = '',
    [switch]$Clean,
    [switch]$ObjectsOnly
)
$ErrorActionPreference = 'Stop'
$buildArguments = @((Join-Path $PSScriptRoot 'build.py'), '--configuration', $Configuration)
if ($Bds) { $buildArguments += @('--bds', $Bds) }
if ($Clean) { $buildArguments += '--clean' }
if ($ObjectsOnly) { $buildArguments += '--objects-only' }
if ($Python) {
    & $Python @buildArguments
} elseif (Get-Command py.exe -ErrorAction SilentlyContinue) {
    & py.exe -3 @buildArguments
} elseif (Get-Command python.exe -ErrorAction SilentlyContinue) {
    & python.exe @buildArguments
} else {
    throw 'Python 3 is required. Pass -Python with its full executable path.'
}
if ($LASTEXITCODE -ne 0) { throw "OdinBCC build failed with exit code $LASTEXITCODE" }
