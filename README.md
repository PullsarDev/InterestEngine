# Interest Engine

Interest Engine is a standalone C++20 library that selects one attention target for
a game NPC. Give it the stimuli your game has already perceived, an observer's
context and preferences, and the previous selection. It returns a target ID or no
target, together with a reason. Hysteresis and optional minimum dwell keep small
score fluctuations from causing constant switching; timestamped events can briefly
raise a stimulus's priority.

The kernel handles selection. Your game supplies perception, stable IDs, time and
snapshots, then resolves the chosen ID into gameplay or presentation. There is no
engine dependency, scene query, object binding, action planner, gaze controller,
animation system, learning model or network service.

**Version 0.1.0:** an early source library with substantive synthetic regression
coverage. API, ABI and tuning compatibility may change before 1.0; there is no
serialized-state format or cross-platform bit-exact simulation guarantee.

**Next release candidate: v0.2.0.** Event directionality adds an optional trailing
`float CandidateSnapshot::eventDirectionality{1.f}` and
`float CandidateTrace::noveltyGate` after `eventStrength`. The trace field occupies
existing padding on tested x64 compilers, preserving its 96-byte size and all
pre-existing field offsets. These change public struct layouts; rebuild
consumers with matching headers and library. Existing aggregate initializers may
omit the new snapshot field and retain legacy behavior. The release version stays
0.1.0 until a release is approved.

Use named fields for trace access; positional `CandidateTrace` aggregate initializers
must account for the inserted field before `eventNovelty`.

## Engineering characteristics

- Standard-library-only production C++20; two implementation files and two headers.
- Explicit host time and caller-owned state; no hidden clock, RNG or global kernel state.
- Prepared `Step()` calls allocate nothing. Setup through `Reserve()` may allocate
  and, with exceptions enabled, throw `std::bad_alloc`.
- Errors at the step level leave memory, decision, scratch and trace untouched.
  Candidate-level errors reject that candidate, including every occurrence of a
  duplicate ID. Invalid numeric inputs are rejected rather than silently repaired.
- Stable ID tie-breaking, candidate-order-independent selection and matching
  decisions with tracing enabled or disabled, covered by regression tests.
- Optional traces expose the values used by the actual scoring and policy path.

Replay is deterministic within a tested build/platform for the same inputs, times,
profile and initial memory. Floating-point math and library implementations can
differ across compilers, platforms and build flags. Avoid fast-math flags if you
depend on the validation and numerical contracts.

## Inputs and decisions

| Input | Purpose |
|---|---|
| `ObserverSnapshot` | Position, reference forward direction and context index; observer ID is bookkeeping only |
| `CandidateSnapshot` | Stable nonzero ID, spatial data, attraction, salience, perception strength, tags and optional event |
| `AttentionProfile` | Scoring weights, spatial gates, thresholds, margins, dwell and novelty parameters; borrows context storage |
| `ContextPreference` | Required/excluded tags, tag affinities and fallback affinity |
| `ObserverMemory` | Previous selection and temporal state; one persistent instance per observer |
| `StepScratch`, optional `DecisionTrace` | Caller-reserved reusable workspace and diagnostics |
| `TimeMs now` | Explicit signed milliseconds, non-decreasing between successful calls for an observer |

Supported stimulus geometry:

| Kind | Gates |
|---|---|
| `Point` | World position, distance cutoff/falloff and angle |
| `Bearing` | World direction and angle; no distance gate |
| `NonSpatial` | No distance or angle penalty |

Positions and distances use consistent host units. Forward and bearing directions
may have any finite nonzero length. `referenceForward` must be a host-owned body,
travel or attention baseline: feeding back the selected target's head/eye direction
would reinforce its own angle score.

For an eligible candidate, the kernel takes the maximum configured affinity among
its matching tag bits, or the fallback when none match. Attraction, salience and
affinity form a weighted mean. Distance, angle and perception strength multiply
that preference into a raw score. There are 64 host-defined tag bits and up to 256
addressable context indices. Each step validates every supplied context entry.

Optional novelty adds `beta * noveltyGate * eventStrength * 2^(-age / halfLife)` to the raw
score, saturating the effective score at 1. An event is the latest host-authored
timestamp/strength pair, resubmitted unchanged while relevant. Re-stamping an old
event every update continually renews it. Zero strength means absent; positive
strength requires a timestamp no later than `now`, even when beta is zero.

`eventDirectionality` controls novelty's angular gate independently of raw preference
scoring: `1` preserves current gating, `0` makes novelty independent of angle, and
intermediate values interpolate linearly:

```text
noveltyAngleGate = 1 - eventDirectionality * (1 - angleGate)
noveltyGate = distanceGate * noveltyAngleGate * perceptionStrength
```

