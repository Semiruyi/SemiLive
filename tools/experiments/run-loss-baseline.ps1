[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows-release/bin',
    [string]$OutputDirectory = '',
    [string]$Python = 'python',
    [ValidateRange(1, 3600)][int]$DurationSeconds = 20,
    [ValidateRange(0, 60)][int]$WarmupSeconds = 3,
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [double[]]$LossPercents = @(0, 0.1, 0.5, 1, 3, 5, 10),
    [ValidateRange(1, 32767)][int]$ReorderPackets = 512,
    [switch]$DisableRtcp
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (!$OutputDirectory) {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
    $OutputDirectory = Join-Path $repo ('experiments/latency-loss-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$root = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
foreach ($loss in $LossPercents) {
    $text = $loss.ToString([Globalization.CultureInfo]::InvariantCulture)
    Write-Host "Starting loss=$text%, seeds=1001..$($Repetitions + 1000)"
    & (Join-Path $PSScriptRoot 'run-experiment.ps1') -BuildDirectory $BuildDirectory `
        -OutputDirectory (Join-Path $root "loss-$text") -Python $Python -Mode relay `
        -LossPercent $loss -Seed 1001 -EnableRtcp:(!$DisableRtcp) -ReorderPackets $ReorderPackets `
        -DurationSeconds $DurationSeconds -WarmupSeconds $WarmupSeconds -Repetitions $Repetitions
}
& $Python (Join-Path $PSScriptRoot 'summarize.py') $root
if ($LASTEXITCODE -ne 0) { throw 'Loss summary failed' }
