// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Kernel latency, throughput, allocation and memory measurements of the production
// Step(). Benchmark infrastructure only: it never reimplements scoring or policy. Every
// input frame is generated, and every scratch/trace/sample buffer reserved, before any
// timed region. A timed region contains one Step call (per-call mode) or one cycle of
// consecutive Step calls (batch mode).
//
// Compared variants are paired: event on/off variants consume the same RNG sequence and
// differ only in event fields; fixed and reshuffled orders submit the same snapshots per
// frame, permuted by a separate order RNG. CheckPairedConfigs/CheckReferencePairing
// assert this on the generated data in every mode, including --smoke.
//
// Usage: interest_step_benchmark [--smoke] [--runs R] [--cycles C] [--raw samples.csv]
//   --smoke   tiny counts for CTest; checks the harness invariants, makes no timing claim.
//   --runs    repetitions of the whole matrix and of the reference workload (default 5).
//   --cycles  replays of each 64-frame sequence per run and mode (default 128).
//   --raw     writes every per-call sample as CSV (section,config,run,index,ns).
//
// The method and recorded results are in docs/benchmarks.md.

#include "test_support.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <numbers>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#elif defined(__linux__)
#include <sys/resource.h>
#endif

// Counts every global allocation, as in interest_allocation_test. Measured regions must
// leave this unchanged.
namespace {
std::size_t allocationCount = 0;
} // namespace

// Optimized GCC inlines these replacements into std::allocator code and then reports the
// free() as mismatched with operator new (-Wmismatched-new-delete), although the pair is
// malloc/free. Keeping them out of line avoids that false positive.
#if defined(__GNUC__) && !defined(__clang__)
#define INTEREST_BENCH_NOINLINE __attribute__((noinline))
#else
#define INTEREST_BENCH_NOINLINE
#endif

INTEREST_BENCH_NOINLINE void* operator new(std::size_t size)
{
    ++allocationCount;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    std::abort();  // keeps the replacement valid when exceptions are disabled
}

INTEREST_BENCH_NOINLINE void operator delete(void* memory) noexcept
{
    std::free(memory);
}

INTEREST_BENCH_NOINLINE void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace {

using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::Decision;
using interest::DecisionKind;
using interest::DecisionTrace;
using interest::Eligibility;
using interest::ObserverMemory;
using interest::ObserverSnapshot;
using interest::SpatialKind;
using interest::StepScratch;
using interest::StepStatus;
using interest::TagMask;
using interest::TimeMs;

using Clock = std::chrono::steady_clock;

constexpr TimeMs kFrameMs = 200;          // 5 Hz observer update
constexpr TimeMs kEventPeriodMs = 2000;   // an eventful source refreshes its event this often
constexpr TagMask kRequiredTag = 1;       // context 0 requires bit 0 ...
constexpr TagMask kExcludedTag = 2;       // ... and excludes bit 1
constexpr TagMask kAffinityTags = 0x3FC;  // bits 2..9 carry context affinities

struct Options {
    bool smoke = false;
    int runs = 5;
    int cycles = 128;
    std::size_t frames = 64;
    const char* rawPath = nullptr;
};

bool failed = false;

void Require(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "HARNESS CHECK FAILED: %s\n", what);
        failed = true;
    }
}

std::int64_t Nanoseconds(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

double Uniform(test::Random& rng, double lo, double hi)
{
    return lo + (hi - lo) * static_cast<double>(rng.Below(1u << 20)) / double(1u << 20);
}

// Decision fingerprint. It keeps every Step result observable (Step is an out-of-line
// call into the static library, so it cannot be elided anyway) and lets runs, modes and
// trace on/off variants be compared for identical decisions.
std::uint64_t Fold(std::uint64_t hash, const Decision& d)
{
    const std::uint64_t packed = std::uint64_t{std::bit_cast<std::uint32_t>(d.effectiveScore)} |
                                 (std::uint64_t{static_cast<std::uint8_t>(d.kind)} << 32) |
                                 (std::uint64_t{static_cast<std::uint8_t>(d.reason)} << 40);
    hash = (hash ^ d.selected) * 0x100000001B3ull;
    return (hash ^ packed) * 0x100000001B3ull;
}

constexpr std::uint64_t kHashSeed = 0xCBF29CE484222325ull;

// Nearest-rank percentile of sorted values; q is in units of 1/100000.
std::int64_t Percentile(const std::vector<std::int64_t>& sorted, std::uint64_t q)
{
    if (sorted.empty()) {
        return 0;
    }
    const std::uint64_t n = sorted.size();
    const std::uint64_t rank = (q * n + 99999) / 100000;
    return sorted[static_cast<std::size_t>(std::clamp<std::uint64_t>(rank, 1, n) - 1)];
}

struct Distribution {
    double mean = 0;
    std::int64_t min = 0, p50 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0;
    std::size_t count = 0;
};

Distribution Summarize(std::vector<std::int64_t> values)
{
    Distribution d;
    if (values.empty()) {
        return d;
    }
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (const std::int64_t v : values) {
        sum += static_cast<double>(v);
    }
    d.count = values.size();
    d.mean = sum / static_cast<double>(values.size());
    d.min = values.front();
    d.p50 = Percentile(values, 50000);
    d.p95 = Percentile(values, 95000);
    d.p99 = Percentile(values, 99000);
    d.p999 = Percentile(values, 99900);
    d.max = values.back();
    return d;
}

// --- Profile and contexts -----------------------------------------------------------

std::vector<ContextPreference> MakeContexts()
{
    std::vector<ContextPreference> contexts(2);
    contexts[0].requiredTags = kRequiredTag;
    contexts[0].excludedTags = kExcludedTag;
    contexts[1].affinityTags = kAffinityTags;
    contexts[0].affinityTags = kAffinityTags;
    for (std::size_t bit = 2; bit < 10; ++bit) {
        contexts[0].tagAffinity[bit] = static_cast<float>(bit - 1) / 8.f;
        contexts[1].tagAffinity[bit] = static_cast<float>(10 - bit) / 8.f;
    }
    return contexts;
}

AttentionProfile MakeProfile(const std::vector<ContextPreference>& contexts, float beta)
{
    AttentionProfile profile{};
    profile.fullRange = 10.f;
    profile.maxRange = 40.f;
    profile.angleFloor = 0.2f;
    profile.angleSharpness = 2.f;
    profile.acquireThreshold = 0.5f;
    profile.releaseThreshold = 0.4f;
    profile.switchMargin = 0.08f;
    profile.overrideMargin = 0.2f;
    profile.minDwellMs = 0;  // product default: dwell off
    profile.contexts = contexts;
    profile.noveltyBeta = beta;
    profile.noveltyHalfLifeMs = 1000;
    return profile;
}

// --- Single-observer workloads --------------------------------------------------------

enum class Spatial : std::uint8_t { Mixed, Front, Surround, Far };
enum class Reject : std::uint8_t {
    None, Disabled, NotPerceived, MissingTag, ExcludedTag, OutOfRange, Duplicate };

struct Workload {
    const char* name;
    Spatial spatial;
    int rejectPercent;  // share of sources made ineligible, spread over six reasons
    bool replace;       // 25% of sources replaced by new IDs every frame
    bool reshuffle;     // submission order reshuffled every frame
};

struct Variant {
    const char* name;
    bool events;  // 25% of sources carry a timestamped event
    float beta;
    bool trace;
};

// Generator state for one simulated stimulus; frames are materialized from it.
struct Source {
    CandidateSnapshot base;
    Reject reject = Reject::None;
    bool eventful = false;
    TimeMs eventPhase = 0;
    float eventStrength = 0.f;
};

Source MakeSource(test::Random& rng, const Workload& w, bool events, std::size_t slot)
{
    Source s;
    CandidateSnapshot& c = s.base;
    c.id = rng.Next() | 1;  // random nonzero IDs, so the duplicate-detection sort does real work
    c.spatial = w.spatial == Spatial::Mixed ? static_cast<SpatialKind>(slot % 3)
                                            : SpatialKind::Point;
    const double pi = std::numbers::pi;
    const double angle = w.spatial == Spatial::Front ? Uniform(rng, -pi / 3, pi / 3)
                                                     : Uniform(rng, -pi, pi);
    double distance = Uniform(rng, 1.0, 38.0);
    if (w.spatial == Spatial::Far && rng.Below(5) != 0) {
        distance = Uniform(rng, 45.0, 100.0);  // beyond the 40-unit cutoff
    }
    c.position = {distance * std::cos(angle), distance * std::sin(angle), Uniform(rng, -1, 1)};
    c.bearing = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)), 0.f};
    c.attraction = rng.Unit();
    c.salience = rng.Unit();
    c.perceptionStrength = 0.25f + 0.75f * rng.Unit();
    c.tags = kRequiredTag | (TagMask{1} << (2 + rng.Below(8))) | (TagMask{1} << (2 + rng.Below(8)));
    if (static_cast<int>(rng.Below(100)) < w.rejectPercent) {
        s.reject = static_cast<Reject>(1 + rng.Below(6));
        switch (s.reject) {
        case Reject::Disabled: c.enabled = false; break;
        case Reject::NotPerceived: c.perceptionStrength = 0.f; break;
        case Reject::MissingTag: c.tags &= ~kRequiredTag; break;
        case Reject::ExcludedTag: c.tags |= kExcludedTag; break;
        case Reject::OutOfRange:
            if (c.spatial == SpatialKind::Point) {
                c.position = {60.0 * std::cos(angle), 60.0 * std::sin(angle), 0.0};
            } else {
                c.enabled = false;  // only Points have a range; recorded in the histogram
            }
            break;
        default: break;  // Duplicate is applied when a frame is materialized
        }
    }
    // Event draws happen whether or not events are enabled, so event-on/off variants of a
    // workload consume the same RNG sequence and differ only in the event fields.
    const bool eventful = rng.Below(4) == 0;
    s.eventful = events && eventful;
    s.eventPhase = static_cast<TimeMs>(rng.Below(static_cast<std::uint64_t>(kEventPeriodMs)));
    s.eventStrength = 0.5f + 0.5f * rng.Unit();
    return s;
}