Distance gating and perception strength still apply. At directionality `1`, scoring
reuses the existing rounded `CandidateTrace::gate` exactly; otherwise it rounds the
new gate once for both scoring and `CandidateTrace::noveltyGate`. Directionality must
be finite and within `[0,1]`, including when event strength or beta is zero; invalid
values reject the candidate as `InvalidEvent` at the existing event-validation stage.
Raw scoring and temporal policy are unchanged. The host still controls perception
strength, event generation and candidate range.

Selection acquires at or above `acquireThreshold`. An eligible incumbent is protected by
the release threshold, positive switch margin and optional minimum dwell. A large
enough challenger can break dwell through `overrideMargin`. An absent or ineligible
incumbent is released or replaced immediately. Exact score ties use the lower ID
for acquisition and never displace an incumbent. Dwell is observed only at steps,
so response timing depends on the host update interval.

`Decision` reports `Acquire`, `Keep`, `Switch`, `Clear` or `None`, the previous and
selected IDs, scores and a `DecisionReason`. `kNoCandidate` is zero. Check
`StepStatus` before consuming a decision: an error leaves the old output in place.
The validation order is profile, observer, time, then capacity. See
[`attention.hpp`](include/interest/attention.hpp) for exact parameter ranges,
rejection precedence and ownership contracts.

## Build and test

Requirements: CMake 3.20 or newer, a C++20 compiler and its standard library. The
validation matrix covers Windows x64 MSVC (Visual Studio 2022) and Linux x86-64
GCC/Clang, in Debug and Release, plus Clang ASan/UBSan. The recorded performance
baseline uses MSVC 19.38. This is a tested matrix, not a promise that every compiler
version or platform is supported. macOS, ARM and other architectures are unverified.

From a compiler-enabled shell:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DINTEREST_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

For an explicit Windows Visual Studio generator, configure with:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DINTEREST_WARNINGS_AS_ERRORS=ON
```

Use the same build/test commands above. `CMAKE_BUILD_TYPE` controls single-config
generators; Visual Studio uses `--config` / `-C`. Repeat with Debug to exercise that
configuration. Tests use checks that remain active in optimized builds.

| CMake option | Default when standalone | Purpose |
|---|---|---|
| `INTEREST_BUILD_TESTS` | `ON` | Regression tests and CTest registration |
| `INTEREST_BUILD_BENCHMARKS` | `ON` | Benchmark executable; smoke test when tests are enabled |
| `INTEREST_BUILD_EXAMPLES` | `ON` | Build the example and test it when tests are enabled |
| `INTEREST_WARNINGS_AS_ERRORS` | `OFF` | Project warnings become errors |
| `INTEREST_CONSTRAINED_BUILD` | `OFF` | Generic no-exceptions/no-RTTI build check with shadow warnings |

Tests, benchmarks and examples default to `OFF` under `add_subdirectory()`.
The constrained configuration verifies compilation with those flags; it does not
remove `Reserve()`'s need for successful setup allocation.

## Integrate with CMake

Place a source checkout under your project, for example at `external/InterestEngine`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyGame LANGUAGES CXX)

add_subdirectory(external/InterestEngine)
add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE InterestEngine::Core)
```

`InterestEngine::Core` aliases the static `interest_core` target and supplies its
include path and C++20 requirement. Warning policy stays on this project's targets.
There is no package manager, download step or installed `find_package()` package.
The executable example below is also built by the parent-project fixture in
[`examples/consumer`](examples/consumer/CMakeLists.txt).

## Example

This complete program is [`examples/basic.cpp`](examples/basic.cpp), built as
`interest_basic_example` and executed by CTest. The zero-initialized profile is
deliberately invalid until the required ranges, thresholds, margins and contexts
are supplied. Context storage outlives both steps; memory persists across them;
scratch and optional trace capacity are reserved before stepping.

