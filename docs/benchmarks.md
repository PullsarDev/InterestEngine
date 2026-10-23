# Step benchmark

These measurements characterize the production kernel on synthetic snapshots.
They exclude perception, host scene traversal, snapshot gathering/copying, animation
and rendering. They are observations on one desktop, not a frame-budget guarantee
or a comparison against other AI systems.

## Reproduce

Build Release as described in the README, then run:

```sh
./build/interest_step_benchmark --runs 5 --cycles 128 --raw build/samples.csv
```

For Visual Studio, the executable is
`./build/Release/interest_step_benchmark.exe`. `--raw` is optional and writes
`section,config,run,index,ns` samples. `--smoke` uses tiny counts and validates the
harness only; its timings carry no performance claim. Debug and sanitizer builds
are correctness checks, not performance baselines.

Use the same compiler, flags, workload and run counts for comparisons. Record CPU,
RAM, OS, power policy, background load and any affinity/frequency controls. Inspect
all runs and tails, not just a favorable median. Deterministic generation and
pairing checks make the input comparisons reproducible; wall-clock timings vary.

## Recorded environment

The following results were measured on 2026-10-16 with the same attention
implementation and benchmark harness distributed here; subsequent release
preparation changes only version metadata, comments and build packaging.

| Component | Recorded configuration |
|---|---|
| CPU | AMD Ryzen 7 5700X, 8 cores / 16 threads, 3.4 GHz base; 512 KiB L2/core, 32 MiB L3 |
| Memory / OS | 32 GiB RAM; Windows 11 Pro 10.0.26200 |
| Power / scheduling | AMD Ryzen High Performance; ordinary desktop with other applications running; no affinity, priority adjustment or frequency lock |
| Compiler | MSVC 19.38.33145, Visual Studio 2022 toolset 14.38.33130, x64 |
| Build tools | CMake 3.31.6-msvc6, Ninja 1.12.1 |
| Flags | `/O2 /Ob2 /DNDEBUG`, `/W4 /WX /permissive- /utf-8`; no LTO/IPO |

Three independent processes each ran five repetitions of the matrix. These are
the corrected measurements with strictly paired variant inputs, checked against
the recorded output of all three processes. No private report or host project is
needed to run the distributed harness.

## Method and workloads

Inputs are generated and every workspace/sample buffer reserved before timing.
The harness invokes the real out-of-line `Step()` and checks allocation counts,
successful statuses and replay fingerprints. Event variants differ only in event
fields; fixed/shuffled variants submit the same per-frame snapshots. These
relationships and trace-on/off decision equivalence are checked in smoke mode too.

The single-observer matrix has 44 configurations: mixed geometry with 0, 16, 100
and 500 candidates across core, trace, beta-zero events and novelty variants;
100/500-candidate front, surround, far80, reject50, shuffled and churn workloads.
The mixed workload uses one-third Point/Bearing/NonSpatial, all eligible, fixed
IDs/order and small jitter. Points span 360 degrees and distances 1–38 with cutoff
40. Weights are 1/1/1, full range 10, angle floor .2, sharpness 2, acquire/release
.5/.4, switch/override margins .08/.2 and dwell 0. Two contexts use eight affinity
tags. Novelty variants use beta .75; the source defines all remaining inputs.

Each configuration uses a deterministic 64-frame sequence at explicit 5 Hz, five
runs and 128 cycles per run/mode: 40,960 per-call samples. Each run/configuration
has an untimed warm-up cycle. Per-call samples include one timer pair. Percentiles
use nearest rank over pooled samples. Batch timing brackets 64 consecutive steps,
including a decision fingerprint fold, with reset outside timing; the reported
batch statistic is the median of five run means. Run-major ordering exposes drift
as run spread. The observed timer granularity was 100 ns and empty regions averaged
18.8–20.6 ns across processes; batch figures are more useful at small counts.

Core mixed-workload **batch nanoseconds per Step**, trace off and no events:

| Submitted candidates | Process 1 | Process 2 | Process 3 |
|---:|---:|---:|---:|
| 0 | 318.9 | 303.2 | 294.6 |
| 16 | 1,248.9 | 1,252.1 | 1,249.3 |
| 100 | 6,721.4 | 6,365.8 | 6,517.1 |
| 500 | 35,371.6 | 34,634.9 | 35,154.7 |

The reference workload is 100 observers × 500 candidates × 5 Hz: 500 steps and
250,000 submitted candidate evaluations per simulated second. One sequential
worker processes synchronous 100-observer ticks, 50 ticks per run, five runs.
Its world is 70% moving Points, 15% Bearing and 15% NonSpatial; 56.5% of submitted
entries are eligible. Host gathering and copying remain outside the timer.

Core reference kernel time per simulated second was **24.013 / 23.479 / 22.817 ms**
across the three processes. In process 1, a synchronous 100-Step tick averaged
4.8025 ms; individual Step mean/p50/p95/p99 were 45.965/42.9/61.6/69.6 microseconds.
This is work accumulated at the stated simulation rate, not a frame time.

Order materially affects CPU cost. Strictly paired reshuffling added 27–28% at
100 mixed candidates and 37–43% at 500 across these processes. In the reference
workload, per-observer submission order cost 45–50% more than shared order despite
identical decisions. Copy/sort/search isolation probes are diagnostic estimates,
not profiler-confirmed causal attribution. Re-run with your actual distributions,
rejections, contexts, event density and tracing needs.

## Allocation and memory evidence

Each recorded Release process observed zero allocations across 3,604,480 measured
single-observer steps and 200,000 reference steps, after preparation. The dedicated
allocation regression also exercises 400 successful prepared calls plus four
step-level rejection paths. This does not prohibit setup allocation in `Reserve()`
or host-side allocation.

On the measured MSVC x64 build, `ObserverMemory` is 32 bytes. Capacity 500 requires
4,000 bytes of scratch ID storage and optionally 48,000 bytes of trace entries,
excluding vector objects and allocator overhead. Input arrays belong to the host.
Scratch/trace may be reused sequentially between observers; only observer memory
must persist. These sizes are implementation/platform observations, not ABI promises.
Peak benchmark-process memory includes pre-generated workloads and is not kernel
retained memory.

## Interpretation limits

Geometric gates attenuate novelty; the synthetic front/behind event test captures
40/40 labelled front events and 0/40 labelled important behind events under its
chosen profile. Saturation can erase a challenger's otherwise sufficient advantage.
Beta, half-life, margins and update cadence alter capture and retention. These
scenarios document limitations rather than validate real gameplay, perception or
human attention. Replay equality is tested within a build/platform; cross-platform
bit-exact output, ARM and other architectures remain unverified.
