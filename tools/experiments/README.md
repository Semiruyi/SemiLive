# Delivery and latency experiments

Use one run and one capture measurement window for AU delivery, on-time delivery,
and T0-to-T5 latency. Tooling lives here; generated data remains in the repository's
experiments/ directory. Existing data directories do not need to be moved.

## Entry points

- run-experiment.ps1: direct or relay runs, with optional loss and RTCP.
- run-loss-baseline.ps1: random-loss matrix using the same runner and analyzer.
- analyze.py: per-run delivery, deadlines, stages, and latency percentiles.
- summarize.py: per-run and grouped reports for a loss matrix.
- compare.py: matched-seed comparison of two reorder-capacity matrices.
- run-random-loss-baseline.ps1: [legacy whole-session runner](legacy-random-loss.md)
  for historical reports and resume support, not the default for new experiments.

Legacy delivery uses whole-session Publisher input AUs; the unified analyzer uses
successfully sent AUs in the capture measurement window. Do not compare these
ratios as if their denominators were identical.

## Same-host measurement

This measures capture-call return to a playable AU becoming available at the
receiver pipeline output (T0 to T5). It does not include DXGI acquisition/copy
inside the capture backend, decoder, file/ffplay submission, or display.
When DXGI reports no change, the publisher emits the existing image on its next
sampling tick: T0 is the current sampling observation, not the age of the image.
Use a continuously moving desktop scene for a useful baseline.

Windows only: both processes use QueryPerformanceCounter on the same host and
boot. Linux builds keep instrumentation disabled. Cross-host traces are rejected.
Do not combine traces across reboots, machine clones, or publisher restarts.

## Run

Build with MSYS2 UCRT64 using the windows-release preset. From the repository:
Ensure the MSYS2 UCRT64 bin directory is on PATH so child processes can load
FFmpeg and GCC runtime DLLs. Python 3 is required; -Python accepts an executable
path if python is not on PATH.

```powershell
./tools/experiments/run-experiment.ps1 -Python python
```

Defaults: direct and zero-loss relay, three runs each, five-second warmup and
60-second measurement. Media settings are publisher defaults; verify dimensions,
frame rate and encoder settings in publisher.json before comparing runs. RTCP is
disabled in this first clean baseline. Choose -Mode direct or -Mode relay to run
one topology. Use -BuildDirectory build/windows-debug/bin for a debug smoke test,
but use Release for optimization comparisons.

Each run retains manifests, normal session JSON, logs, H.264 output, publisher.csv,
receiver.csv, trace status files, frames.csv and summary.json. The environment
variables SEMILIVE_LATENCY_TRACE (output CSV path) and SEMILIVE_LATENCY_RUN_ID
(shared alphanumeric/hyphen/underscore ID) can also enable tracing manually.
They must be set before process launch. Missing either disables recording.

## Boundaries and interpretation

- T0: capture call returned, before preparing/publishing the captured frame.
- T1: begin preparing/converting the corresponding frame for encoding.
- T2: encoded packet assigned to an AU, including delayed output or flush.
- T3: immediately before the first RTP send call.
- T4: successful UDP receive, before copying its bytes into a datagram.
- T5: recovery gate and timestamp mapping succeeded, before output submission.

Per-frame times are integer QPC ticks. PTS preserves capture identity through
delayed encoding; packetizer returns the actual RTP timestamp. The analysis joins
run ID + SSRC + RTP timestamp, checks host/clock/frequency, rejects duplicate keys
(including ambiguous full timestamp wrap), and reports unmatched or invalid rows.
AU byte counts can differ because the receiver rebuilds Annex-B prefixes and may
prepend parameter sets; packet counts are checked for the clean scenario.

Percentiles use nearest rank. Stage percentiles are not additive. First-packet
latency includes send-call and userspace receive scheduling, not pure wire time.
Receive-to-ready includes sending the rest of the AU, receiving, reordering and
assembly. IDR and non-IDR distributions are reported separately.

