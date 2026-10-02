[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows-release/bin',
    [string]$OutputDirectory = '',
    [string]$Python = 'python',
    [ValidateRange(1, 3600)][int]$DurationSeconds = 60,
    [ValidateRange(0, 60)][int]$WarmupSeconds = 5,
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateSet('direct', 'relay', 'both')][string]$Mode = 'both',
    [ValidateRange(0, 100)][double]$LossPercent = 0,
    [UInt64]$Seed = 1001,
    [switch]$EnableRtcp,
    [ValidateRange(1, 65535)][int]$PublisherRtcpPort = 5005,
    [ValidateRange(1, 65535)][int]$ReceiverRtcpPort = 5007,
    [ValidateRange(1, 65535)][int]$ReceiverPort = 5006,
    [ValidateRange(1, 65535)][int]$RelayPort = 5004,
    [ValidateRange(1, 32767)][int]$ReorderPackets = 512,
    [string]$Display = 'primary'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$bin = (Resolve-Path (Join-Path $repo $BuildDirectory)).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('experiments/latency-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if ($ReceiverPort -eq $RelayPort) { throw 'Receiver and relay ports must differ' }
if ($LossPercent -gt 0 -and $Mode -ne 'relay') { throw 'Loss requires relay mode' }
if ($EnableRtcp -and $PublisherRtcpPort -eq $ReceiverRtcpPort) { throw 'RTCP ports must differ' }
$lossText = $LossPercent.ToString([Globalization.CultureInfo]::InvariantCulture)
$root = New-Item -ItemType Directory -Path $OutputDirectory
$oldTrace = $env:SEMILIVE_LATENCY_TRACE
$oldRun = $env:SEMILIVE_LATENCY_RUN_ID
$modes = if ($Mode -eq 'both') { @('direct', 'relay') } else { @($Mode) }
try {
    foreach ($kind in $modes) {
        for ($index = 1; $index -le $Repetitions; $index++) {
            $run = [guid]::NewGuid().ToString()
            $dir = (New-Item -ItemType Directory -Path (Join-Path $root.FullName "$kind-$index")).FullName
            $env:SEMILIVE_LATENCY_RUN_ID = $run
            $processes = @()
            $seconds = $DurationSeconds + $WarmupSeconds
            $runSeed = $Seed + $index - 1
            try {
                $env:SEMILIVE_LATENCY_TRACE = Join-Path $dir 'receiver.csv'
                $receiverArgs = @('--bind-address', '127.0.0.1', '--bind-port', "$ReceiverPort",
                    '--rtp-reorder-packets', "$ReorderPackets",
                    '--output-mode', 'file', '--output', 'received.h264', '--stats-json', 'receiver.json',
                    '--run-duration-seconds', "$($seconds + 5)")
                if ($EnableRtcp) {
                    $receiverArgs += @('--rtcp-bind-address', '127.0.0.1', '--rtcp-bind-port', "$ReceiverRtcpPort",
                        '--rtcp-peer-address', '127.0.0.1', '--rtcp-peer-port', "$PublisherRtcpPort")
                }
                $receiver = Start-Process -FilePath (Join-Path $bin 'semilive_receiver.exe') -ArgumentList $receiverArgs `
                    -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
                    -RedirectStandardOutput (Join-Path $dir 'receiver.stdout.log') `
                    -RedirectStandardError (Join-Path $dir 'receiver.stderr.log')
                $null = $receiver.Handle
                $processes += $receiver
                Start-Sleep -Milliseconds 750
                if ($receiver.HasExited) { throw 'Receiver failed during startup' }
                $port = $ReceiverPort
                $relayArgs = @()
                if ($kind -eq 'relay') {
                    $env:SEMILIVE_LATENCY_TRACE = $null
                    $relayArgs = @('--bind-address', '127.0.0.1', '--bind-port', "$RelayPort",
                        '--forward-address', '127.0.0.1', '--forward-port', "$ReceiverPort",
                        '--loss-percent', $lossText, '--seed', "$runSeed", '--stats-json', 'relay.json',
                        '--run-duration-seconds', "$($seconds + 3)")
                    $relay = Start-Process -FilePath (Join-Path $bin 'semilive_relay.exe') -ArgumentList $relayArgs `
                        -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
                        -RedirectStandardOutput (Join-Path $dir 'relay.stdout.log') `
                        -RedirectStandardError (Join-Path $dir 'relay.stderr.log')
                    $null = $relay.Handle
                    $processes += $relay
                    Start-Sleep -Milliseconds 750
                    if ($relay.HasExited) { throw 'Relay failed during startup' }
                    $port = $RelayPort
                }
                $env:SEMILIVE_LATENCY_TRACE = Join-Path $dir 'publisher.csv'
                $publisherArgs = @('--rtp-address', '127.0.0.1', '--rtp-port', "$port", '--display', $Display,
                    '--stats-json', 'publisher.json', '--run-duration-seconds', "$seconds")
                if ($EnableRtcp) {
                    $publisherArgs += @('--rtcp-bind-address', '127.0.0.1', '--rtcp-bind-port', "$PublisherRtcpPort",
                        '--rtcp-peer-address', '127.0.0.1', '--rtcp-peer-port', "$ReceiverRtcpPort")
                }
                @{run_id=$run; mode=$kind; warmup_seconds=$WarmupSeconds; duration_seconds=$DurationSeconds;
                    publisher_args=$publisherArgs; receiver_args=$receiverArgs; relay_args=$relayArgs;
                    build_directory=$bin; rtcp_enabled=[bool]$EnableRtcp; loss_percent=$LossPercent; seed=$runSeed;
                    reorder_packets=$ReorderPackets} | ConvertTo-Json -Depth 5 |
                    Set-Content -LiteralPath (Join-Path $dir 'manifest.json') -Encoding UTF8
                $publisher = Start-Process -FilePath (Join-Path $bin 'semilive_publisher.exe') -ArgumentList $publisherArgs `
                    -WorkingDirectory $dir -WindowStyle Hidden -PassThru `
                    -RedirectStandardOutput (Join-Path $dir 'publisher.stdout.log') `
                    -RedirectStandardError (Join-Path $dir 'publisher.stderr.log')
                $null = $publisher.Handle
                $processes += $publisher
                foreach ($process in @($publisher) + @($processes | Where-Object { $_.Id -ne $publisher.Id })) {
                    if (!$process.WaitForExit(($seconds + 30) * 1000)) { throw 'Experiment timed out' }
                    $process.Refresh()
                    if ($process.ExitCode -ne 0) { throw "Process failed: $($process.ExitCode); inspect $dir" }
                }
                foreach ($role in @('publisher', 'receiver')) {
                    $report = Get-Content -LiteralPath (Join-Path $dir "$role.json") -Raw | ConvertFrom-Json
                    if (!$report.session.run_succeeded) { throw "$role reported failure" }
                }
                $analysisArgs = @()
                if ($LossPercent -gt 0) { $analysisArgs += '--allow-loss' }
                & $Python (Join-Path $PSScriptRoot 'analyze.py') (Join-Path $dir 'publisher.csv') `
                    (Join-Path $dir 'receiver.csv') --output $dir --warmup-seconds $WarmupSeconds `
                    --duration-seconds $DurationSeconds --quiet @analysisArgs
                if ($LASTEXITCODE -ne 0) { throw "Analysis failed: $dir" }
                $summary = Get-Content -LiteralPath (Join-Path $dir 'summary.json') -Raw | ConvertFrom-Json
                Write-Host "Completed $kind seed=$runSeed loss=$lossText% delivery=$($summary.delivery_percent)% p95=$($summary.metrics_ms.capture_to_ready_ms.p95)ms"
            } finally {
                foreach ($process in $processes) {
                    if (!$process.HasExited) { Stop-Process -Id $process.Id }
                    $process.Dispose()
                }
            }
        }
    }
} finally {
    $env:SEMILIVE_LATENCY_TRACE = $oldTrace
    $env:SEMILIVE_LATENCY_RUN_ID = $oldRun
}
