[CmdletBinding()]
param(
    [ValidateSet('Base', 'Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$Bds = '',
    [string]$Python = '',
    [switch]$Clean,
    [switch]$ObjectsOnly
)
$ErrorActionPreference = 'Stop'
# The IDE can invoke the Base inheritance node; use Debug output and flags.
switch ($Configuration.ToLowerInvariant()) {
    'base' { $Configuration = 'Debug' }
    'debug' { $Configuration = 'Debug' }
    'release' { $Configuration = 'Release' }
}
$buildArguments = @((Join-Path $PSScriptRoot 'build.py'), '--configuration', $Configuration)
if ($Bds) { $buildArguments += @('--bds', $Bds) }
if ($Clean) { $buildArguments += '--clean' }
if ($ObjectsOnly) { $buildArguments += '--objects-only' }
if ($Python) {
    $pythonExecutable = $Python
} else {
    $launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($launcher) {
        $pythonExecutable = $launcher.Source
        $buildArguments = @('-3') + $buildArguments
    } else {
        $launcher = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $launcher) { throw 'Python 3 is required. Pass -Python with its full executable path.' }
        $pythonExecutable = $launcher.Source
    }
}
function Quote-NativeArgument([string]$Value) {
    # Do not quote ordinary switches: the Python launcher parses its selector first.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    # Windows argv escaping: preserve quotes and trailing backslashes inside quotes.
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $pythonExecutable
$start.Arguments = (($buildArguments | ForEach-Object { Quote-NativeArgument $_ }) -join ' ')
$start.WorkingDirectory = $PSScriptRoot
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw 'Could not start the Python build process.' }
    # Drain both pipes concurrently so compiler diagnostics cannot block the child.
    $standardOutput = $process.StandardOutput.ReadToEndAsync()
    $standardError = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [Console]::Out.Write($standardOutput.Result)
    [Console]::Error.Write($standardError.Result)
    $exitCode = $process.ExitCode
} finally {
    $process.Dispose()
}
if ($exitCode -ne 0) { throw "OdinBCC build failed with exit code $exitCode" }
