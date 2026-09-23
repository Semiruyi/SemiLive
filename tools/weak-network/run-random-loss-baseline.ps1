[CmdletBinding()]
param(
    [string]$BuildDirectory = "build/windows-debug/bin",
    [string]$OutputDirectory = "",
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 120,
    [double[]]$LossPercents = @(0, 0.1, 0.5, 1, 3, 5, 10),
    [UInt64[]]$Seeds = @(1001, 1002, 1003),
    [ValidateRange(1, 65535)]
    [int]$RelayPort = 5004,
    [ValidateRange(1, 65535)]
    [int]$ReceiverPort = 5006,
    [switch]$EnableRtcp,
    [ValidateRange(1, 65535)]
    [int]$PublisherRtcpPort = 5005,
    [ValidateRange(1, 65535)]
    [int]$ReceiverRtcpPort = 5007,
    [ValidateRange(100, 60000)]
    [int]$RtcpReportIntervalMilliseconds = 1000,
    [ValidateRange(0, 60000)]
    [int]$StartupDelayMilliseconds = 750,
    [string]$Display = "primary",
    [switch]$KeepMedia,
    [switch]$Resume,
    [switch]$SummarizeOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Median {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-Maximum {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }
    return ($Values | Measure-Object -Maximum).Maximum
}

function Get-Minimum {
    param([double[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return $null
    }
    return ($Values | Measure-Object -Minimum).Minimum
}

function Get-RequiredJson {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing report: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-OptionalProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Wait-ProcessAndGetExitCode {
    param([Diagnostics.Process]$Process)

    $Process.WaitForExit()
    $Process.Refresh()
    try {
        $exitCode = $Process.ExitCode
    } catch {
        return $null
    }
    if ($null -eq $exitCode) {
        return $null
    }
    return [int]$exitCode
}

function Test-SuccessfulReport {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $report = Get-RequiredJson $Path
        return [bool]$report.session.run_succeeded
    } catch {
        return $false
    }
}

function Get-ErrorTail {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "stderr log is missing"
    }
    $lines = @((Get-Content -LiteralPath $Path -Tail 5))
    if ($lines.Count -eq 0) {
        return "stderr log is empty"
    }
    return $lines -join " | "
}

function Test-CompleteRun {
    param(
        [string]$Path,
        [bool]$RequireRtcp = $false
    )

    foreach ($name in @("publisher.json", "relay.json", "receiver.json", "run.json")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $name) -PathType Leaf)) {
            return $false
        }
    }
    try {
        $publisher = Get-RequiredJson (Join-Path $Path "publisher.json")
        $relay = Get-RequiredJson (Join-Path $Path "relay.json")
        $receiver = Get-RequiredJson (Join-Path $Path "receiver.json")
        $run = Get-RequiredJson (Join-Path $Path "run.json")
        $succeeded = [bool]$publisher.session.run_succeeded -and
            [bool]$relay.session.run_succeeded -and
            [bool]$receiver.session.run_succeeded
        if (-not $succeeded) {
            return $false
        }
        if ($RequireRtcp -and
            ($null -eq (Get-OptionalProperty $publisher "rtcp") -or
             $null -eq (Get-OptionalProperty $receiver "rtcp"))) {
            return $false
        }
        return $true
    } catch {
        return $false
    }
}