CandidateSnapshot Materialize(const Source& s, test::Random& rng, TimeMs now)
{
    CandidateSnapshot c = s.base;
    if (c.spatial == SpatialKind::Point) {
        c.position.x += Uniform(rng, -0.25, 0.25);
        c.position.y += Uniform(rng, -0.25, 0.25);
    }
    c.salience = std::clamp(c.salience + 0.0625f * (rng.Unit() - 0.5f), 0.f, 1.f);
    if (s.eventful && now >= s.eventPhase) {
        c.eventTimeMs = now - (now - s.eventPhase) % kEventPeriodMs;
        c.eventStrength = s.eventStrength;
    }
    return c;
}

// Candidate data comes from `seed` alone and is materialized in canonical slot order;
// a reshuffling workload then permutes each finished frame with a separate order RNG.
// Fixed-order and reshuffled variants with the same seed therefore submit identical
// snapshots per frame, differing only in submission order.
std::vector<std::vector<CandidateSnapshot>> MakeFrames(const Workload& w, bool events,
                                                       std::size_t n, std::size_t frameCount,
                                                       std::uint64_t seed)
{
    test::Random rng(seed);
    test::Random orderRng(seed ^ 0x0DE5C0DEull);
    std::vector<Source> sources;
    for (std::size_t i = 0; i < n; ++i) {
        sources.push_back(MakeSource(rng, w, events, i));
    }
    std::vector<std::vector<CandidateSnapshot>> frames(frameCount);
    for (std::size_t f = 0; f < frameCount; ++f) {
        if (w.replace && f > 0 && n > 0) {
            for (std::size_t k = 0; k < n / 4; ++k) {
                const auto slot = static_cast<std::size_t>(rng.Below(n));
                sources[slot] = MakeSource(rng, w, events, slot);
            }
        }
        const TimeMs now = static_cast<TimeMs>(f) * kFrameMs;
        for (std::size_t i = 0; i < n; ++i) {
            frames[f].push_back(Materialize(sources[i], rng, now));
            if (sources[i].reject == Reject::Duplicate && i > 0) {
                frames[f][i].id = frames[f][i - 1].id;  // both occurrences are rejected
            }
        }
        if (w.reshuffle && f > 0) {
            orderRng.Shuffle(frames[f]);
        }
    }
    return frames;
}

struct Config {
    std::string label;
    const Workload* workload;
    const Variant* variant;
    std::size_t n;
    std::vector<std::vector<CandidateSnapshot>> frames;
    std::uint64_t checksum = 0;               // one cycle's decision fingerprint
    std::vector<std::int64_t> perCall;        // pooled per-call samples, all runs
    std::vector<std::int64_t> perCallRunP50;  // per-run medians
    std::vector<double> batchMean;            // per-run batch mean ns/step
    // Untimed traced characterization of one cycle.
    std::size_t eligible = 0, entries = 0, switches = 0, clears = 0, acquires = 0;
    std::size_t rejected[16]{};
};

// Workspace a host would keep: per-observer memory plus caller-prepared buffers.
struct Workspace {
    ObserverMemory memory{};
    StepScratch scratch;
    DecisionTrace trace;
    Decision output{};
};

// One untimed cycle; returns its fingerprint and, when traced, characterizes it.
std::uint64_t Cycle(Config& c, const ObserverSnapshot& observer, const AttentionProfile& profile,
                    Workspace& ws, bool characterize)
{
    interest::ResetMemory(ws.memory);
    std::uint64_t hash = kHashSeed;
    for (std::size_t f = 0; f < c.frames.size(); ++f) {
        DecisionTrace* trace = c.variant->trace || characterize ? &ws.trace : nullptr;
        const StepStatus status =
            interest::Step(observer, c.frames[f], profile, static_cast<TimeMs>(f) * kFrameMs,
                           ws.memory, ws.scratch, ws.output, trace);
        Require(status == StepStatus::Ok, "warm-up Step returned Ok");
        hash = Fold(hash, ws.output);
        if (characterize) {
            for (const interest::CandidateTrace& e : ws.trace.candidates) {
                ++c.entries;
                if (e.eligibility == Eligibility::Eligible) {
                    ++c.eligible;
                } else {
                    ++c.rejected[static_cast<std::size_t>(e.eligibility)];
                }
            }
            c.switches += ws.output.kind == DecisionKind::Switch;
            c.clears += ws.output.kind == DecisionKind::Clear;
            c.acquires += ws.output.kind == DecisionKind::Acquire;
        }
    }
    return hash;
}

void MeasureRun(Config& c, const ObserverSnapshot& observer, const AttentionProfile& profile,
                Workspace& ws, const Options& options, std::vector<std::int64_t>& runSamples)
{
    DecisionTrace* trace = c.variant->trace ? &ws.trace : nullptr;
    const std::size_t frameCount = c.frames.size();

    // Warm-up: one untimed cycle (caches, branch predictors, page faults).
    Require(Cycle(c, observer, profile, ws, false) == c.checksum, "warm-up replay identical");

    const std::size_t allocationsBefore = allocationCount;
    bool allOk = true;
    bool identical = true;

    // Per-call mode: one Step per timed region.
    runSamples.clear();  // capacity reserved by the caller
    for (int cycle = 0; cycle < options.cycles; ++cycle) {
        interest::ResetMemory(ws.memory);
        std::uint64_t hash = kHashSeed;
        for (std::size_t f = 0; f < frameCount; ++f) {
            const TimeMs now = static_cast<TimeMs>(f) * kFrameMs;
            const Clock::time_point begin = Clock::now();
            const StepStatus status = interest::Step(observer, c.frames[f], profile, now,
                                                     ws.memory, ws.scratch, ws.output, trace);
            const Clock::time_point end = Clock::now();
            runSamples.push_back(Nanoseconds(begin, end));
            allOk = allOk && status == StepStatus::Ok;
            hash = Fold(hash, ws.output);
        }
        identical = identical && hash == c.checksum;
    }

    // Batch mode: one timed region per cycle, without per-call timer overhead. The
    // fingerprint fold (two multiply-xors per step) stays inside the timed region.
    std::int64_t batchNs = 0;
    for (int cycle = 0; cycle < options.cycles; ++cycle) {
        interest::ResetMemory(ws.memory);
        std::uint64_t hash = kHashSeed;
        bool ok = true;
        const Clock::time_point begin = Clock::now();
        for (std::size_t f = 0; f < frameCount; ++f) {
            ok = interest::Step(observer, c.frames[f], profile, static_cast<TimeMs>(f) * kFrameMs,
                                ws.memory, ws.scratch, ws.output, trace) == StepStatus::Ok && ok;
            hash = Fold(hash, ws.output);
        }
        const Clock::time_point end = Clock::now();
        batchNs += Nanoseconds(begin, end);
        allOk = allOk && ok;
        identical = identical && hash == c.checksum;
    }

    Require(allocationCount == allocationsBefore, "no allocations in measured Step regions");
    Require(allOk, "every measured Step returned Ok");
    Require(identical, "measured cycles replay the warm-up decisions exactly");

    c.perCall.insert(c.perCall.end(), runSamples.begin(), runSamples.end());
    std::vector<std::int64_t> sorted = runSamples;
    std::sort(sorted.begin(), sorted.end());
    c.perCallRunP50.push_back(Percentile(sorted, 50000));
    c.batchMean.push_back(static_cast<double>(batchNs) /
                          static_cast<double>(frameCount * static_cast<std::size_t>(options.cycles)));
}

