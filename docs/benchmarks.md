# Step benchmark

These measurements characterize the production kernel on synthetic snapshots.
They exclude perception, host scene traversal, snapshot gathering/copying, animation
and rendering. They are observations on one desktop, not a frame-budget guarantee
or a comparison against other attention-selection systems.

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

The following results were measured on 2026-10-16 using the attention implementation
and benchmark harness in this repository.

| Component | Recorded configuration |
|---|---|
| CPU | AMD Ryzen 7 5700X, 8 cores / 16 threads, 3.4 GHz base; 512 KiB L2/core, 32 MiB L3 |
| Memory / OS | 32 GiB RAM; Windows 11 Pro 10.0.26200 |
| Power / scheduling | AMD Ryzen High Performance; ordinary desktop with other applications running; no affinity, priority adjustment or frequency lock |
| Compiler | MSVC 19.38.33145, Visual Studio 2022 toolset 14.38.33130, x64 |
| Build tools | CMake 3.31.6-msvc6, Ninja 1.12.1 |
| Flags | `/O2 /Ob2 /DNDEBUG`, `/W4 /WX /permissive- /utf-8`; no LTO/IPO |

Three independent processes each ran five repetitions of the matrix with strictly
paired variant inputs. The tables below summarize the recorded output of those
processes; the harness checks input pairing as part of each run.

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

At default event directionality, geometric gates attenuate novelty; the synthetic
front/behind event test captures 40/40 labelled front events and 0/40 labelled important
behind events under its chosen profile. Saturation can erase a challenger's otherwise
sufficient advantage.
Beta, half-life, margins and update cadence alter capture and retention. These
scenarios document limitations rather than validate real gameplay, perception or
human attention. Replay equality is tested within a build/platform; cross-platform
bit-exact output, ARM and other architectures remain unverified.

## Event directionality candidate (proposed v0.2.0)

`CandidateSnapshot::eventDirectionality` changes novelty's angular gate, not raw
preference scoring. Its default of 1 reuses the legacy rounded `gate` exactly.
Lower values reduce the novelty angle penalty while keeping distance and perception
gating. The host still controls perception strength, event generation and candidate
range. Capture and return remain conditional on effective scores and temporal policy.

The trailing snapshot field and new `CandidateTrace::noveltyGate` change the public
layout contract: rebuild consumers against matching headers and library. The trace
field now sits immediately after `eventStrength`, before `eventNovelty`, reusing four
bytes of x64 alignment padding. The snapshot remains 104 bytes and the trace returns
to 96 bytes, with all v0.1.0 trace field offsets unchanged. Capacity 500 again uses
48,000 bytes of optional trace storage; scratch ID storage and 32-byte observer memory
are unchanged. The smoke test checks every legacy field offset on x64. These are
tested layouts, not ABI promises for other targets. Positional trace aggregate
initializers must account for the inserted field; named field access is unchanged.

The following timing table records the initial implementation with a 104-byte trace.
It is retained as historical evidence; the focused layout comparison below measures
the subsequent 96-byte trace against the pre-cleanup PR head.

The comparison on 2026-10-24 rebuilds tag `v0.1.0` (`f89159b`) and the event
directionality implementation (`0f9d1b4`) with the same MSVC 19.38.33145 x64 Release settings,
Visual Studio generator, warnings as errors and no LTO/IPO. CPU, RAM, OS and power
policy match the recorded environment above; background desktop applications were
running, with no affinity, priority or frequency controls. No compiler builds ran
concurrently with the timed processes.

Each revision runs the unchanged 44-configuration harness in three independent
processes, each with `--runs 5 --cycles 128`. Process order is baseline/candidate,
candidate/baseline, baseline/candidate to expose drift. All inputs retain default
directionality, so these measure compatibility overhead on existing workloads,
not the cost or behavior of nondefault directionality. Single-observer comparisons
use the median of three reported process batch medians; reference comparisons use
the median of the three aggregate kernel times per simulated second. Per-call tails
and all process results are inspected separately; timings are desktop observations.

Measured batch nanoseconds per Step (default directionality throughout):