function Get-RunRows {
    param([string]$Root)

    $rows = @()
    $runDirectories = @(Get-ChildItem -LiteralPath (Join-Path $Root "raw") `
        -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
    foreach ($runDirectory in $runDirectories) {
        $publisher = Get-RequiredJson (Join-Path $runDirectory.FullName "publisher.json")
        $relay = Get-RequiredJson (Join-Path $runDirectory.FullName "relay.json")
        $receiver = Get-RequiredJson (Join-Path $runDirectory.FullName "receiver.json")
        $runConfig = Get-RequiredJson (Join-Path $runDirectory.FullName "run.json")

        if ($relay.application -ne "semilive_relay") {
            throw "Unexpected relay report in $($runDirectory.FullName)"
        }
        if ([UInt64]$relay.config.impairment.seed -ne [UInt64]$runConfig.seed) {
            throw "Relay seed does not match run config in $($runDirectory.FullName)"
        }
        if ([Math]::Abs([double]$relay.config.impairment.loss_percent -
                        [double]$runConfig.loss_percent) -gt 0.0000001) {
            throw "Relay loss setting does not match run config in $($runDirectory.FullName)"
        }

        $traffic = $relay.traffic
        if ([UInt64]$traffic.received_datagrams -ne
            ([UInt64]$traffic.forwarded_datagrams + [UInt64]$traffic.dropped_datagrams)) {
            throw "Relay datagram conservation failed in $($runDirectory.FullName)"
        }
        if ([UInt64]$traffic.received_bytes -ne
            ([UInt64]$traffic.forwarded_bytes + [UInt64]$traffic.dropped_bytes)) {
            throw "Relay byte conservation failed in $($runDirectory.FullName)"
        }

        $reorder = $receiver.rtp.reorder
        $assembler = $receiver.h264.access_unit_assembler
        $recovery = $receiver.h264.recovery
        $sessionDurationMs = [double]$receiver.session.duration_ms
        $receivedAndLost = [double]$reorder.received_packets +
            [double]$reorder.confirmed_lost_packets
        $receiverLossPercent = if ($receivedAndLost -gt 0) {
            100.0 * [double]$reorder.confirmed_lost_packets / $receivedAndLost
        } else { 0.0 }
        $stallsPerMinute = if ($sessionDurationMs -gt 0) {
            60000.0 * [double]$receiver.session.output_stall_events /
                $sessionDurationMs
        } else { 0.0 }
        $averageRecoveryMs = if ([UInt64]$recovery.episodes_completed -gt 0) {
            [double]$recovery.wait_total_ms /
                [double]$recovery.episodes_completed
        } else { $null }
        $publisherAccessUnits = [UInt64]$publisher.encoder.submitted_access_units
        $submittedAccessUnits = [UInt64]$receiver.output.submitted_access_units
        $outputDeliveryRatioPercent = if ($publisherAccessUnits -gt 0) {
            100.0 * [double]$submittedAccessUnits /
                [double]$publisherAccessUnits
        } else { 0.0 }
        $recoveryCompletionRatePercent = if ([UInt64]$recovery.episodes_started -gt 0) {
            100.0 * [double]$recovery.episodes_completed /
                [double]$recovery.episodes_started
        } else { $null }
        $publisherRtcp = Get-OptionalProperty $publisher "rtcp"
        $receiverRtcp = Get-OptionalProperty $receiver "rtcp"
        $publisherFractionLostPercentValue = Get-OptionalProperty `
            $publisherRtcp "reported_fraction_lost_percent"
        $publisherFractionLostRaw = Get-OptionalProperty `
            $publisherRtcp "reported_fraction_lost"
        $publisherFractionLostPercent = if ($null -ne
            $publisherFractionLostPercentValue) {
            [double]$publisherFractionLostPercentValue
        } elseif ($null -ne $publisherFractionLostRaw) {
            100.0 * [double]$publisherFractionLostRaw / 256.0
        } else { $null }
        $receiverFractionLostPercentValue = Get-OptionalProperty `
            $receiverRtcp "current_fraction_lost_percent"
        $receiverFractionLostRaw = Get-OptionalProperty `
            $receiverRtcp "current_fraction_lost"
        $receiverFractionLostPercent = if ($null -ne
            $receiverFractionLostPercentValue) {
            [double]$receiverFractionLostPercentValue
        } elseif ($null -ne $receiverFractionLostRaw) {
            100.0 * [double]$receiverFractionLostRaw / 256.0
        } else { $null }

        $rows += [PSCustomObject][ordered]@{
            run = $runDirectory.Name
            loss_percent = [double]$runConfig.loss_percent
            seed = [UInt64]$runConfig.seed
            duration_seconds = [int]$runConfig.duration_seconds
            publisher_exit_code = $runConfig.exit_codes.publisher
            relay_exit_code = $runConfig.exit_codes.relay
            receiver_exit_code = $runConfig.exit_codes.receiver
            publisher_access_units = $publisherAccessUnits
            publisher_datagrams = [UInt64]$publisher.rtp.media_datagrams
            publisher_rtcp_sender_reports_sent = if ($null -ne $publisherRtcp) { [UInt64]$publisherRtcp.sender_reports_sent } else { $null }
            publisher_rtcp_receiver_reports_received = if ($null -ne $publisherRtcp) { [UInt64]$publisherRtcp.receiver_reports_received } else { $null }
            publisher_rtcp_rtt_samples = if ($null -ne $publisherRtcp) { [UInt64]$publisherRtcp.rtt_samples } else { $null }
            publisher_rtcp_current_rtt_ms = if ($null -ne $publisherRtcp) { $publisherRtcp.current_rtt_ms } else { $null }
            publisher_rtcp_reported_fraction_lost_percent = $publisherFractionLostPercent
            publisher_rtcp_reported_cumulative_lost = if ($null -ne $publisherRtcp) { $publisherRtcp.reported_cumulative_lost } else { $null }
            publisher_rtcp_reported_jitter_rtp_ticks = if ($null -ne $publisherRtcp) { $publisherRtcp.reported_jitter_rtp_ticks } else { $null }
            relay_received_datagrams = [UInt64]$traffic.received_datagrams
            relay_forwarded_datagrams = [UInt64]$traffic.forwarded_datagrams
            relay_dropped_datagrams = [UInt64]$traffic.dropped_datagrams
            relay_actual_loss_percent = [double]$traffic.actual_loss_percent
            receiver_received_packets = [UInt64]$reorder.received_packets
            receiver_confirmed_lost_packets = [UInt64]$reorder.confirmed_lost_packets
            receiver_confirmed_loss_percent = $receiverLossPercent
            receiver_rtcp_sender_reports_received = if ($null -ne $receiverRtcp) { [UInt64]$receiverRtcp.sender_reports_received } else { $null }
            receiver_rtcp_receiver_reports_sent = if ($null -ne $receiverRtcp) { [UInt64]$receiverRtcp.receiver_reports_sent } else { $null }
            receiver_rtcp_fraction_lost_percent = $receiverFractionLostPercent
            receiver_rtcp_cumulative_lost = if ($null -ne $receiverRtcp) { $receiverRtcp.cumulative_lost } else { $null }
            receiver_rtcp_interarrival_jitter_rtp_ticks = if ($null -ne $receiverRtcp) { $receiverRtcp.interarrival_jitter_rtp_ticks } else { $null }
            discarded_access_units = [UInt64]$assembler.discarded_access_units
            dropped_while_waiting = [UInt64]$recovery.dropped_while_waiting
            recovery_episodes_started = [UInt64]$recovery.episodes_started
            recovery_episodes_completed = [UInt64]$recovery.episodes_completed
            recovery_completion_rate_percent = $recoveryCompletionRatePercent
            recovery_average_ms = $averageRecoveryMs
            recovery_maximum_ms = [Int64]$recovery.wait_maximum_including_active_ms
            active_recovery_wait_ms = $recovery.active_wait_ms
            first_output_succeeded = $null -ne $receiver.session.first_output_delay_ms
            first_output_delay_ms = $receiver.session.first_output_delay_ms
            maximum_output_gap_ms = $receiver.session.maximum_output_gap_ms
            terminal_output_gap_ms = $receiver.session.terminal_output_gap_ms
            ended_waiting_for_random_access =
                $recovery.state -eq "waiting_for_random_access"
            output_stall_events = [UInt64]$receiver.session.output_stall_events
            output_stall_excess_total_ms = [Int64]$receiver.session.output_stall_excess_total_ms
            output_stalls_per_minute = $stallsPerMinute
            submitted_access_units = $submittedAccessUnits
            output_delivery_ratio_percent = $outputDeliveryRatioPercent
            output_backpressure_drops = [UInt64]$receiver.output.backpressure_drops
            run_succeeded = [bool]($publisher.session.run_succeeded -and
                $relay.session.run_succeeded -and $receiver.session.run_succeeded)
        }
    }
    return $rows
}

function Write-Summaries {
    param([string]$Root)

    $rows = @(Get-RunRows $Root)
    if ($rows.Count -eq 0) {
        throw "No complete runs found under $(Join-Path $Root 'raw')"
    }
    $rows | Export-Csv -LiteralPath (Join-Path $Root "runs.csv") `
        -NoTypeInformation -Encoding UTF8

    $summary = @()
    foreach ($group in ($rows | Group-Object loss_percent | Sort-Object { [double]$_.Name })) {
        $validRuns = @($group.Group | Where-Object run_succeeded)
        $firstOutputRuns = @($validRuns | Where-Object first_output_succeeded)
        $endedWaitingRuns = @($validRuns | Where-Object ended_waiting_for_random_access)
        $terminalGapRuns = @($validRuns | Where-Object {
            $null -ne $_.terminal_output_gap_ms
        })
        $recoveryEpisodesStarted = ($validRuns |
            Measure-Object recovery_episodes_started -Sum).Sum
        $recoveryEpisodesCompleted = ($validRuns |
            Measure-Object recovery_episodes_completed -Sum).Sum
        $recoveryCompletionRate = if ($recoveryEpisodesStarted -gt 0) {
            100.0 * [double]$recoveryEpisodesCompleted /
                [double]$recoveryEpisodesStarted
        } else { $null }
        $summary += [PSCustomObject][ordered]@{
            loss_percent = [double]$group.Name
            runs = $group.Count
            successful_runs = $validRuns.Count
            relay_actual_loss_median_percent = Get-Median @($validRuns | ForEach-Object { [double]$_.relay_actual_loss_percent })
            output_delivery_ratio_median_percent = Get-Median @($validRuns | ForEach-Object { [double]$_.output_delivery_ratio_percent })
            output_delivery_ratio_worst_percent = Get-Minimum @($validRuns | ForEach-Object { [double]$_.output_delivery_ratio_percent })
            ended_waiting_rate_percent = if ($validRuns.Count -gt 0) { 100.0 * $endedWaitingRuns.Count / $validRuns.Count } else { $null }
            terminal_output_gap_median_ms = Get-Median @($terminalGapRuns | ForEach-Object { [double]$_.terminal_output_gap_ms })
            terminal_output_gap_worst_ms = Get-Maximum @($terminalGapRuns | ForEach-Object { [double]$_.terminal_output_gap_ms })
            first_output_success_rate_percent = if ($validRuns.Count -gt 0) { 100.0 * $firstOutputRuns.Count / $validRuns.Count } else { $null }
            first_output_delay_median_ms = Get-Median @($firstOutputRuns | ForEach-Object { [double]$_.first_output_delay_ms })
            recovery_completion_rate_percent = $recoveryCompletionRate
        }
    }
    $summary | Export-Csv -LiteralPath (Join-Path $Root "summary.csv") `
        -NoTypeInformation -Encoding UTF8
    Write-Host "Wrote $($rows.Count) run rows to $(Join-Path $Root 'runs.csv')"
    Write-Host "Wrote aggregate results to $(Join-Path $Root 'summary.csv')"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$outputDirectoryWasSpecified = $PSBoundParameters.ContainsKey("OutputDirectory")
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format "yyyy-MM-dd-HHmmss"
    $OutputDirectory = Join-Path $repoRoot "experiments/random-loss-baseline-$stamp"
} elseif (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if ($Resume -and $SummarizeOnly) {
    throw "Resume and SummarizeOnly cannot be combined"
}
if ($Resume) {
    if (-not $outputDirectoryWasSpecified) {
        throw "Resume requires OutputDirectory"
    }
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Experiment directory does not exist: $OutputDirectory"
    }
    $existingConfig = Get-RequiredJson (Join-Path $OutputDirectory "config.json")
    if ($existingConfig.experiment -ne "random-loss-baseline") {
        throw "OutputDirectory is not a random-loss baseline experiment"
    }
    foreach ($name in @("BuildDirectory", "DurationSeconds", "LossPercents",
                         "Seeds", "RelayPort", "ReceiverPort", "EnableRtcp",
                         "PublisherRtcpPort", "ReceiverRtcpPort",
                         "RtcpReportIntervalMilliseconds",
                         "StartupDelayMilliseconds", "Display", "KeepMedia")) {
        if ($PSBoundParameters.ContainsKey($name)) {
            throw "Resume reloads $name from config.json; do not specify it"
        }
    }
    $BuildDirectory = [string]$existingConfig.build_directory
    $DurationSeconds = [int]$existingConfig.duration_seconds
    $LossPercents = @($existingConfig.loss_percents | ForEach-Object { [double]$_ })
    $Seeds = @($existingConfig.seeds | ForEach-Object { [UInt64]$_ })
    $RelayPort = [int]$existingConfig.relay_port
    $ReceiverPort = [int]$existingConfig.receiver_port
    $configuredEnableRtcp = Get-OptionalProperty $existingConfig "enable_rtcp"
    $EnableRtcp = if ($null -ne $configuredEnableRtcp) {
        [bool]$configuredEnableRtcp
    } else {
        $false
    }
    if ($EnableRtcp) {
        $PublisherRtcpPort = [int]$existingConfig.publisher_rtcp_port
        $ReceiverRtcpPort = [int]$existingConfig.receiver_rtcp_port
        $RtcpReportIntervalMilliseconds =
            [int]$existingConfig.rtcp_report_interval_milliseconds
    }
    $StartupDelayMilliseconds = [int]$existingConfig.startup_delay_milliseconds
    $Display = [string]$existingConfig.display
}
$keepMediaFiles = if ($PSBoundParameters.ContainsKey("KeepMedia")) {
    [bool]$KeepMedia
} elseif ($Resume) {
    [bool]$existingConfig.keep_media
} else {
    $false
}

if ($SummarizeOnly) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Output directory does not exist: $OutputDirectory"
    }
    Write-Summaries $OutputDirectory
    exit 0
}

if ($RelayPort -eq $ReceiverPort) {
    throw "RelayPort and ReceiverPort must be different"
}
if ($EnableRtcp) {
    $configuredPorts = @($RelayPort, $ReceiverPort, $PublisherRtcpPort,
        $ReceiverRtcpPort)
    if (($configuredPorts | Select-Object -Unique).Count -ne
        $configuredPorts.Count) {
        throw "RTP and RTCP ports must all be different when RTCP is enabled"
    }
}
if ($LossPercents.Count -eq 0 -or $Seeds.Count -eq 0) {
    throw "LossPercents and Seeds must each contain at least one value"
}
foreach ($loss in $LossPercents) {
    if ([double]::IsNaN($loss) -or [double]::IsInfinity($loss) -or
        $loss -lt 0 -or $loss -gt 100) {
        throw "LossPercents values must be in 0..100"
    }
}
if (-not $Resume -and (Test-Path -LiteralPath $OutputDirectory)) {
    throw "Refusing to overwrite existing experiment directory: $OutputDirectory"
}

$resolvedBuildDirectory = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $repoRoot $BuildDirectory
}
$publisherExe = (Resolve-Path (Join-Path $resolvedBuildDirectory "semilive_publisher.exe")).Path
$relayExe = (Resolve-Path (Join-Path $resolvedBuildDirectory "semilive_relay.exe")).Path
$receiverExe = (Resolve-Path (Join-Path $resolvedBuildDirectory "semilive_receiver.exe")).Path

if ($Resume) {
    $rawDirectory = Get-Item -LiteralPath (Join-Path $OutputDirectory "raw")
} else {
    $null = New-Item -ItemType Directory -Path $OutputDirectory
    $rawDirectory = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory "raw")
    $commit = (& git -C $repoRoot rev-parse HEAD 2>$null)
    $experimentConfig = [PSCustomObject][ordered]@{
        schema_version = 2
        experiment = "random-loss-baseline"
        created_at = (Get-Date).ToUniversalTime().ToString("o")
        git_commit = if ($LASTEXITCODE -eq 0) { "$commit".Trim() } else { $null }
        build_directory = $resolvedBuildDirectory
        duration_seconds = $DurationSeconds
        loss_percents = @($LossPercents)
        seeds = @($Seeds)
        relay_port = $RelayPort
        receiver_port = $ReceiverPort
        enable_rtcp = [bool]$EnableRtcp
        publisher_rtcp_port = $PublisherRtcpPort
        receiver_rtcp_port = $ReceiverRtcpPort
        rtcp_report_interval_milliseconds = $RtcpReportIntervalMilliseconds
        startup_delay_milliseconds = $StartupDelayMilliseconds
        display = $Display
        keep_media = $keepMediaFiles
        operating_system = [Environment]::OSVersion.VersionString
        powershell_version = $PSVersionTable.PSVersion.ToString()
    }
    $experimentConfig | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $OutputDirectory "config.json") -Encoding UTF8
}

$startupPaddingSeconds = [int][Math]::Ceiling($StartupDelayMilliseconds / 1000.0)
$tailSeconds = 1
foreach ($loss in $LossPercents) {
    $lossText = ([double]$loss).ToString("0.####", [Globalization.CultureInfo]::InvariantCulture)
    $lossName = $lossText.Replace('.', '_')
    foreach ($seed in $Seeds) {
        $runName = "loss-$lossName-seed-$seed"
        $runPath = Join-Path $rawDirectory.FullName $runName
        if (Test-Path -LiteralPath $runPath -PathType Container) {
            if (Test-CompleteRun $runPath ([bool]$EnableRtcp)) {
                if (-not $keepMediaFiles) {
                    Remove-Item -LiteralPath (Join-Path $runPath "received.h264") `
                        -Force -ErrorAction SilentlyContinue
                }
                Write-Host "Skipping complete $runName"
                continue
            }
            if (-not $Resume) {
                throw "Run directory already exists but is incomplete: $runPath"
            }
            $failedRoot = Join-Path $OutputDirectory "failed"
            $null = New-Item -ItemType Directory -Path $failedRoot -Force
            $archiveName = "$runName-$(Get-Date -Format 'yyyyMMdd-HHmmssfff')"
            $archivePath = Join-Path $failedRoot $archiveName
            Move-Item -LiteralPath $runPath -Destination $archivePath
            Write-Host "Archived incomplete $runName to $archivePath"
        }
        $runDirectory = New-Item -ItemType Directory -Path $runPath
        Write-Host "Running $runName for $DurationSeconds seconds"

        $receiverDuration = $DurationSeconds + $tailSeconds +
            (2 * $startupPaddingSeconds)
        $relayDuration = $DurationSeconds + $tailSeconds +
            $startupPaddingSeconds
        $receiverArgs = @(
            "--bind-address", "127.0.0.1",
            "--bind-port", "$ReceiverPort",
            "--output-mode", "file",
            "--output", "received.h264",
            "--stats-json", "receiver.json",
            "--run-duration-seconds", "$receiverDuration"
        )
        $relayArgs = @(
            "--bind-address", "127.0.0.1",
            "--bind-port", "$RelayPort",
            "--forward-address", "127.0.0.1",
            "--forward-port", "$ReceiverPort",
            "--loss-percent", $lossText,
            "--seed", "$seed",
            "--stats-json", "relay.json",
            "--run-duration-seconds", "$relayDuration"
        )
        $publisherArgs = @(
            "--rtp-address", "127.0.0.1",
            "--rtp-port", "$RelayPort",
            "--display", $Display,
            "--stats-json", "publisher.json",
            "--run-duration-seconds", "$DurationSeconds"
        )
        if ($EnableRtcp) {
            $receiverArgs += @(
                "--rtcp-bind-address", "127.0.0.1",
                "--rtcp-bind-port", "$ReceiverRtcpPort",
                "--rtcp-peer-address", "127.0.0.1",
                "--rtcp-peer-port", "$PublisherRtcpPort",
                "--rtcp-report-interval-ms", "$RtcpReportIntervalMilliseconds"
            )
            $publisherArgs += @(
                "--rtcp-bind-address", "127.0.0.1",
                "--rtcp-bind-port", "$PublisherRtcpPort",
                "--rtcp-peer-address", "127.0.0.1",
                "--rtcp-peer-port", "$ReceiverRtcpPort",
                "--rtcp-report-interval-ms", "$RtcpReportIntervalMilliseconds"
            )
        }

        $receiverProcess = $null
        $relayProcess = $null
        $publisherProcess = $null
        $receiverExitCode = $null
        $relayExitCode = $null
        $publisherExitCode = $null
        try {
            $receiverProcess = Start-Process -FilePath $receiverExe `
                -ArgumentList $receiverArgs -WorkingDirectory $runDirectory.FullName `
                -RedirectStandardOutput (Join-Path $runDirectory.FullName "receiver.stdout.log") `
                -RedirectStandardError (Join-Path $runDirectory.FullName "receiver.stderr.log") `
                -NoNewWindow -PassThru
            $null = $receiverProcess.Handle
            Start-Sleep -Milliseconds $StartupDelayMilliseconds
            if ($receiverProcess.HasExited) {
                throw "Receiver exited during startup with code $($receiverProcess.ExitCode)"
            }

            $relayProcess = Start-Process -FilePath $relayExe `
                -ArgumentList $relayArgs -WorkingDirectory $runDirectory.FullName `
                -RedirectStandardOutput (Join-Path $runDirectory.FullName "relay.stdout.log") `
                -RedirectStandardError (Join-Path $runDirectory.FullName "relay.stderr.log") `
                -NoNewWindow -PassThru
            $null = $relayProcess.Handle
            Start-Sleep -Milliseconds $StartupDelayMilliseconds
            if ($relayProcess.HasExited) {
                throw "Relay exited during startup with code $($relayProcess.ExitCode)"
            }

            $publisherProcess = Start-Process -FilePath $publisherExe `
                -ArgumentList $publisherArgs -WorkingDirectory $runDirectory.FullName `
                -RedirectStandardOutput (Join-Path $runDirectory.FullName "publisher.stdout.log") `
                -RedirectStandardError (Join-Path $runDirectory.FullName "publisher.stderr.log") `
                -NoNewWindow -PassThru
            $null = $publisherProcess.Handle

            $publisherExitCode = Wait-ProcessAndGetExitCode $publisherProcess
            $publisherReportPath = Join-Path $runDirectory.FullName "publisher.json"
            $publisherExitFailed = $null -ne $publisherExitCode -and
                $publisherExitCode -ne 0
            if ($publisherExitFailed -or
                -not (Test-SuccessfulReport $publisherReportPath)) {
                $publisherErrorPath = Join-Path $runDirectory.FullName "publisher.stderr.log"
                $publisherError = Get-ErrorTail $publisherErrorPath
                throw "Publisher failed in $runName (exit=$publisherExitCode, " +
                    "report_exists=$(Test-Path -LiteralPath $publisherReportPath)): " +
                    $publisherError
            }
            $relayExitCode = Wait-ProcessAndGetExitCode $relayProcess
            $receiverExitCode = Wait-ProcessAndGetExitCode $receiverProcess
            $relayReportPath = Join-Path $runDirectory.FullName "relay.json"
            $receiverReportPath = Join-Path $runDirectory.FullName "receiver.json"
            $relayExitFailed = $null -ne $relayExitCode -and $relayExitCode -ne 0
            $receiverExitFailed = $null -ne $receiverExitCode -and
                $receiverExitCode -ne 0
            if ($relayExitFailed -or -not (Test-SuccessfulReport $relayReportPath)) {
                throw "Relay failed in $runName (exit=$relayExitCode): " +
                    (Get-ErrorTail (Join-Path $runDirectory.FullName "relay.stderr.log"))
            }
            if ($receiverExitFailed -or
                -not (Test-SuccessfulReport $receiverReportPath)) {
                throw "Receiver failed in $runName (exit=$receiverExitCode): " +
                    (Get-ErrorTail (Join-Path $runDirectory.FullName "receiver.stderr.log"))
            }

            $runConfig = [PSCustomObject][ordered]@{
                schema_version = 1
                run = $runName
                loss_percent = [double]$loss
                seed = [UInt64]$seed
                duration_seconds = $DurationSeconds
                exit_codes = [PSCustomObject][ordered]@{
                    publisher = $publisherExitCode
                    relay = $relayExitCode
                    receiver = $receiverExitCode
                }
                commands = [PSCustomObject][ordered]@{
                    publisher = @($publisherExe) + $publisherArgs
                    relay = @($relayExe) + $relayArgs
                    receiver = @($receiverExe) + $receiverArgs
                }
            }
            $runConfig | ConvertTo-Json -Depth 6 |
                Set-Content -LiteralPath (Join-Path $runDirectory.FullName "run.json") -Encoding UTF8

            if (-not (Test-CompleteRun $runPath ([bool]$EnableRtcp))) {
                throw "Completed process reports failed validation in $runName"
            }

            if (-not $keepMediaFiles) {
                Remove-Item -LiteralPath (Join-Path $runDirectory.FullName "received.h264") `
                    -Force -ErrorAction SilentlyContinue
            }
        } finally {
            foreach ($process in @($publisherProcess, $relayProcess, $receiverProcess)) {
                if ($null -ne $process) {
                    if (-not $process.HasExited) {
                        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                    }
                    $process.Dispose()
                }
            }
        }
    }
}

Write-Summaries $OutputDirectory
Write-Host "Experiment complete: $OutputDirectory"