// --- Reference workload: 100 observers x 500 candidates x 5 Hz --------------------------

constexpr std::size_t kRefObservers = 100;
constexpr std::size_t kRefCandidates = 500;
constexpr std::size_t kRefTicks = 50;  // 10 simulated seconds at 5 Hz
constexpr auto kTicksPerSecond = static_cast<std::size_t>(1000 / kFrameMs);

struct WorldSource {
    Source source;
    double vx = 0, vy = 0;  // units per second, Points only
};

WorldSource MakeWorldSource(test::Random& rng, bool events)
{
    WorldSource w;
    CandidateSnapshot& c = w.source.base;
    c.id = rng.Next() | 1;
    const std::uint64_t kind = rng.Below(20);  // 70% Point, 15% Bearing, 15% NonSpatial
    c.spatial = kind < 14 ? SpatialKind::Point
              : kind < 17 ? SpatialKind::Bearing : SpatialKind::NonSpatial;
    const double pi = std::numbers::pi;
    const double angle = Uniform(rng, -pi, pi);
    const double radius = 60.0 * std::sqrt(Uniform(rng, 0, 1));  // uniform over a disc
    c.position = {radius * std::cos(angle), radius * std::sin(angle), Uniform(rng, 0, 2)};
    c.bearing = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)), 0.f};
    const double heading = Uniform(rng, -pi, pi);
    const double speed = Uniform(rng, 0, 1.5);
    w.vx = speed * std::cos(heading);
    w.vy = speed * std::sin(heading);
    c.attraction = rng.Unit();
    c.salience = rng.Unit();
    c.perceptionStrength = 0.25f + 0.75f * rng.Unit();
    c.tags = (TagMask{1} << (2 + rng.Below(8))) | (TagMask{1} << (2 + rng.Below(8)));
    if (rng.Below(10) != 0) {
        c.tags |= kRequiredTag;  // 90% satisfy context 0
    }
    if (rng.Below(33) == 0) {
        c.tags |= kExcludedTag;
    }
    c.enabled = rng.Below(20) != 0;  // 5% disabled
    const bool eventful = rng.Below(4) == 0;  // drawn either way: see MakeSource
    w.source.eventful = events && eventful;
    w.source.eventPhase = static_cast<TimeMs>(rng.Below(static_cast<std::uint64_t>(kEventPeriodMs)));
    w.source.eventStrength = 0.5f + 0.5f * rng.Unit();
    return w;
}

std::vector<std::vector<CandidateSnapshot>> MakeWorldFrames(bool events, std::size_t ticks)
{
    test::Random rng(0xB3E7C0DEull);
    std::vector<WorldSource> world;
    for (std::size_t i = 0; i < kRefCandidates; ++i) {
        world.push_back(MakeWorldSource(rng, events));
    }
    std::vector<std::vector<CandidateSnapshot>> frames(ticks);
    for (std::size_t t = 0; t < ticks; ++t) {
        if (t > 0) {
            for (int k = 0; k < 10; ++k) {  // 2% of sources replaced per tick
                world[static_cast<std::size_t>(rng.Below(kRefCandidates))] = MakeWorldSource(rng, events);
            }
        }
        const TimeMs now = static_cast<TimeMs>(t) * kFrameMs;
        const double seconds = static_cast<double>(kFrameMs) / 1000.0;
        for (WorldSource& w : world) {
            if (w.source.base.spatial == SpatialKind::Point) {
                w.source.base.position.x += w.vx * seconds;
                w.source.base.position.y += w.vy * seconds;
            }
            frames[t].push_back(Materialize(w.source, rng, now));
        }
    }
    return frames;
}

std::vector<ObserverSnapshot> MakeObservers()
{
    test::Random rng(0x0B5E7E25ull);
    std::vector<ObserverSnapshot> observers(kRefObservers);
    const double pi = std::numbers::pi;
    for (std::size_t i = 0; i < kRefObservers; ++i) {
        const double angle = Uniform(rng, -pi, pi);
        const double radius = 30.0 * std::sqrt(Uniform(rng, 0, 1));
        const double facing = Uniform(rng, -pi, pi);
        observers[i].id = i + 1;
        observers[i].position = {radius * std::cos(angle), radius * std::sin(angle), 0.0};
        observers[i].referenceForward = {static_cast<float>(std::cos(facing)),
                                         static_cast<float>(std::sin(facing)), 0.f};
        observers[i].context = static_cast<interest::ContextId>(i % 2);
    }
    return observers;
}

// Each observer submits the same world in its own gather order (a fixed per-observer
// permutation, as an observer-specific spatial query would produce), copied into its own
// host buffer before the tick, outside every timed region. With sharedOrder all
// observers use the world order instead; only the order differs.
void Gather(const std::vector<CandidateSnapshot>& world,
            const std::vector<std::vector<std::size_t>>& orders,
            std::vector<std::vector<CandidateSnapshot>>& gathered)
{
    for (std::size_t o = 0; o < gathered.size(); ++o) {
        for (std::size_t i = 0; i < world.size(); ++i) {
            gathered[o][i] = world[orders[o][i]];
        }
    }
}

// One reference-workload variant: its world, observers and host-owned state. Variants
// are measured interleaved (every run measures every variant once), so machine drift
// over the session lands on all variants alike instead of separating them.
class ReferenceVariant {
public:
    ReferenceVariant(const char* name, bool events, float beta, bool traced, bool sharedOrder,
                     const std::vector<ContextPreference>& contexts, const Options& options)
        : name_(name), events_(events), beta_(beta), traced_(traced), sharedOrder_(sharedOrder),
          ticks_(options.smoke ? 3 : kRefTicks), world_(MakeWorldFrames(events, ticks_)),
          observers_(MakeObservers()), profile_(MakeProfile(contexts, beta)),
          orders_(kRefObservers, std::vector<std::size_t>(kRefCandidates)),
          frames_(kRefObservers, std::vector<CandidateSnapshot>(kRefCandidates)),
          memories_(kRefObservers)
    {
        test::Random orderRng(0x0D0E5ull);
        for (std::vector<std::size_t>& order : orders_) {
            for (std::size_t i = 0; i < kRefCandidates; ++i) {
                order[i] = i;
            }
            if (!sharedOrder_) {
                orderRng.Shuffle(order);
            }
        }
        // Host-owned state: one memory per observer; one scratch/trace per (single)
        // worker, since scratch contents are unspecified between calls and the trace is
        // read at once.
        interest::Reserve(scratch_, kRefCandidates);
        interest::Reserve(trace_, kRefCandidates);
        observerSum_.assign(kRefObservers, 0.0);
        observerEligible_.assign(kRefObservers, 0.0);
        const std::size_t stepsPerPass = ticks_ * kRefObservers;
        steps_.reserve(stepsPerPass * static_cast<std::size_t>(options.runs));
        ticksNs_.reserve(ticks_ * static_cast<std::size_t>(options.runs));
        runTickMean_.reserve(static_cast<std::size_t>(options.runs));
        passSamples_.reserve(stepsPerPass);

        // Untimed traced pass: warm-up, reference fingerprint and workload characterization.
        ResetMemories();
        std::uint64_t hash = kHashSeed;
        for (std::size_t t = 0; t < ticks_; ++t) {
            Gather(world_[t], orders_, frames_);  // host gathering, untimed
            for (std::size_t o = 0; o < kRefObservers; ++o) {
                Require(interest::Step(observers_[o], frames_[o], profile_,
                                       static_cast<TimeMs>(t) * kFrameMs, memories_[o], scratch_,
                                       output_, &trace_) == StepStatus::Ok,
                        "reference warm-up Step returned Ok");
                hash = Fold(hash, output_);
                std::size_t eligible = 0;
                for (const interest::CandidateTrace& e : trace_.candidates) {
                    eligible += e.eligibility == Eligibility::Eligible;
                }
                observerEligible_[o] += static_cast<double>(eligible) / static_cast<double>(ticks_);
                switches_ += output_.kind == DecisionKind::Switch;
                clears_ += output_.kind == DecisionKind::Clear;
                acquires_ += output_.kind == DecisionKind::Acquire;
            }
        }
        checksum_ = hash;
    }