| Configuration | Candidates | v0.1.0 | Candidate | Change |
|---|---:|---:|---:|---:|
| churn/core | 100 | 7828.5 | 7970.6 | +1.8% |
| churn/core | 500 | 45798.2 | 46221.8 | +0.9% |
| churn/core+trace | 100 | 8063.0 | 8232.6 | +2.1% |
| churn/core+trace | 500 | 46743.8 | 47516.9 | +1.7% |
| far80/core | 100 | 4100.5 | 4255.1 | +3.8% |
| far80/core | 500 | 22575.2 | 23296.6 | +3.2% |
| far80/core+trace | 100 | 4617.0 | 4765.0 | +3.2% |
| far80/core+trace | 500 | 24943.8 | 25774.9 | +3.3% |
| front/core | 100 | 6948.0 | 7250.4 | +4.4% |
| front/core | 500 | 35678.9 | 37233.9 | +4.4% |
| front/core+trace | 100 | 7294.1 | 7587.7 | +4.0% |
| front/core+trace | 500 | 37266.3 | 38987.5 | +4.6% |
| mixed/core | 0 | 293.3 | 294.4 | +0.4% |
| mixed/core | 16 | 1180.8 | 1219.3 | +3.3% |
| mixed/core | 100 | 6150.1 | 6422.7 | +4.4% |
| mixed/core | 500 | 32019.7 | 33278.4 | +3.9% |
| mixed/core+trace | 0 | 291.7 | 291.5 | -0.1% |
| mixed/core+trace | 16 | 1231.7 | 1276.7 | +3.7% |
| mixed/core+trace | 100 | 6482.7 | 6773.7 | +4.5% |
| mixed/core+trace | 500 | 33533.5 | 35128.1 | +4.8% |
| mixed/events,beta0 | 0 | 291.8 | 291.3 | -0.2% |
| mixed/events,beta0 | 16 | 1188.4 | 1228.7 | +3.4% |
| mixed/events,beta0 | 100 | 6242.7 | 6489.5 | +4.0% |
| mixed/events,beta0 | 500 | 32287.8 | 33712.6 | +4.4% |
| mixed/novelty | 0 | 292.8 | 291.9 | -0.3% |
| mixed/novelty | 16 | 1197.3 | 1237.4 | +3.3% |
| mixed/novelty | 100 | 6260.2 | 6512.6 | +4.0% |
| mixed/novelty | 500 | 32361.4 | 33710.0 | +4.2% |
| mixed/novelty+trace | 0 | 295.6 | 291.9 | -1.3% |
| mixed/novelty+trace | 16 | 1253.3 | 1292.8 | +3.2% |
| mixed/novelty+trace | 100 | 6574.5 | 6858.9 | +4.3% |
| mixed/novelty+trace | 500 | 33989.5 | 35497.1 | +4.4% |
| reject50/core | 100 | 4711.8 | 4872.8 | +3.4% |
| reject50/core | 500 | 24198.0 | 25080.6 | +3.6% |
| reject50/core+trace | 100 | 5184.9 | 5358.5 | +3.3% |
| reject50/core+trace | 500 | 26547.4 | 27458.3 | +3.4% |
| shuffle/core | 100 | 7659.6 | 7791.6 | +1.7% |
| shuffle/core | 500 | 44783.3 | 45370.5 | +1.3% |
| shuffle/core+trace | 100 | 7884.1 | 8089.7 | +2.6% |
| shuffle/core+trace | 500 | 45897.8 | 46571.8 | +1.5% |
| surround/core | 100 | 6951.0 | 7250.4 | +4.3% |
| surround/core | 500 | 35741.6 | 37329.9 | +4.4% |
| surround/core+trace | 100 | 7275.9 | 7564.2 | +4.0% |
| surround/core+trace | 500 | 37471.1 | 39120.6 | +4.4% |

Reference kernel milliseconds per simulated second (100 observers × 500 candidates × 5 Hz):

| Configuration | v0.1.0 | Candidate | Change |
|---|---:|---:|---:|
| ref/core | 21.099 | 21.401 | +1.4% |
| ref/core,shared-order | 14.110 | 14.489 | +2.7% |
| ref/novelty | 21.572 | 21.970 | +1.8% |
| ref/novelty+trace | 22.088 | 22.700 | +2.8% |

This is a measurable regression: nonempty single-observer batch medians increase
0.9–4.8%, and reference variants increase 1.4–2.8%. Empty-workload differences are
within 1.3% and do not establish an improvement. These results include validation
of the additional scalar and the extra trace field; no profiler-based attribution
of their individual costs was performed.

For mixed/core at 500 candidates, the three process batch medians are
31,884.5 / 32,328.5 / 32,019.7 ns for v0.1.0 and
33,383.5 / 33,271.7 / 33,278.4 ns for the candidate. Per-call p99 ranges are
36.6–48.8 and 36.9–37.7 microseconds respectively. Mixed/novelty at 500 has p99
ranges of 36.2–47.0 and 39.4–51.1 microseconds. Across all single-observer
configurations/processes the largest individual samples are 1.2103 ms (v0.1.0)
and 1.6740 ms (candidate). Tails vary with desktop scheduling; lower isolated
percentiles do not demonstrate a performance improvement or a latency guarantee.

All six processes pass the harness's pairing, status, replay and trace-on/off
checks, with zero allocations in 3,604,480 measured single-observer calls and
200,000 reference calls per process. Nondefault directionality is separately
covered by the prepared allocation regression and correctness suites. An
independent 4,000-frame × 40-candidate Release replay against v0.1.0 also produces
identical binary serialization of every decision, observer-memory field and
pre-existing trace field at default directionality.

