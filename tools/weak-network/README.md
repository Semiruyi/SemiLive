# Random-loss baseline runner

`run-random-loss-baseline.ps1` runs the current no-retransmission pipeline through the deterministic UDP relay,
validates the machine-readable reports, and writes per-run and aggregate CSV files.

Build the Windows binaries first, then run a short pilot from PowerShell:

```powershell
./tools/weak-network/run-random-loss-baseline.ps1 `
  -BuildDirectory build/windows-debug/bin `
  -DurationSeconds 60 `
  -LossPercents 0,0.1,0.5,1,3,5,10 `
  -Seeds 1001,1002,1003 `
  -EnableRtcp
```

From an MSYS2 UCRT64 shell, keep the UCRT runtime on `PATH` and invoke Windows PowerShell. Omitting the matrix
parameters uses the default loss rates `0,0.1,0.5,1,3,5,10` and seeds `1001,1002,1003`:

```sh
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File tools/weak-network/run-random-loss-baseline.ps1 \
  -DurationSeconds 60
```

The script creates a new timestamped directory below `experiments/`. Generated experiment directories are ignored
by Git. Each run keeps stdout, stderr, `run.json`, and the Publisher, Relay, and Receiver JSON reports. The received
H.264 file is removed after report validation unless `-KeepMedia` is specified.

`-EnableRtcp` opens a direct control path on Publisher port 5005 and Receiver port 5007. Override these with
`-PublisherRtcpPort` and `-ReceiverRtcpPort` if necessary. The control path is intentionally outside the one-way RTP
loss relay: RR still measures the impaired forward RTP stream, while feedback delivery stays reliable for this first
baseline. SR/RR is observation only at this stage and does not retransmit media.

Use file output for the measured baseline. Run separate ffplay sessions for visual checks so player scheduling and
output backpressure do not contaminate the transport comparison.

To regenerate CSV files after inspecting or copying a completed experiment:

```powershell
./tools/weak-network/run-random-loss-baseline.ps1 `
  -OutputDirectory experiments/random-loss-baseline-YYYY-MM-DD-HHMMSS `
  -SummarizeOnly
```

`runs.csv` contains one detailed row per seed for diagnosis and auditing. `summary.csv` intentionally contains only
the cross-seed fields used to read the result and compare the baseline with later NACK experiments:

- `output_delivery_ratio_median_percent` and `output_delivery_ratio_worst_percent`: Receiver output access units
  divided by Publisher input access units. This is the primary measure of how much playable video survived.
- `ended_waiting_rate_percent` and `terminal_output_gap_*`: whether a run ended while waiting for a random-access
  frame and how long the final freeze lasted.
- `first_output_success_rate_percent` and `first_output_delay_median_ms`: whether playback started and how long
  startup usually took.
- `recovery_completion_rate_percent`: completed recovery episodes divided by started episodes, aggregated across
  all seeds at that loss rate.

Use `runs`, `successful_runs`, and `relay_actual_loss_median_percent` as summary-level experiment-integrity checks.
Packet conservation, discard counts, recovery durations, output gaps, and stall counts remain available in
`runs.csv`. When RTCP is enabled, `runs.csv` also includes report counts, the Publisher RTT sample, Receiver-reported
loss, cumulative loss, and jitter. RTCP fields stay empty for older or RTCP-disabled reports. A run that never
produces a first frame has no inter-output stall to count, so those diagnostic fields must not be interpreted alone.

If a process fails during a long matrix, resume the same experiment instead of discarding completed runs:

```powershell
./tools/weak-network/run-random-loss-baseline.ps1 `
  -OutputDirectory experiments/random-loss-baseline-YYYY-MM-DD-HHMMSS `
  -Resume
```

Resume reloads the original matrix and duration from `config.json`, skips complete runs, and moves an incomplete run
to `failed/` before retrying it. The preserved attempt logs remain available for diagnosis.

The relay guarantees the same drop-decision sequence for the same seed, loss rate, and input datagram sequence. A
desktop capture may still encode into a different datagram sequence when the captured content or timing changes, so
formal results use fixed capture content, multiple seeds, and repeated runs.