    // One run: pass 1 times every Step individually; pass 2 times each tick's 100 Steps
    // back to back, as one worker would run them.
    void MeasureRun(int run, std::FILE* raw)
    {
        DecisionTrace* trace = traced_ ? &trace_ : nullptr;
        const std::size_t allocationsBefore = allocationCount;
        ResetMemories();
        std::uint64_t hash = kHashSeed;
        passSamples_.clear();
        for (std::size_t t = 0; t < ticks_; ++t) {
            Gather(world_[t], orders_, frames_);  // host gathering, untimed
            const TimeMs now = static_cast<TimeMs>(t) * kFrameMs;
            for (std::size_t o = 0; o < kRefObservers; ++o) {
                const Clock::time_point begin = Clock::now();
                const StepStatus status = interest::Step(observers_[o], frames_[o], profile_, now,
                                                         memories_[o], scratch_, output_, trace);
                const Clock::time_point end = Clock::now();
                const std::int64_t ns = Nanoseconds(begin, end);
                passSamples_.push_back(ns);
                observerSum_[o] += static_cast<double>(ns);
                allOk_ = allOk_ && status == StepStatus::Ok;
                hash = Fold(hash, output_);
            }
        }
        identical_ = identical_ && hash == checksum_;
        steps_.insert(steps_.end(), passSamples_.begin(), passSamples_.end());

        ResetMemories();
        hash = kHashSeed;
        std::int64_t runTotal = 0;
        for (std::size_t t = 0; t < ticks_; ++t) {
            Gather(world_[t], orders_, frames_);  // host gathering, untimed
            const TimeMs now = static_cast<TimeMs>(t) * kFrameMs;
            bool ok = true;
            const Clock::time_point begin = Clock::now();
            for (std::size_t o = 0; o < kRefObservers; ++o) {
                ok = interest::Step(observers_[o], frames_[o], profile_, now, memories_[o],
                                    scratch_, output_, trace) == StepStatus::Ok && ok;
                hash = Fold(hash, output_);
            }
            const Clock::time_point end = Clock::now();
            ticksNs_.push_back(Nanoseconds(begin, end));
            runTotal += ticksNs_.back();
            allOk_ = allOk_ && ok;
        }
        identical_ = identical_ && hash == checksum_;
        runTickMean_.push_back(static_cast<double>(runTotal) / static_cast<double>(ticks_));
        allocationFree_ = allocationFree_ && allocationCount == allocationsBefore;

        if (raw != nullptr) {  // outside both passes
            for (std::size_t i = 0; i < passSamples_.size(); ++i) {
                std::fprintf(raw, "reference,%s,%d,%zu,%lld\n", name_, run, i,
                             static_cast<long long>(passSamples_[i]));
            }
        }
    }

    // Checks the run invariants and, outside smoke mode, prints the distributions.
    std::uint64_t Report(const Options& options) const
    {
        Require(allocationFree_, "no allocations in reference Step regions");
        Require(allOk_, "every reference Step returned Ok");
        Require(identical_, "reference passes replay the warm-up decisions exactly");
        if (options.smoke) {
            return checksum_;
        }
        const Distribution step = Summarize(steps_);
        const Distribution tick = Summarize(ticksNs_);
        std::printf("\n[%s] events %s, beta %.2f, trace %s, %s; %zu observers x %zu candidates,"
                    " %zu ticks (%zu s simulated) x %d runs\n", name_, events_ ? "on" : "off",
                    static_cast<double>(beta_), traced_ ? "on" : "off",
                    sharedOrder_ ? "one shared submission order" : "per-observer gather order",
                    kRefObservers, kRefCandidates, ticks_, ticks_ / kTicksPerSecond, options.runs);
        std::printf("  per-Step ns  (n=%zu): mean %.0f  p50 %lld  p95 %lld  p99 %lld  p99.9 %lld  max %lld\n",
                    step.count, step.mean, static_cast<long long>(step.p50),
                    static_cast<long long>(step.p95), static_cast<long long>(step.p99),
                    static_cast<long long>(step.p999), static_cast<long long>(step.max));
        std::printf("  per-tick us  (n=%zu, 100 Steps back to back): mean %.1f  p50 %.1f  p99 %.1f  max %.1f\n",
                    tick.count, tick.mean / 1000.0, static_cast<double>(tick.p50) / 1000.0,
                    static_cast<double>(tick.p99) / 1000.0, static_cast<double>(tick.max) / 1000.0);
        const double kernelNsPerSimSecond = tick.mean * static_cast<double>(kTicksPerSecond);
        const double stepsPerSecond = static_cast<double>(kRefObservers) * 1e9 / tick.mean;
        std::printf("  aggregate: %.3f ms kernel time per simulated second on one worker (%.3f%% of one core);"
                    " achieved %.0f Steps/s = %.3g candidate evaluations/s\n",
                    kernelNsPerSimSecond / 1e6, kernelNsPerSimSecond / 1e7, stepsPerSecond,
                    stepsPerSecond * static_cast<double>(kRefCandidates));

        // Per-observer cost: mean per-call ns of each observer, with its eligible count.
        std::vector<std::size_t> order(kRefObservers);
        for (std::size_t i = 0; i < kRefObservers; ++i) {
            order[i] = i;
        }
        const double perObserverSteps = static_cast<double>(ticks_) * options.runs;
        std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
            return observerSum_[a] < observerSum_[b];
        });
        const auto line = [&](const char* label, std::size_t index) {
            const std::size_t o = order[index];
            std::printf("    %-6s observer %3zu: %7.0f ns/Step, %5.1f eligible of %zu\n", label,
                        o + 1, observerSum_[o] / perObserverSteps, observerEligible_[o],
                        kRefCandidates);
        };
        std::printf("  per-observer mean cost (sorted over %zu observers):\n", kRefObservers);
        line("min", 0);
        line("p10", kRefObservers / 10);
        line("median", kRefObservers / 2);
        line("p90", kRefObservers * 9 / 10);
        line("max", kRefObservers - 1);
        double eligibleTotal = 0;
        for (const double e : observerEligible_) {
            eligibleTotal += e;
        }
        std::printf("  workload: %.1f%% of submitted candidates eligible; per pass %zu acquires,"
                    " %zu switches, %zu clears; fingerprint %016llx\n",
                    100.0 * eligibleTotal / static_cast<double>(kRefObservers * kRefCandidates),
                    acquires_, switches_, clears_, static_cast<unsigned long long>(checksum_));
        return checksum_;
    }

    const char* Name() const { return name_; }
    const std::vector<double>& RunTickMeans() const { return runTickMean_; }