Recording reserves 100,000 rows per process and exports at normal process exit;
overflow is reported in the .status sidecar and invalidates analysis. Forced
termination can leave incomplete files, which analysis rejects. Receive timing
tracking is bounded to 4,096 pending AUs; discarded entries surface as unmatched
publisher rows. Loss runs use the same collector with loss-aware analysis.

Reanalyze with:

```powershell
python tools/experiments/analyze.py publisher.csv receiver.csv --output result --warmup-seconds 5 --duration-seconds 60
python tools/experiments/test_analyze.py
python tools/experiments/test_compare.py
```

## Random loss experiments

```powershell
./tools/experiments/run-loss-baseline.ps1 -Python python
./tools/experiments/run-loss-baseline.ps1 -Python python -ReorderPackets 1024
```

Defaults: Release, 0/0.1/0.5/1/3/5/10 percent random loss, three seeds
(1001/1002/1003), three-second warmup and 20-second measurement per run.
RTCP/NACK and automatic PLI are enabled; feedback is direct loopback and has no added loss or delay.
Use -DisablePli to retain NACK while disabling automatic PLI (the historical baseline).
PLI waits 100ms in recovery, retries every 500ms, and pauses after 1000ms without
accepted media. Override with -PliInitialWaitMs, -PliRetryIntervalMs and
-PliMediaTimeoutMs. Publisher requests coalesce and are applied to the next input
frame no more frequently than every 500ms. Existing queued output is not discarded.
These intervals are experimental defaults, not a production tuning claim.
Reorder capacity defaults to 512 packets; -ReorderPackets changes capacity only,
leaving the 50ms hold limit unchanged. Receiver CLI: --rtp-reorder-packets COUNT.
After both matrices finish, compare with:

```powershell
python tools/experiments/compare.py experiments/BASELINE experiments/CANDIDATE
```

For a PLI on/off comparison, keep reorder capacity and all other settings equal:

```powershell
./tools/experiments/run-loss-baseline.ps1 -Python python -ReorderPackets 1024 -DisablePli -OutputDirectory experiments/pli-off
./tools/experiments/run-loss-baseline.ps1 -Python python -ReorderPackets 1024 -OutputDirectory experiments/pli-on
python tools/experiments/compare.py experiments/pli-off experiments/pli-on --variable pli
```

Reports include PLI sent/received counts and applied encoder requests. An accepted
request is not proof of receiver recovery; IDR loss and natural GOP boundaries can
affect recovery. Counters alone do not attribute an episode to PLI rather than NACK
or a periodic IDR. The legacy whole-session runner explicitly disables PLI to
preserve its historical NACK-only behavior.

This checks media settings, seeds, warmup/measurement duration, RTCP and hold
limit, then writes comparison.csv/json into CANDIDATE. Identical settings and
seeds do not guarantee identical encoded media; compare AU burst sizes as well.
The relay drops media and retransmissions alike. This is random RTP packet loss,
not a realistic bandwidth/RTT/jitter simulation. Use -DisableRtcp for an
unprotected comparison. Keep desktop content consistent; a fixed seed reproduces
drop decisions but does not make desktop capture/encoded packet sizes identical.

The loss analyzer permits missing AUs and changed packet counts due to retries.
Negative/missing timestamps, ambiguous identities and trace overflow still fail
validation. An empty receiver trace is permitted and reported as zero delivery,
with null latency percentiles. No received frames means there is no receiver
clock sample available to compare.

Delivery and on-time delivery use the publisher's successfully sent AUs in the
measurement capture window as denominator. They are AU-ready metrics, not
decoded/displayed frame quality. Percentiles describe only delivered AUs. The
AU delivery-gap metric excludes receiver startup and the artificial shutdown
drain; it measures gaps between delivered selected AUs. Whole-session reorder
gap and retransmission counters include warmup and shutdown.
It does not include missing leading/trailing output in the measurement window;
always interpret it alongside delivery rate, especially when there are fewer
than two delivered AUs.

runs.csv contains each run; groups.csv/groups.json contain medians of per-run
metrics, not percentiles of a pooled sample. Full publisher/receiver/relay JSON
and raw traces remain under each loss directory.
