# Step benchmark

This report compares the published v0.1.0 kernel with the released v0.2.0 Step implementation
on existing workloads at default event directionality. It measures production
`Step()` calls, excluding host perception, scene traversal, snapshot gathering and
copying, animation and rendering. Results are observations on one desktop, not a
frame-budget guarantee or a comparison with other attention-selection systems.

Across all nonempty single-observer configurations, the v0.2.0 implementation's measured batch
medians are **0.8–3.8% higher** than v0.1.0. Reference medians are **1.0–2.0% higher**.
These are measured compatibility regressions; no performance improvement is claimed.
Both kernels use 96-byte traces on x64. Process ranges and latency tails below retain
all observations, including a large v0.2.0 reference-workload spike.

## Revisions and environment

Measured on 2026-10-24. Both source revisions were built fresh with identical
compiler settings; the benchmark harness is byte-identical between them.

| Item | Configuration |
|---|---|
| Release | `v0.1.0`, commit `f89159b296149b0c0552c132fa2528643976316c` |
| v0.2.0 implementation | `v0.2.0` (pre-release implementation measured; see note below) |
| CPU | AMD Ryzen 7 5700X, 8 cores / 16 logical processors |
| Memory / OS | 32 GiB RAM; Windows 11 Pro 10.0.26200 |
| Power / scheduling | AMD Ryzen High Performance; normal priority; logical CPU 2 only, affinity mask `0x4` |
| Compiler | MSVC 19.38.33145, Visual Studio 2022 toolset 14.38.33130, x64 |
| Build tools | CMake 3.31.6-msvc6; Visual Studio 17 2022 generator and MSBuild |
| Flags | `/O2 /Ob2 /DNDEBUG`, warnings as errors; no LTO/IPO or fast-math |
| Background load | Ordinary desktop applications; no concurrent compiler builds or benchmark processes; no frequency lock |

**Measurement provenance:** The reported v0.2.0 timings were collected from the
final pre-release Step implementation, not from a build of the published tag itself.
The published `v0.2.0` tag has byte-identical `src/step.cpp`,
`include/interest/attention.hpp` and `benchmarks/step_benchmark.cpp` files.
The subsequent release preparation changed version metadata and documentation.
These are measurements of the same Step implementation and benchmark workload;
they must not be read as newly collected timings from the exact release-tag build.
For a publicly reproducible source comparison, check out `v0.1.0` and `v0.2.0`
and rerun the harness; observed timings may vary.

Affinity was applied immediately after launching each process, before the Step
workload measurements. It reduces migration but does not prevent other processes
from sharing the same logical CPU.

## Reproduce

To reproduce with public release tags, check out `v0.1.0` and `v0.2.0` separately
and configure both with the same tools:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DINTEREST_WARNINGS_AS_ERRORS=ON -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Run each executable three times, sequentially, using v0.1.0/v0.2.0,
v0.2.0/v0.1.0, v0.1.0/v0.2.0 order. Each invocation uses `--runs 5 --cycles 128`.
For the Windows affinity setting used here, use a distinct output file per process:

```powershell
$exe = (Resolve-Path './build/Release/interest_step_benchmark.exe').Path
$process = Start-Process -FilePath $exe -ArgumentList '--runs','5','--cycles','128' `
  -WindowStyle Hidden -PassThru -RedirectStandardOutput 'benchmark.txt'