private:
    void ResetMemories()
    {
        for (ObserverMemory& m : memories_) {
            interest::ResetMemory(m);
        }
    }

    const char* name_;
    bool events_;
    float beta_;
    bool traced_;
    bool sharedOrder_;
    std::size_t ticks_;
    std::vector<std::vector<CandidateSnapshot>> world_;
    std::vector<ObserverSnapshot> observers_;
    AttentionProfile profile_;
    std::vector<std::vector<std::size_t>> orders_;
    std::vector<std::vector<CandidateSnapshot>> frames_;  // per-observer gathered buffers
    std::vector<ObserverMemory> memories_;
    StepScratch scratch_;
    DecisionTrace trace_;
    Decision output_{};
    std::vector<std::int64_t> steps_;         // pooled per-Step samples
    std::vector<std::int64_t> ticksNs_;       // pooled per-tick totals (100 back-to-back Steps)
    std::vector<double> runTickMean_;         // per-run mean tick time, for paired ratios
    std::vector<double> observerSum_;         // per-observer summed per-call ns
    std::vector<double> observerEligible_;    // per-observer mean eligible candidates per step
    std::vector<std::int64_t> passSamples_;
    std::size_t switches_ = 0, clears_ = 0, acquires_ = 0;
    std::uint64_t checksum_ = 0;
    bool allOk_ = true;
    bool identical_ = true;
    bool allocationFree_ = true;
};

// --- Environment, timer and memory reports ---------------------------------------------

void PrintEnvironment()
{
#if defined(_MSC_FULL_VER)
    std::printf("compiler: MSVC %d\n", _MSC_FULL_VER);
#elif defined(__clang__)
    std::printf("compiler: Clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
#if defined(NDEBUG)
    std::printf("build: NDEBUG (optimized configuration)\n");
#else
    std::printf("build: assertions enabled (Debug configuration)\n");
#endif
    std::printf("hardware threads: %u; pointer size %zu\n", std::thread::hardware_concurrency(),
                sizeof(void*));
    std::printf("clock: steady_clock, period %lld/%lld s\n",
                static_cast<long long>(Clock::period::num), static_cast<long long>(Clock::period::den));
}

void MeasureTimer(bool smoke)
{
    const std::size_t pairs = smoke ? 1000 : 1000000;
    std::vector<std::int64_t> samples;
    samples.reserve(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        const Clock::time_point begin = Clock::now();
        const Clock::time_point end = Clock::now();
        samples.push_back(Nanoseconds(begin, end));
    }
    // Granularity: smallest positive step between successive distinct readings.
    std::int64_t granularity = INT64_MAX;
    for (int i = 0; i < 1000; ++i) {
        const Clock::time_point start = Clock::now();
        Clock::time_point next = Clock::now();
        while (next == start) {
            next = Clock::now();
        }
        granularity = std::min(granularity, Nanoseconds(start, next));
    }
    const Distribution d = Summarize(samples);
    std::printf("timer: granularity %lld ns; empty timed region (n=%zu): mean %.1f  p50 %lld  p99 %lld"
                "  max %lld ns\n", static_cast<long long>(granularity), d.count, d.mean,
                static_cast<long long>(d.p50), static_cast<long long>(d.p99),
                static_cast<long long>(d.max));
}

void PrintMemory()
{
    std::printf("\nObject sizes (bytes, sizeof/alignof):\n");
    std::printf("  ObserverMemory %zu/%zu  ObserverSnapshot %zu/%zu  CandidateSnapshot %zu/%zu\n",
                sizeof(ObserverMemory), alignof(ObserverMemory), sizeof(ObserverSnapshot),
                alignof(ObserverSnapshot), sizeof(CandidateSnapshot), alignof(CandidateSnapshot));
    std::printf("  CandidateTrace %zu/%zu  Decision %zu/%zu  StepScratch %zu  DecisionTrace %zu"
                "  AttentionProfile %zu  ContextPreference %zu\n",
                sizeof(interest::CandidateTrace), alignof(interest::CandidateTrace),
                sizeof(Decision), alignof(Decision), sizeof(StepScratch), sizeof(DecisionTrace),
                sizeof(AttentionProfile), sizeof(ContextPreference));
    std::printf("Reserved heap storage after Reserve(n) (capacity x element size, excluding"
                " allocator overhead):\n");
    for (const std::size_t n : {std::size_t{0}, std::size_t{16}, std::size_t{100}, std::size_t{500}}) {
        StepScratch scratch;
        DecisionTrace trace;
        const std::size_t before = allocationCount;
        interest::Reserve(scratch, n);
        interest::Reserve(trace, n);
        const std::size_t allocations = allocationCount - before;
        std::printf("  n=%3zu: scratch capacity %3zu = %5zu B; trace capacity %3zu = %6zu B;"
                    " %zu allocation(s) by Reserve\n", n, scratch.sortedIds.capacity(),
                    scratch.sortedIds.capacity() * sizeof(CandidateId), trace.candidates.capacity(),
                    trace.candidates.capacity() * sizeof(interest::CandidateTrace), allocations);
    }
    const std::size_t scratchBytes = kRefCandidates * sizeof(CandidateId);
    const std::size_t traceBytes = kRefCandidates * sizeof(interest::CandidateTrace);
    std::printf("Reference workload (%zu observers, %zu candidates, one worker):\n", kRefObservers,
                kRefCandidates);
    std::printf("  retained per observer: ObserverMemory %zu B -> %zu B for all observers\n",
                sizeof(ObserverMemory), kRefObservers * sizeof(ObserverMemory));
    std::printf("  per worker: scratch %zu B + optional trace %zu B (contents need not persist)\n",
                scratchBytes, traceBytes);
    std::printf("  if every observer owned buffers instead: scratch %zu B, trace %zu B\n",
                kRefObservers * scratchBytes, kRefObservers * traceBytes);
    std::printf("  host-owned input: one %zu-candidate snapshot array = %zu B\n", kRefCandidates,
                kRefCandidates * sizeof(CandidateSnapshot));
}

void PrintPeakProcessMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        std::printf("peak process working set: %.1f MiB (benchmark process, dominated by"
                    " pre-generated input frames; not kernel memory)\n",
                    static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0));
    }
#elif defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        std::printf("peak process RSS: %.1f MiB (benchmark process, dominated by pre-generated"
                    " input frames; not kernel memory)\n",
                    static_cast<double>(usage.ru_maxrss) / 1024.0);
    }
#endif
}

// A deliberately under-reserved Step is rejected without mutation or allocation.
void CheckUnderReserved(const std::vector<ContextPreference>& contexts)
{
    const Workload w{"mixed", Spatial::Mixed, 0, false, false};
    const auto frames = MakeFrames(w, true, 100, 1, 0x5EED0B01ull);
    const AttentionProfile profile = MakeProfile(contexts, 0.75f);
    StepScratch scratch;
    DecisionTrace trace;
    interest::Reserve(scratch, 99);
    interest::Reserve(trace, 100);
    ObserverMemory memory{};
    memory.current = 7;
    memory.hasStepped = true;
    Decision output{};
    output.selected = 3;
    const ObserverMemory memoryBefore = memory;
    const std::size_t before = allocationCount;
    const StepStatus status = interest::Step(test::Observer(), frames[0], profile, 0, memory,
                                             scratch, output, &trace);
    Require(status == StepStatus::InsufficientCapacity && allocationCount == before &&
                test::SameMemory(memory, memoryBefore) && output.selected == 3 &&
                trace.candidates.empty() && scratch.sortedIds.capacity() == 99,
            "under-reserved scratch is rejected without mutation or allocation");
    interest::Reserve(scratch, 100);
    const std::size_t reserved = allocationCount;
    const StepStatus retry = interest::Step(test::Observer(), frames[0], profile, 0, memory,
                                            scratch, output, &trace);
    Require(retry == StepStatus::Ok && allocationCount == reserved,
            "the same Step succeeds without allocation once capacity is reserved");
    std::printf("under-reserved check: capacity 99 for 100 candidates -> %s, memory/output/trace"
                " unchanged, 0 allocations; after Reserve(100) -> %s\n",
                interest::ToString(status), interest::ToString(retry));
}

double MedianBatch(const Config& c)
{
    std::vector<double> batch = c.batchMean;
    std::sort(batch.begin(), batch.end());
    return batch[batch.size() / 2];
}