```cpp
// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

#include "interest/attention.hpp"

#include <array>
#include <cstdio>

int main()
{
    // The profile borrows this storage. Keep it alive through every Step call.
    std::array<interest::ContextPreference, 1> contexts{};
    contexts[0].affinityTags = 1;
    contexts[0].tagAffinity[0] = 0.8f;

    interest::AttentionProfile profile{};
    profile.contexts = contexts;
    profile.fullRange = 10.f;
    profile.maxRange = 30.f;
    profile.acquireThreshold = 0.5f;
    profile.releaseThreshold = 0.35f;
    profile.switchMargin = 0.1f;
    profile.overrideMargin = 0.3f;
    profile.minDwellMs = 400;
    profile.noveltyBeta = 0.25f;
    profile.noveltyHalfLifeMs = 1000;
    if (interest::ValidateProfile(profile) != interest::StepStatus::Ok) {
        return 1;
    }

    interest::ObserverMemory memory{};  // one per observer; persist between calls
    interest::StepScratch scratch{};
    interest::DecisionTrace trace{};    // optional
    interest::Reserve(scratch, 8);      // setup may allocate; Step will not
    interest::Reserve(trace, 8);

    interest::ObserverSnapshot observer{};
    observer.id = 1;
    observer.referenceForward = {1.f, 0.f, 0.f};  // body/travel baseline
    observer.context = 0;

    std::array<interest::CandidateSnapshot, 1> candidates{};
    auto& candidate = candidates[0];
    candidate.id = 42;
    candidate.position = {5.0, 0.0, 0.0};
    candidate.attraction = 0.8f;
    candidate.salience = 0.7f;
    candidate.tags = 1;
    candidate.eventTimeMs = 1000;
    candidate.eventStrength = 1.f;

    // Explicit monotonic host time; the event timestamp stays unchanged on replay.
    for (const interest::TimeMs now : {1000, 1200}) {
        interest::Decision decision{};
        const auto status = interest::Step(observer, candidates, profile, now,
                                           memory, scratch, decision, &trace);
        if (status != interest::StepStatus::Ok) {
            std::fprintf(stderr, "Step failed: %s\n", interest::ToString(status));
            return 1;
        }
        switch (decision.kind) {
        case interest::DecisionKind::Acquire:
        case interest::DecisionKind::Keep:
        case interest::DecisionKind::Switch:
            // The host resolves the ID and drives gameplay or presentation.
            std::printf("%s %llu (%s)\n", interest::ToString(decision.kind),
                        static_cast<unsigned long long>(decision.selected),
                        interest::ToString(decision.reason));
            break;
        case interest::DecisionKind::Clear:
        case interest::DecisionKind::None:
            std::puts("No attention target");
            break;
        }
        if (decision.selected != 42 ||
            decision.kind != (now == 1000 ? interest::DecisionKind::Acquire
                                         : interest::DecisionKind::Keep)) {
            return 1;
        }
    }
    return 0;
}
```

Expected output:

```text
Acquire 42 (HighestScore)
Keep 42 (HighestScore)
```

Keep observer memory value-initialized or reset it with `ResetMemory()`; let `Step()`
maintain it thereafter. Resetting memory does not make an old event new. Reserve
for the largest submitted candidate count, including rejected candidates. Reserve
again after copying scratch/trace buffers because copying need not retain capacity.
Independent concurrent calls need distinct mutable memory, scratch, output and
trace, with snapshot/profile/context data held immutable during each call. Host
scheduling and synchronization are outside this library's validation scope.

## Benchmarks

CTest runs `interest_step_benchmark --smoke` to check the harness. For measurements,
run the optimized executable manually:

```sh
./build/interest_step_benchmark --runs 5 --cycles 128 --raw build/samples.csv
```

With Visual Studio, use `./build/Release/interest_step_benchmark.exe` instead.
See [workloads, methodology and measured results](docs/benchmarks.md). These are
kernel-only synthetic measurements; perception, snapshot gathering and animation
must be budgeted separately.

## Limits and stability

- By default spatial gates also attenuate novelty. Lower event directionality can
  let behind events contribute novelty without changing their raw preference score.
  Distance cutoff and zero-perception rejection still apply; the kernel has no
  semantic understanding of importance.
- Score saturation can hide a challenger's unsaturated advantage. Positive margins
  can retain a target after novelty fades. Thresholds, weights, beta, half-life and
  update cadence need host-specific tuning; defaults are not a gameplay policy.
- Work scales with submitted candidates and contexts. ID sorting and searches,
  geometry, event math and optional trace writes have workload-dependent cost.
  There is no real-time deadline or frame-budget guarantee.
- Memory holds only temporal selection state. There is no familiarity decay,
  fatigue, candidate/event history, lost-target grace or multi-observer coordination.
- Validated scores stay bounded, but diagnostic fields may echo invalid inputs;
  extreme finite coordinates may produce an infinite diagnostic distance and an
  out-of-range rejection. Do not treat every trace field as a finite valid score.
- Regression scenarios and baseline comparisons are synthetic engineering tests.
  They do not establish perceptual realism, biological validity or gameplay quality.

See [contribution and validation guidance](CONTRIBUTING.md) for the test map and
change expectations.

## License

Copyright 2026 PullsarDev. Licensed under the [Apache License 2.0](LICENSE).
Test-support attribution is retained in [third-party notices](THIRD_PARTY_NOTICES.md).