$process.ProcessorAffinity = [IntPtr]0x4
$process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Normal
$process.WaitForExit()
```

Optional `--raw samples.csv` writes `section,config,run,index,ns` samples. `--smoke`
checks harness invariants with tiny counts; its timings carry no performance claim.
Debug and sanitizer builds are correctness checks. Compare matching configurations
and inspect process spread and tails. Record any different affinity, background
load, power policy or frequency controls when reproducing these measurements.

## Workloads and statistics

All measured v0.2.0 inputs use default `eventDirectionality == 1`. This compares
compatibility overhead on existing workloads; timings for nondefault directionality
are not characterized. Event directionality modifies novelty, not raw preference
scoring. The host still controls perception strength, event generation and candidate
range; distance cutoff and zero-perception rejection remain effective.

The single-observer matrix has 44 configurations: mixed geometry with 0, 16, 100
and 500 candidates across core, trace, beta-zero events and novelty variants;
100/500-candidate front, surround, far80, reject50, shuffle and churn workloads.
Mixed inputs use one-third Point/Bearing/NonSpatial, eligible fixed IDs/order and
small jitter. Points span 360 degrees and distances 1–38 with cutoff 40. Weights
are 1/1/1, full range 10, angle floor .2, sharpness 2, acquire/release .5/.4,
switch/override margins .08/.2 and dwell 0. Two contexts use eight affinity tags.
Novelty variants use beta .75 and a 1,000 ms half-life. See
[`step_benchmark.cpp`](../benchmarks/step_benchmark.cpp) for all inputs.

Inputs and sample/workspace buffers are prepared before timing. Each process runs
five repetitions of a deterministic 64-frame sequence at explicit 5 Hz, with 128
cycles per repetition and mode: 40,960 per-call samples per configuration. Every
run/configuration has an untimed warm-up cycle. Per-call samples include one timer
pair; percentiles use nearest rank over pooled samples. Batch timing brackets 64
consecutive Steps, including a decision-fingerprint fold, with reset outside timing.
Each process reports the median of five batch run means.

The reference workload has 100 observers × 500 candidates × 5 Hz: 500 Steps and
250,000 submitted candidate evaluations per simulated second, on one sequential
worker. Each process measures 50 synchronous 100-observer ticks per run, five runs.
Its world is 70% moving Point, 15% Bearing and 15% NonSpatial, with 56.5% of submitted
entries eligible. Gathering and copying remain outside the timer. Reference values
are kernel work per simulated second, not frame times.

The tables report median [minimum–maximum] across three independent processes per
revision. Single-observer values summarize the three process batch medians;
reference values summarize the three aggregate kernel times per simulated second.
Change is `100 * (candidate median / release median - 1)`: positive means slower in
these samples. Ranges describe observed variability, not confidence intervals.

## Direct comparison: full 44-configuration matrix

Batch nanoseconds per Step, median [process range]:

| Configuration | Candidates | v0.1.0 | v0.2.0 implementation | Change |
|---|---:|---:|---:|---:|
| churn/core | 100 | 7849.1 [7812.5–8416.1] | 7985.4 [7920.7–8001.4] | +1.7% |
| churn/core | 500 | 45896.4 [45658.1–46024.1] | 46351.5 [46169.4–46528.0] | +1.0% |
| churn/core+trace | 100 | 8088.4 [8032.8–8262.0] | 8167.8 [8151.8–8254.0] | +1.0% |
| churn/core+trace | 500 | 46883.9 [46537.9–47017.1] | 47358.6 [47128.6–47512.5] | +1.0% |
| far80/core | 100 | 4107.1 [4092.6–4501.8] | 4191.8 [4189.2–4251.6] | +2.1% |
| far80/core | 500 | 22576.7 [22506.2–22740.4] | 23200.9 [23069.0–23438.9] | +2.8% |
| far80/core+trace | 100 | 4616.2 [4602.1–4639.1] | 4707.6 [4693.9–4790.7] | +2.0% |
| far80/core+trace | 500 | 25078.6 [25008.3–25106.5] | 25443.1 [25361.6–25729.6] | +1.5% |
| front/core | 100 | 6946.9 [6927.8–6953.9] | 7157.5 [7142.1–7217.0] | +3.0% |
| front/core | 500 | 36231.6 [35852.4–37593.8] | 36806.2 [36769.4–37233.7] | +1.6% |
| front/core+trace | 100 | 7259.5 [7253.9–7263.2] | 7480.8 [7474.0–7557.6] | +3.0% |
| front/core+trace | 500 | 37388.1 [37381.9–37510.6] | 38442.5 [38338.0–38804.6] | +2.8% |
| mixed/core | 0 | 292.1 [291.3–295.3] | 296.5 [291.3–296.8] | +1.5% |
| mixed/core | 16 | 1175.2 [1175.1–1181.9] | 1216.2 [1211.9–1221.9] | +3.5% |
| mixed/core | 100 | 6140.6 [6135.2–6155.2] | 6371.2 [6339.0–6404.2] | +3.8% |
| mixed/core | 500 | 32002.7 [31929.5–32060.8] | 33193.9 [33038.9–33389.5] | +3.7% |
| mixed/core+trace | 0 | 293.6 [290.4–295.7] | 292.8 [291.9–293.4] | -0.3% |
| mixed/core+trace | 16 | 1230.3 [1229.8–1236.3] | 1277.3 [1266.8–1279.2] | +3.8% |
| mixed/core+trace | 100 | 6482.0 [6482.0–6486.0] | 6716.3 [6687.1–6784.3] | +3.6% |
| mixed/core+trace | 500 | 33594.7 [33573.2–34068.5] | 34845.7 [34647.7–35084.7] | +3.7% |
| mixed/events,beta0 | 0 | 291.5 [290.1–296.8] | 291.5 [289.9–293.0] | 0.0% |
| mixed/events,beta0 | 16 | 1187.9 [1183.7–1188.0] | 1222.6 [1218.9–1228.0] | +2.9% |
| mixed/events,beta0 | 100 | 6239.3 [6236.7–6240.8] | 6462.7 [6414.8–6485.1] | +3.6% |
| mixed/events,beta0 | 500 | 32408.5 [32402.6–33047.1] | 33456.3 [33249.0–33671.3] | +3.2% |
| mixed/novelty | 0 | 293.3 [293.1–293.5] | 291.9 [291.5–293.6] | -0.5% |
| mixed/novelty | 16 | 1198.8 [1195.0–1201.5] | 1237.0 [1230.4–1239.4] | +3.2% |
| mixed/novelty | 100 | 6239.1 [6236.8–6258.4] | 6454.0 [6431.1–6539.5] | +3.4% |
| mixed/novelty | 500 | 32519.7 [32380.5–33208.9] | 33736.7 [33335.7–34355.4] | +3.7% |
| mixed/novelty+trace | 0 | 294.2 [293.1–294.2] | 292.9 [292.4–293.4] | -0.4% |
| mixed/novelty+trace | 16 | 1259.3 [1247.1–1261.1] | 1284.7 [1280.8–1287.6] | +2.0% |
| mixed/novelty+trace | 100 | 6575.6 [6572.1–6583.2] | 6799.1 [6796.2–6868.1] | +3.4% |
| mixed/novelty+trace | 500 | 33987.7 [33983.6–34229.2] | 35201.0 [35066.5–35563.0] | +3.6% |
| reject50/core | 100 | 4709.3 [4707.1–4869.3] | 4832.7 [4793.3–4861.6] | +2.6% |
| reject50/core | 500 | 24252.9 [24215.5–24298.2] | 24872.4 [24716.0–25050.0] | +2.6% |
| reject50/core+trace | 100 | 5216.3 [5195.2–5247.2] | 5369.1 [5260.9–5384.1] | +2.9% |
| reject50/core+trace | 500 | 26554.1 [26526.7–26564.3] | 27193.6 [27066.8–27468.9] | +2.4% |
| shuffle/core | 100 | 7666.1 [7662.6–7678.4] | 7769.1 [7727.3–7861.0] | +1.3% |
| shuffle/core | 500 | 44985.4 [44855.5–45018.1] | 45337.4 [45104.6–45670.5] | +0.8% |
| shuffle/core+trace | 100 | 7898.2 [7896.9–7930.6] | 8046.8 [7992.5–8139.5] | +1.9% |
| shuffle/core+trace | 500 | 45941.3 [45926.0–46373.5] | 46395.9 [46270.9–46659.5] | +1.0% |
| surround/core | 100 | 6942.0 [6928.8–6970.8] | 7149.8 [7146.6–7225.3] | +3.0% |
| surround/core | 500 | 35833.4 [35816.4–35842.0] | 37034.3 [36895.4–37455.4] | +3.4% |
| surround/core+trace | 100 | 7254.1 [7244.7–7277.9] | 7469.0 [7468.3–7578.7] | +3.0% |
| surround/core+trace | 500 | 37548.8 [37464.5–40708.5] | 38532.5 [38400.1–39045.7] | +2.6% |


## Reference variants

Kernel milliseconds per simulated second, median [process range]:

| Configuration | v0.1.0 | v0.2.0 implementation | Change |
|---|---:|---:|---:|
| ref/core | 21.176 [21.085–21.358] | 21.394 [21.357–29.844] | +1.0% |
| ref/core,shared-order | 14.084 [14.082–14.174] | 14.362 [14.346–16.420] | +2.0% |
| ref/novelty | 21.649 [21.585–22.427] | 22.085 [21.861–23.976] | +2.0% |
| ref/novelty+trace | 22.171 [22.142–22.382] | 22.473 [22.417–24.770] | +1.4% |


## Latency and variability

Per-call p99 ranges across the three processes, microseconds, at 500 mixed candidates:

| Configuration | v0.1.0 p99 range | v0.2.0 p99 range |
|---|---:|---:|
| mixed/core | 38.8–50.5 | 39.1–50.8 |
| mixed/core+trace | 40.5–51.5 | 39.0–48.7 |
| mixed/novelty | 39.1–50.4 | 37.2–51.2 |
| mixed/novelty+trace | 44.6–51.8 | 44.9–56.0 |


Reference synchronous 100-observer tick p99 ranges across processes, milliseconds:

| Configuration | v0.1.0 tick p99 range | v0.2.0 tick p99 range |
|---|---:|---:|
| ref/core | 4.329–6.199 | 4.537–18.493 |
| ref/core,shared-order | 3.045–3.399 | 3.087–5.760 |
| ref/novelty | 4.441–6.397 | 4.468–7.127 |
| ref/novelty+trace | 4.618–6.559 | 4.571–7.900 |


The observed timer granularity was 100 ns; empty timed regions averaged 18.9–19.1 ns.
Batch measurements are more useful than per-call timers at small candidate counts.
The largest individual single-observer samples were 2.3073 ms for v0.1.0 and
2.5486 ms for the v0.2.0 implementation. They are retained as observations, not latency bounds.

All three v0.2.0 reference-core process totals were 21.394 / 29.844 / 21.357 ms
per simulated second; release totals were 21.176 / 21.085 / 21.358 ms. The middle
v0.2.0 process also had a synchronous-tick p99 of 18.493 ms. Its samples are
included in the ranges and percentiles. No profiler attribution was performed;
affinity and repeated processes do not eliminate desktop interference. The medians
therefore do not describe worst-case latency or establish a fixed overhead budget.

## Memory and numerical compatibility

Both measured kernels have a 96-byte `CandidateTrace` on the tested x64 builds.
In v0.2.0, `noveltyGate` occupies four bytes of existing alignment padding
after `eventStrength`. All 17 v0.1.0 trace field offsets remain unchanged; no packing
directives are used. Source consumers should rebuild with matching headers/library,
and positional trace aggregate initializers must account for the inserted member.
These are verified layouts, not ABI promises for other architectures.

| Compiler / target | Trace size | Alignment | eventStrength | noveltyGate | eventNovelty | noveltyContribution |
|---|---:|---:|---:|---:|---:|---:|
| MSVC 19.38.33145 / Windows x64 | 96 | 8 | 72 | 76 | 80 | 88 |
| MSVC 19.51.36260 / Windows x64 | 96 | 8 | 72 | 76 | 80 | 88 |
| GCC 13.3.0 / Linux x86-64 | 96 | 8 | 72 | 76 | 80 | 88 |
| Clang 18.1.3 / Linux x86-64 | 96 | 8 | 72 | 76 | 80 | 88 |

On MSVC x64, `CandidateSnapshot` is 104 bytes and `ObserverMemory` is 32 bytes in
both revisions. Prepared capacity 500 uses 4,000 bytes of scratch IDs and optionally
48,000 bytes of trace entries, excluding vector objects and allocator overhead.
Only observer memory must persist; one worker can reuse scratch and trace buffers
sequentially. Input arrays belong to the host. Benchmark-process memory also includes
pre-generated input frames and is not kernel retained memory.

Each of the six Release processes passes the harness's status, paired-input, exact
replay and trace-on/off checks, observing zero allocations in 3,604,480 measured
single-observer calls and 200,000 reference calls per process. The dedicated
allocation regression also reports zero across 404 prepared calls, including
nondefault directionality and rejection paths. `Reserve()` and host setup may
allocate; these observations concern prepared Step calls.

An independent 4,000-frame × 40-candidate Release replay against v0.1.0 produces
identical binary serialization of every decision, observer-memory field and
pre-existing trace field at default directionality. Padding bytes are excluded.
The default path reuses the original rounded `gate` exactly. The smoke regression
checks the 96-byte size and all 17 legacy offsets on x64; the complete suite also
covers invalid events, validation precedence, tracing, permutations and failure
atomicity. Validation includes MSVC/GCC/Clang Debug and Release, constrained builds,
Release consumers and Clang ASan/UBSan.

## Interpretation limits

Measurements cover synthetic kernel workloads on one machine with desktop load.
Affinity does not remove contention, frequency variation or scheduling delays.
Replay checks establish compatibility within a build/platform; cross-platform
bit-exact simulation, ARM and other architectures remain unverified. These timings
do not isolate the cost of a particular validation check or field. Both revisions
already use 96-byte traces, so this comparison does not establish a benefit from
changing trace layout.

Default angular gating can attenuate behind events. Lower event directionality
reduces that novelty penalty while retaining distance and perception gating. Score
saturation can erase a challenger's unsaturated advantage; margins and dwell can
retain a target after novelty fades. Beta, half-life and update cadence require
host-specific tuning. The scenarios test mechanism behavior, not perceptual realism,
human attention or gameplay quality.