// Attribution estimates measured outside Step. ValidateProfile is the public function
// that Step calls first on every call. The duplicate-detection primitive repeats Step's
// own library calls (copy the IDs, std::sort, one std::equal_range per candidate) on the
// same frames. Neither replaces the Step measurements; they only apportion them.
void MeasureIsolatedCosts(const AttentionProfile& profile, const std::vector<Config>& configs,
                          const Workload& mixed, const Workload& shuffle, const Variant& core,
                          const Options& options)
{
    std::size_t allocations = allocationCount;
    const std::size_t calls = options.smoke ? 1000 : 2000000;
    std::size_t valid = 0;
    const Clock::time_point begin = Clock::now();
    for (std::size_t i = 0; i < calls; ++i) {
        valid += interest::ValidateProfile(profile) == StepStatus::Ok;  // out-of-line, not hoisted
    }
    const Clock::time_point end = Clock::now();
    allocations = allocationCount - allocations;
    Require(valid == calls, "ValidateProfile accepts the benchmark profile");
    const double validateNs = static_cast<double>(Nanoseconds(begin, end)) / static_cast<double>(calls);

    std::vector<CandidateId> ids;
    ids.reserve(500);
    if (!options.smoke) {
        std::printf("\nIsolated attribution estimates (measured outside Step):\n");
    }
    for (const Config& c : configs) {
        // kDistributions holds a copy of the shuffle workload, so compare by name.
        if (c.variant != &core || (std::strcmp(c.workload->name, mixed.name) != 0 &&
                                   std::strcmp(c.workload->name, shuffle.name) != 0)) {
            continue;
        }
        if (c.n == 0) {
            if (!options.smoke) {
                std::printf("  ValidateProfile (2 contexts): %.1f ns/call = %.0f%% of %s n=0 batch"
                            " (%.1f ns)\n", validateNs, 100.0 * validateNs / MedianBatch(c),
                            c.label.c_str(), MedianBatch(c));
            }
            continue;
        }
        std::size_t duplicates = 0;
        const std::size_t before = allocationCount;
        const Clock::time_point start = Clock::now();
        for (int cycle = 0; cycle < options.cycles; ++cycle) {
            for (const std::vector<CandidateSnapshot>& frame : c.frames) {
                ids.clear();
                for (const CandidateSnapshot& candidate : frame) {
                    ids.push_back(candidate.id);
                }
                std::sort(ids.begin(), ids.end());
                for (const CandidateSnapshot& candidate : frame) {
                    const auto range = std::equal_range(ids.begin(), ids.end(), candidate.id);
                    duplicates += range.second - range.first > 1;
                }
            }
        }
        const Clock::time_point stop = Clock::now();
        allocations += allocationCount - before;
        Require(duplicates == 0, "benchmark mixed/shuffle frames have unique IDs");
        const double perStep = static_cast<double>(Nanoseconds(start, stop)) /
            static_cast<double>(c.frames.size() * static_cast<std::size_t>(options.cycles));
        if (!options.smoke) {
            std::printf("  duplicate detection (copy IDs + sort + equal_range), %-8s n=%3zu: %8.1f"
                        " ns/Step = %4.1f%% of its Step batch (%.1f ns)\n", c.workload->name, c.n,
                        perStep, 100.0 * perStep / MedianBatch(c), MedianBatch(c));
        }
    }
    Require(allocations == 0, "no allocations in isolated measurements");
}

// --- Cross-configuration pairing checks ------------------------------------------------
// A comparison between two configurations is attributable to the factor they are meant to
// differ in only if their inputs are otherwise identical. These checks assert that on the
// generated snapshots directly, independently of each configuration's own replay.

std::uint32_t Bits(float v) { return std::bit_cast<std::uint32_t>(v); }
std::uint64_t Bits(double v) { return std::bit_cast<std::uint64_t>(v); }

// Every snapshot field except the event pair, bitwise.
auto NonEventKey(const CandidateSnapshot& c)
{
    return std::make_tuple(c.id, static_cast<std::uint8_t>(c.spatial), Bits(c.position.x),
                           Bits(c.position.y), Bits(c.position.z), Bits(c.bearing.x),
                           Bits(c.bearing.y), Bits(c.bearing.z), Bits(c.attraction),
                           Bits(c.salience), Bits(c.perceptionStrength), Bits(c.maxDistance),
                           c.tags, c.enabled);
}

auto FullKey(const CandidateSnapshot& c)
{
    return std::tuple_cat(NonEventKey(c), std::make_tuple(c.eventTimeMs, Bits(c.eventStrength)));
}

bool HasEvent(const CandidateSnapshot& c)
{
    return c.eventTimeMs != 0 || Bits(c.eventStrength) != 0;
}

using Frames = std::vector<std::vector<CandidateSnapshot>>;

// Same snapshots at the same positions. With `eventsOnly`, `withoutEvents` must carry no
// event data and the two may differ only in the event pair.
bool SamePerIndex(const Frames& withoutEvents, const Frames& other, bool eventsOnly,
                  std::size_t& eventEntries)
{
    if (withoutEvents.size() != other.size()) {
        return false;
    }
    for (std::size_t f = 0; f < other.size(); ++f) {
        if (withoutEvents[f].size() != other[f].size()) {
            return false;
        }
        for (std::size_t i = 0; i < other[f].size(); ++i) {
            const CandidateSnapshot& a = withoutEvents[f][i];
            const CandidateSnapshot& b = other[f][i];
            eventEntries += HasEvent(b);
            const bool same = eventsOnly ? !HasEvent(a) && NonEventKey(a) == NonEventKey(b)
                                         : FullKey(a) == FullKey(b);
            if (!same) {
                return false;
            }
        }
    }
    return true;
}

// Same multiset of snapshots in every frame; counts frames whose order differs.
bool SameMultisetPerFrame(const Frames& a, const Frames& b, std::size_t& reordered)
{
    if (a.size() != b.size()) {
        return false;
    }
    using Key = decltype(FullKey(CandidateSnapshot{}));
    std::vector<Key> ka;
    std::vector<Key> kb;
    for (std::size_t f = 0; f < a.size(); ++f) {
        if (a[f].size() != b[f].size()) {
            return false;
        }
        ka.clear();
        kb.clear();
        bool sameOrder = true;
        for (std::size_t i = 0; i < a[f].size(); ++i) {
            ka.push_back(FullKey(a[f][i]));
            kb.push_back(FullKey(b[f][i]));
            sameOrder = sameOrder && ka.back() == kb.back();
        }
        reordered += !sameOrder;
        std::sort(ka.begin(), ka.end());
        std::sort(kb.begin(), kb.end());
        if (ka != kb) {
            return false;
        }
    }
    return true;
}

bool SameCharacterization(const Config& a, const Config& b)
{
    return a.entries == b.entries && a.eligible == b.eligible &&
           std::equal(std::begin(a.rejected), std::end(a.rejected), std::begin(b.rejected));
}