## Trace layout cleanup

`noveltyGate` is placed at offset 76, between `eventStrength` and `eventNovelty`.
Local probes compiled the v0.1.0 header, pre-cleanup PR header (`c3ed2d7`) and compact
header: their trace sizes were 96 / 104 / 96 bytes, with all 17 legacy field offsets
identical. The regression now checks every legacy offset, the new offset, size and
alignment on x64. No packing directives, allocation changes or scoring changes are
involved. A trace buffer for 500 candidates returns from 52,000 to 48,000 bytes.

Verified layouts (bytes; MSVC locally and in CI, GCC/Clang through the CI matrix):

| Compiler / target | Size | Alignment | eventStrength | noveltyGate | eventNovelty | noveltyContribution |
|---|---:|---:|---:|---:|---:|---:|
| MSVC 19.38.33145 / Windows x64 | 96 | 8 | 72 | 76 | 80 | 88 |
| MSVC 19.51.36260 / Windows x64 (CI) | 96 | 8 | 72 | 76 | 80 | 88 |
| GCC 13.3.0 / Linux x86-64 | 96 | 8 | 72 | 76 | 80 | 88 |
| Clang 18.1.3 / Linux x86-64 | 96 | 8 | 72 | 76 | 80 | 88 |

An independent 4,000-frame × 40-candidate replay matches every serialized decision,
observer-memory field and trace field, including `noveltyGate`, against the
pre-cleanup PR. It covers directionality 0, .25, .5, .75 and 1, invalid directionality,
beta zero and absent events. A separate default-directionality replay still matches
all pre-existing fields against v0.1.0 exactly. Padding bytes are excluded from
comparisons because they are not diagnostic values.

Focused Release comparison on 2026-10-24: pre-cleanup PR `c3ed2d7` (104-byte trace)
versus layout cleanup `5eeb858` (96-byte trace), using MSVC 19.38.33145 x64, the same
Release flags, Windows desktop and High Performance power policy as above. Each
revision ran the unchanged 44-configuration harness and four reference variants
in three processes with `--runs 5 --cycles 64`. Process order was before/compact,
compact/before, before/compact. No compiler builds ran during timing. Default
inputs and scoring code were unchanged. The tables focus on mixed workloads and
reference variants. Single-observer values use the median of process batch medians;
reference values use the median of process aggregate times, as above.

Batch nanoseconds per Step:

| Configuration | Candidates | Before (104 bytes) | Compact (96 bytes) | Change |
|---|---:|---:|---:|---:|
| mixed/core | 100 | 6434.1 | 6517.7 | +1.3% |
| mixed/core | 500 | 33752.3 | 33807.4 | +0.2% |
| mixed/core+trace | 100 | 6789.2 | 6783.8 | -0.1% |
| mixed/core+trace | 500 | 36225.8 | 34875.5 | -3.7% |
| mixed/novelty | 100 | 6575.2 | 6448.0 | -1.9% |
| mixed/novelty | 500 | 35057.9 | 33783.9 | -3.6% |
| mixed/novelty+trace | 100 | 6876.9 | 6806.2 | -1.0% |
| mixed/novelty+trace | 500 | 35722.1 | 35414.8 | -0.9% |

Reference kernel milliseconds per simulated second:

| Configuration | Before (104 bytes) | Compact (96 bytes) | Change |
|---|---:|---:|---:|
| ref/core | 22.588 | 22.371 | -1.0% |
| ref/core,shared-order | 14.773 | 14.784 | +0.1% |
| ref/novelty | 22.299 | 23.295 | +4.5% |
| ref/novelty+trace | 23.139 | 23.423 | +1.2% |

Across all nonempty single-observer configurations, measured batch changes range
from -3.7% to +1.6%; reference variants range from -1.0% to +4.5%. The observed
+4.5% reference-novelty regression is retained in the results. Run ranges overlap
and desktop scheduling noise is substantial, so these measurements do not establish
a general execution-speed improvement or isolate a layout-caused regression.
The confirmed benefit is the 4,000-byte reduction in 500-entry trace storage.

For mixed/core+trace at 500 candidates, process batch medians are
36,225.8 / 35,355.4 / 39,251.5 ns before and
34,849.4 / 35,240.8 / 34,875.5 ns compact. Per-call p99 ranges are 47.6–59.4 and
48.4–53.8 microseconds respectively. Reference-novelty aggregate process values
span 22.183–24.335 ms before and 22.380–24.347 ms compact per simulated second.
The largest individual single-observer samples across all configurations/processes
are 2.2141 ms before and 3.8266 ms compact. These tails do not imply a latency bound.

All six processes pass status, pairing, replay, trace-on/off and allocation checks,
with zero allocations in 1,802,240 measured single-observer calls and 200,000
reference calls per process. The dedicated prepared allocation test also remains
at zero across 404 calls, including nondefault directionality and rejection paths.