// Pairs every configuration with every other generated from the same workload data (same
// spatial kind, rejection share, replacement and n):
//   - same order and event setting: identical snapshots (the trace and beta variants);
//   - same order, events on vs off: identical except the event pair, absent when off;
//   - fixed vs reshuffled order, same event setting: identical snapshot multisets per frame.
// Paired variants must also have identical eligibility, and every pair whose decisions
// must agree (same beta, and same events unless beta is 0, where an event cannot change
// an effective score) must have an equal decision fingerprint.
void CheckPairedConfigs(const std::vector<Config>& configs)
{
    std::size_t identical = 0, eventOnly = 0, orderOnly = 0, decisions = 0;
    std::size_t eventEntries = 0, reorderedFrames = 0;
    const bool failedBefore = failed;
    for (std::size_t i = 0; i < configs.size(); ++i) {
        for (std::size_t j = i + 1; j < configs.size(); ++j) {
            const Config& a = configs[i];
            const Config& b = configs[j];
            const Workload& wa = *a.workload;
            const Workload& wb = *b.workload;
            if (a.n != b.n || wa.spatial != wb.spatial || wa.rejectPercent != wb.rejectPercent ||
                wa.replace != wb.replace) {
                continue;
            }
            const bool sameEvents = a.variant->events == b.variant->events;
            if (wa.reshuffle == wb.reshuffle) {
                const Config& off = a.variant->events ? b : a;
                const Config& on = a.variant->events ? a : b;
                Require(SamePerIndex(off.frames, on.frames, !sameEvents, eventEntries),
                        sameEvents ? "same-order variants submit identical snapshots"
                                   : "event on/off variants differ only in event fields");
                ++(sameEvents ? identical : eventOnly);
            } else if (sameEvents) {
                Require(SameMultisetPerFrame(a.frames, b.frames, reorderedFrames),
                        "fixed and reshuffled variants submit the same snapshots per frame");
                ++orderOnly;
            } else {
                continue;  // order and events both differ: not a designed comparison
            }
            Require(SameCharacterization(a, b), "paired variants have identical eligibility");
            if (a.variant->beta == b.variant->beta && (sameEvents || a.variant->beta == 0.f)) {
                Require(a.checksum == b.checksum, "paired variants make identical decisions");
                ++decisions;
            }
        }
    }
    Require(identical > 0 && eventOnly > 0 && orderOnly > 0 && decisions > 0,
            "every pairing category was checked");
    Require(eventEntries > 0, "event variants actually carry events");
    Require(reorderedFrames > 0, "reshuffled variants actually change submission order");
    std::printf("paired-input checks %s: %zu identical-snapshot pairs, %zu event-only pairs,"
                " %zu order-only pairs (%zu reordered frames), %zu equal-decision pairs\n",
                failed && !failedBefore ? "FAILED" : "passed", identical, eventOnly, orderOnly,
                reorderedFrames, decisions);
}

// The reference scene's core and novelty worlds differ only in the event pair.
void CheckReferencePairing(const Options& options)
{
    const std::size_t ticks = options.smoke ? 3 : kRefTicks;
    std::size_t eventEntries = 0;
    const bool same = SamePerIndex(MakeWorldFrames(false, ticks), MakeWorldFrames(true, ticks),
                                   true, eventEntries);
    Require(same && eventEntries > 0, "reference core/novelty worlds differ only in event fields");
    std::printf("reference pairing %s: core and novelty worlds identical except %zu event"
                " entries over %zu ticks\n", same && eventEntries > 0 ? "passed" : "FAILED",
                eventEntries, ticks);
}

const char* ShortReason(std::size_t e)
{
    switch (static_cast<Eligibility>(e)) {
    case Eligibility::InvalidId: return "id";
    case Eligibility::DuplicateId: return "dup";
    case Eligibility::InvalidValue: return "val";
    case Eligibility::InvalidGeometry: return "geo";
    case Eligibility::Disabled: return "dis";
    case Eligibility::NotPerceived: return "np";
    case Eligibility::MissingRequiredTag: return "req";
    case Eligibility::ExcludedTag: return "exc";
    case Eligibility::OutOfRange: return "oor";
    case Eligibility::InvalidEvent: return "evt";
    default: return "?";
    }
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            options.smoke = true;
        } else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            options.runs = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            options.cycles = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            options.rawPath = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--smoke] [--runs R] [--cycles C] [--raw file.csv]\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.smoke) {
        options.runs = 1;
        options.cycles = 2;
        options.frames = 4;
    }
    std::FILE* raw = nullptr;
    if (options.rawPath != nullptr) {
#if defined(_MSC_VER)
        if (fopen_s(&raw, options.rawPath, "w") != 0) { raw = nullptr; }
#else
        raw = std::fopen(options.rawPath, "w");
#endif
        if (raw == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", options.rawPath);
            return 2;
        }
        std::fprintf(raw, "section,config,run,index,ns\n");
    }

    std::printf("interest_step_benchmark%s: runs %d, cycles %d, frames/cycle %zu\n",
                options.smoke ? " (smoke: harness checks only, no timing claims)" : "",
                options.runs, options.cycles, options.frames);
    PrintEnvironment();
    MeasureTimer(options.smoke);

    const std::vector<ContextPreference> contexts = MakeContexts();
    const AttentionProfile profileOff = MakeProfile(contexts, 0.f);
    const AttentionProfile profileOn = MakeProfile(contexts, 0.75f);

    static const Workload kMixed{"mixed", Spatial::Mixed, 0, false, false};
    static const Workload kShuffle{"shuffle", Spatial::Mixed, 0, false, true};
    static const Workload kDistributions[] = {
        {"front", Spatial::Front, 0, false, false},
        {"surround", Spatial::Surround, 0, false, false},
        {"far80", Spatial::Far, 0, false, false},
        {"reject50", Spatial::Mixed, 50, false, false},
        kShuffle,
        {"churn", Spatial::Mixed, 0, true, true},
    };
    static const Variant kCore{"core", false, 0.f, false};
    static const Variant kCoreTrace{"core+trace", false, 0.f, true};
    static const Variant kEventsBeta0{"events,beta0", true, 0.f, false};
    static const Variant kNovelty{"novelty", true, 0.75f, false};
    static const Variant kNoveltyTrace{"novelty+trace", true, 0.75f, true};

    std::vector<Config> configs;
    const auto add = [&](const Workload& w, const Variant& v, std::size_t n) {
        Config c;
        c.label = std::string(w.name) + "/" + v.name;
        c.workload = &w;
        c.variant = &v;
        c.n = n;
        // Same seed for every variant of a workload and size: only the variant differs.
        const std::uint64_t seed = std::uint64_t{0xB0000000u} + std::uint64_t{n} * 131u +
                                   std::uint64_t{static_cast<std::uint8_t>(w.spatial)} * 7u +
                                   std::uint64_t{static_cast<unsigned>(w.rejectPercent)} +
                                   std::uint64_t{w.replace ? 3u : 0u};
        c.frames = MakeFrames(w, v.events, n, options.frames, seed);
        configs.push_back(std::move(c));
    };
    for (const std::size_t n : {std::size_t{0}, std::size_t{16}, std::size_t{100}, std::size_t{500}}) {
        for (const Variant* v : {&kCore, &kCoreTrace, &kEventsBeta0, &kNovelty, &kNoveltyTrace}) {
            add(kMixed, *v, n);
        }
    }
    for (const Workload& w : kDistributions) {
        for (const std::size_t n : {std::size_t{100}, std::size_t{500}}) {
            add(w, kCore, n);
            add(w, kCoreTrace, n);
        }
    }

    // Characterize every configuration (untimed, traced) and record its fingerprint.
    Workspace ws;
    interest::Reserve(ws.scratch, 500);
    interest::Reserve(ws.trace, 500);
    const ObserverSnapshot observer = test::Observer(0);
    std::size_t maxSamples = 0;
    for (Config& c : configs) {
        const AttentionProfile& profile = c.variant->beta > 0.f ? profileOn : profileOff;
        c.checksum = Cycle(c, observer, profile, ws, true);
        Require(Cycle(c, observer, profile, ws, false) == c.checksum, "trace-off cycle matches traced cycle");
        const std::size_t samplesPerRun = c.frames.size() * static_cast<std::size_t>(options.cycles);
        maxSamples = std::max(maxSamples, samplesPerRun);
        c.perCall.reserve(samplesPerRun * static_cast<std::size_t>(options.runs));
    }
    CheckPairedConfigs(configs);
    CheckReferencePairing(options);

    // Run-major order: every run measures every configuration once, so drift over the
    // session shows up as run-to-run spread rather than as a configuration difference.
    std::vector<std::int64_t> runSamples;
    runSamples.reserve(maxSamples);
    for (int run = 0; run < options.runs; ++run) {
        for (Config& c : configs) {
            const AttentionProfile& profile = c.variant->beta > 0.f ? profileOn : profileOff;
            MeasureRun(c, observer, profile, ws, options, runSamples);
            if (raw != nullptr) {
                for (std::size_t i = 0; i < runSamples.size(); ++i) {
                    std::fprintf(raw, "single,%s/n%zu,%d,%zu,%lld\n", c.label.c_str(), c.n, run, i,
                                 static_cast<long long>(runSamples[i]));
                }
            }
        }
    }

    if (!options.smoke) {
        std::printf("\nWorkloads (untimed traced characterization of one %zu-frame cycle):\n",
                    options.frames);
        std::printf("  %-22s %4s %7s %4s %4s %4s  rejected entries by reason\n", "config", "n",
                    "elig%", "acq", "sw", "clr");
        for (const Config& c : configs) {
            if (c.variant->trace) {
                continue;  // identical inputs and decisions to the untraced variant
            }
            std::printf("  %-22s %4zu %6.1f%% %4zu %4zu %4zu ", c.label.c_str(), c.n,
                        c.entries == 0 ? 0.0
                                       : 100.0 * static_cast<double>(c.eligible) /
                                             static_cast<double>(c.entries),
                        c.acquires, c.switches, c.clears);
            for (std::size_t e = 1; e < 16; ++e) {
                if (c.rejected[e] != 0) {
                    std::printf(" %s %.1f%%", ShortReason(e),
                                100.0 * static_cast<double>(c.rejected[e]) /
                                    static_cast<double>(c.entries));
                }
            }
            std::printf("\n");
        }

        std::printf("\nPer-Step latency, single observer (ns; per-call samples include one timer"
                    " pair; batch = back-to-back mean without per-call timers):\n");
        std::printf("  %-22s %4s %7s %6s %6s %6s %6s %7s %13s %9s %15s %9s %12s\n", "config", "n",
                    "samples", "mean", "p50", "p95", "p99", "p99.9", "max", "batch",
                    "batch run range", "p50 runs", "cand/s");
        double baseBatch[2] = {0, 0};  // n == 0 batch mean for core / novelty, per-candidate deltas
        for (const Config& c : configs) {
            const Distribution d = Summarize(c.perCall);
            std::vector<double> batch = c.batchMean;
            std::sort(batch.begin(), batch.end());
            const double batchMedian = batch[batch.size() / 2];
            const auto [p50Min, p50Max] = std::minmax_element(c.perCallRunP50.begin(), c.perCallRunP50.end());
            std::printf("  %-22s %4zu %7zu %6.0f %6lld %6lld %6lld %7lld %13lld %9.1f %7.1f-%-7.1f %4lld-%-4lld %12.3g\n",
                        c.label.c_str(), c.n, d.count, d.mean, static_cast<long long>(d.p50),
                        static_cast<long long>(d.p95), static_cast<long long>(d.p99),
                        static_cast<long long>(d.p999), static_cast<long long>(d.max), batchMedian,
                        batch.front(), batch.back(), static_cast<long long>(*p50Min),
                        static_cast<long long>(*p50Max),
                        static_cast<double>(c.n) * 1e9 / batchMedian);
            if (c.workload == &kMixed && c.n == 0 && c.variant == &kCore) {
                baseBatch[0] = batchMedian;
            }
            if (c.workload == &kMixed && c.n == 0 && c.variant == &kNovelty) {
                baseBatch[1] = batchMedian;
            }
        }
        std::printf("\nMarginal batch cost per submitted candidate, mixed workload ((batch(n) -"
                    " batch(0)) / n, ns):\n");
        for (const Config& c : configs) {
            if (c.workload != &kMixed || c.n == 0) {
                continue;
            }
            std::vector<double> batch = c.batchMean;
            std::sort(batch.begin(), batch.end());
            const double base = c.variant->events ? baseBatch[1] : baseBatch[0];
            std::printf("  %-22s n=%3zu: %6.2f\n", c.label.c_str(), c.n,
                        (batch[batch.size() / 2] - base) / static_cast<double>(c.n));
        }
    }

    if (!options.smoke) {
        // Paired per-run ratios of batch means: both configurations of a pair were measured
        // in the same run, on input data the pairing checks proved identical apart from the
        // compared factor.
        const auto find = [&](const char* workload, const Variant& v, std::size_t n) -> const Config* {
            for (const Config& c : configs) {
                if (std::strcmp(c.workload->name, workload) == 0 && c.variant == &v && c.n == n) {
                    return &c;
                }
            }
            return nullptr;
        };
        const auto ratio = [&](const char* label, const Config* a, const Config* b) {
            if (a == nullptr || b == nullptr) {
                return;
            }
            std::vector<double> r;
            for (std::size_t run = 0; run < a->batchMean.size(); ++run) {
                r.push_back(a->batchMean[run] / b->batchMean[run] - 1.0);
            }
            std::sort(r.begin(), r.end());
            std::printf("  %-34s n=%3zu: median %+6.1f%%  runs %+6.1f%% .. %+6.1f%%\n", label, a->n,
                        100.0 * r[r.size() / 2], 100.0 * r.front(), 100.0 * r.back());
        };
        std::printf("\nPaired per-run batch ratios (%d runs):\n", options.runs);
        for (const std::size_t n : {std::size_t{16}, std::size_t{100}, std::size_t{500}}) {
            ratio("trace: core+trace / core", find("mixed", kCoreTrace, n), find("mixed", kCore, n));
            ratio("events present, beta 0 / core", find("mixed", kEventsBeta0, n), find("mixed", kCore, n));
            ratio("beta .75 / beta 0 (same events)", find("mixed", kNovelty, n), find("mixed", kEventsBeta0, n));
            ratio("novelty / core", find("mixed", kNovelty, n), find("mixed", kCore, n));
            ratio("trace: novelty+trace / novelty", find("mixed", kNoveltyTrace, n), find("mixed", kNovelty, n));
            ratio("order only: shuffle / mixed", find("shuffle", kCore, n), find("mixed", kCore, n));
            ratio("replace+order: churn / mixed", find("churn", kCore, n), find("mixed", kCore, n));
        }
    }

    MeasureIsolatedCosts(profileOff, configs, kMixed, kShuffle, kCore, options);

    // Reference workload variants: only events/beta/trace/submission order differ.
    std::printf("\nReference workload: %zu observers x %zu candidates x 5 Hz = %zu Steps and %zu"
                " submitted candidate evaluations per simulated second (input counts, not a"
                " measurement). One sequential worker, synchronous ticks.\n", kRefObservers,
                kRefCandidates, kRefObservers * kTicksPerSecond,
                kRefObservers * kRefCandidates * kTicksPerSecond);
    std::vector<ReferenceVariant> refs;
    refs.reserve(4);
    refs.emplace_back("ref/core", false, 0.f, false, false, contexts, options);
    refs.emplace_back("ref/novelty", true, 0.75f, false, false, contexts, options);
    refs.emplace_back("ref/novelty+trace", true, 0.75f, true, false, contexts, options);
    refs.emplace_back("ref/core,shared-order", false, 0.f, false, true, contexts, options);
    for (int run = 0; run < options.runs; ++run) {
        for (ReferenceVariant& v : refs) {
            v.MeasureRun(run, raw);
        }
    }
    std::uint64_t refChecksums[4]{};
    for (std::size_t i = 0; i < refs.size(); ++i) {
        refChecksums[i] = refs[i].Report(options);
    }
    // Order- and trace-independent decisions over identical world data.
    Require(refChecksums[0] == refChecksums[3], "reference per-observer and shared order decide identically");
    Require(refChecksums[1] == refChecksums[2], "reference novelty trace on/off decide identically");
    if (!options.smoke) {
        // Paired per-run ratios of mean tick time: both variants measured in the same run.
        const auto ratios = [&](std::size_t a, std::size_t b) {
            std::vector<double> r;
            for (std::size_t run = 0; run < refs[a].RunTickMeans().size(); ++run) {
                r.push_back(refs[a].RunTickMeans()[run] / refs[b].RunTickMeans()[run] - 1.0);
            }
            std::sort(r.begin(), r.end());
            std::printf("  %-22s vs %-22s median %+6.1f%%  runs %+6.1f%% .. %+6.1f%%\n",
                        refs[a].Name(), refs[b].Name(), 100.0 * r[r.size() / 2],
                        100.0 * r.front(), 100.0 * r.back());
        };
        std::printf("\nReference paired per-run tick-time ratios (%d interleaved runs):\n", options.runs);
        ratios(1, 0);
        ratios(2, 1);
        ratios(0, 3);
    }

    PrintMemory();
    CheckUnderReserved(contexts);
    if (raw != nullptr) {
        std::fclose(raw);
    }
    PrintPeakProcessMemory();
    std::printf("harness checks: %s (statuses Ok, exact replay across warm-up/modes/runs, trace"
                " on/off identical decisions, 0 allocations in measured regions)\n",
                failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
