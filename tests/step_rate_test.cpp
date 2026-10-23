// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Compares temporal selection and minimum dwell at 5 Hz and 20 Hz.

#include "test_support.hpp"

#include <cstdio>
#include <vector>

namespace {

using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::DecisionKind;
using interest::DecisionReason;
using interest::StepStatus;
using interest::TimeMs;
using test::Harness;
using test::Observer;
using test::Scored;

using Candidates = std::vector<CandidateSnapshot>;

constexpr TimeMs kReference = 1;  // 1 kHz
constexpr TimeMs kRates[2]{50, 200};
const char* const kRateNames[2]{"20 Hz", "5 Hz"};

// One target change (Acquire, Switch or Clear).
struct Change {
    TimeMs time;
    CandidateId selected;
    DecisionReason reason;
};

struct Run {
    std::vector<Change> changes;
    TimeMs end = 0;
    int statelessChanges = 0;  // target changes of B1 on the same steps
    int overrides = 0;         // DwellOverride decisions
    bool consistent = true;
};

template <typename Scene>
Run RunScene(const Scene& scene, const AttentionProfile& profile, TimeMs interval,
             TimeMs duration)
{
    Run run;
    run.end = duration;
    Harness h;
    CandidateId stateless = 0;
    for (TimeMs now = 0; now < duration; now += interval) {
        const Candidates candidates = scene(now);
        if (h.Run(Observer(), candidates, profile, now) != StepStatus::Ok ||
            !test::ConsistentStep(h, profile)) {
            run.consistent = false;
        }
        const DecisionKind kind = h.output.kind;
        if (kind == DecisionKind::Acquire || kind == DecisionKind::Switch ||
            kind == DecisionKind::Clear) {
            run.changes.push_back({now, h.output.selected, h.output.reason});
        }
        run.overrides += h.output.reason == DecisionReason::DwellOverride ? 1 : 0;
        const CandidateId next = test::StatelessSelection(h.trace);
        run.statelessChanges += next != stateless ? 1 : 0;
        stateless = next;
    }
    return run;
}

// How long `id` was selected; a selection lasts until the next change.
TimeMs HeldMs(const Run& run, CandidateId id)
{
    TimeMs held = 0;
    for (std::size_t i = 0; i < run.changes.size(); ++i) {
        if (run.changes[i].selected == id) {
            const TimeMs until = i + 1 < run.changes.size() ? run.changes[i + 1].time : run.end;
            held += until - run.changes[i].time;
        }
    }
    return held;
}

int ChangesTo(const Run& run, CandidateId id)
{
    int count = 0;
    for (const Change& change : run.changes) {
        count += change.selected == id ? 1 : 0;
    }
    return count;
}

// Time from `from` until the first change to `id` before `to`; -1 if there is none.
TimeMs LatencyTo(const Run& run, CandidateId id, TimeMs from, TimeMs to)
{
    for (const Change& change : run.changes) {
        if (change.selected == id && change.time >= from && change.time < to) {
            return change.time - from;
        }
    }
    return -1;
}

double PerMinute(int count, TimeMs duration)
{
    return count * 60000.0 / static_cast<double>(duration);
}

// Deterministic per-cell noise in [-amplitude, amplitude], one value per 50 ms cell.
std::vector<float> CellNoise(std::uint64_t seed, TimeMs duration, float amplitude)
{
    test::Random rng(seed);
    std::vector<float> noise(static_cast<std::size_t>(duration / 50 + 1));
    for (float& value : noise) {
        value = amplitude * (2.f * rng.Unit() - 1.f);
    }
    return noise;
}

float At(const std::vector<float>& cells, TimeMs t)
{
    return cells[static_cast<std::size_t>(t / 50)];
}

// --- C12 -----------------------------------------------------------------------------

struct TimedScene {
    const char* name;
    TimeMs duration;
    Candidates (*scene)(TimeMs);
};

// C11 timing: B (+switchMargin) appears 200 ms into A's dwell and leaves at 3050 ms.
Candidates PersistentChallenger(TimeMs t)
{
    Candidates c;
    if (t >= 150) {
        c.push_back(Scored(1, 0.625f));
    }
    if (t >= 350 && t < 3050) {
        c.push_back(Scored(2, 0.75f));
    }
    return c;
}

// C11a timing: B (+overrideMargin) appears 200 ms into A's dwell.
Candidates StrongChallenger(TimeMs t)
{
    Candidates c;
    if (t >= 150) {
        c.push_back(Scored(1, 0.625f));
    }
    if (t >= 350) {
        c.push_back(Scored(2, 0.875f));
    }
    return c;
}

// C10 timing: A leaves 300 ms into its dwell; B qualifies as the fallback.
Candidates InvalidDuringDwell(TimeMs t)
{
    Candidates c;
    if (t >= 150 && t < 450) {
        c.push_back(Scored(1, 0.75f));
    }
    if (t >= 150) {
        c.push_back(Scored(2, 0.5625f));
    }
    return c;
}

// Release deferred by dwell, then reacquisition: A falls below release 200 ms after it is
// acquired, and recovers at 4050 ms.
Candidates DeferredRelease(TimeMs t)
{
    if (t < 150) {
        return {};
    }
    return {Scored(1, t < 350 || t >= 4050 ? 0.625f : 0.25f)};
}

// C09 timing: A alternates 0.5625/0.4375 every 150 ms from 150 ms, then falls to 0.25.
Candidates ThresholdOscillation(TimeMs t)
{
    if (t < 150) {
        return {};
    }
    if (t >= 3000) {
        return {Scored(1, 0.25f)};
    }
    return {Scored(1, ((t - 150) / 150) % 2 == 0 ? 0.5625f : 0.4375f)};
}

// The same scenes give the same sequence of target changes and reasons at 5 Hz, 20 Hz
// and 1 kHz; each change is late by less than two intervals of its rate.
void C12_SameScenesAt5And20Hz()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = test::TemporalProfile(contexts);
    const TimedScene scenes[]{
        {"C11 persistent challenger in dwell", 5000, PersistentChallenger},
        {"C11a strong challenger in dwell", 3000, StrongChallenger},
        {"C10 target leaves during dwell", 3000, InvalidDuringDwell},
        {"release deferred by dwell", 6000, DeferredRelease},
        {"C09 threshold oscillation", 4000, ThresholdOscillation},
    };
    std::printf("  %-36s %-34s %s\n", "scene", "1 kHz changes (ms, id, reason)",
                "delay at 20 Hz / 5 Hz (ms)");
    bool sameSequence = true;
    bool withinBound = true;
    bool consistent = true;
    for (const TimedScene& s : scenes) {
        const Run reference = RunScene(s.scene, profile, kReference, s.duration);
        Run runs[2];
        for (std::size_t r = 0; r < 2; ++r) {
            runs[r] = RunScene(s.scene, profile, kRates[r], s.duration);
            consistent = consistent && runs[r].consistent && reference.consistent;
            sameSequence = sameSequence && runs[r].changes.size() == reference.changes.size();
        }
        for (std::size_t i = 0; i < reference.changes.size(); ++i) {
            const Change& ideal = reference.changes[i];
            std::printf("  %-36s %5lld %2llu %-22s", i == 0 ? s.name : "",
                        static_cast<long long>(ideal.time),
                        static_cast<unsigned long long>(ideal.selected),
                        interest::ToString(ideal.reason));
            for (std::size_t r = 0; r < 2; ++r) {
                if (i >= runs[r].changes.size()) {
                    continue;
                }
                const Change& observed = runs[r].changes[i];
                const TimeMs delay = observed.time - ideal.time;
                sameSequence = sameSequence && observed.selected == ideal.selected &&
                               observed.reason == ideal.reason;
                withinBound = withinBound && delay >= 0 && delay < 2 * kRates[r];
                std::printf(" %5lld", static_cast<long long>(delay));
            }
            std::printf("\n");
        }
    }
    CHECK(consistent);
    CHECK(sameSequence);
    CHECK(withinBound);
}

// C08 noise sampled at both rates. Each source is 0.70 +- 0.04 per 50 ms cell, so the 5 Hz
// run sees every fourth cell. B1 flicker grows with the update rate; B2 does not switch.
void C12_NoiseAt5And20Hz()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = test::TemporalProfile(contexts);
    constexpr TimeMs kDuration = 60000;
    const std::vector<float> a = CellNoise(0x5EED0C12u, kDuration, 0.04f);
    const std::vector<float> b = CellNoise(0x5EED0C13u, kDuration, 0.04f);
    const auto scene = [&a, &b](TimeMs t) {
        return Candidates{Scored(1, 0.7f + At(a, t)), Scored(2, 0.7f + At(b, t))};
    };
    for (std::size_t r = 0; r < 2; ++r) {
        const Run run = RunScene(scene, profile, kRates[r], kDuration);
        const auto changes = static_cast<int>(run.changes.size());
        std::printf("  C08 noise at %-6s B1 %6.1f changes/min, B2 %4.1f\n", kRateNames[r],
                    PerMinute(run.statelessChanges, kDuration), PerMinute(changes, kDuration));
        CHECK(run.consistent);
        CHECK(changes == 1);  // the first acquisition only
        CHECK(run.statelessChanges > (r == 0 ? 400 : 100));
    }
}

// --- Minimum dwell: what does it add beyond the margin and the release band? ----------

struct Setting {
    const char* name;
    TimeMs dwell;
    float switchMargin;
    float overrideMargin;
};
// Dwell from 0 to 1000 ms with the baseline margins, and the alternative a host might use
// instead of dwell: a switch margin as wide as the override margin, with no dwell.
constexpr Setting kSettings[]{
    {"dwell 0", 0, 0.125f, 0.25f},       {"dwell 250", 250, 0.125f, 0.25f},
    {"dwell 500", 500, 0.125f, 0.25f},   {"dwell 1000", 1000, 0.125f, 0.25f},
    {"margin 0.25, dwell 0", 0, 0.25f, 0.25f},
};
constexpr std::size_t kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);
constexpr std::size_t kWide = kSettingCount - 1;

AttentionProfile With(const Setting& setting, std::span<const ContextPreference> contexts)
{
    AttentionProfile profile = test::TemporalProfile(contexts);
    profile.minDwellMs = setting.dwell;
    profile.switchMargin = setting.switchMargin;
    profile.overrideMargin = setting.overrideMargin;
    return profile;
}

// (a) Transient above-margin spikes on a settled target. A (0.625) has been held for 2 s.
// B (0.5) jumps to 0.8, which leads A by more than switchMargin but less than
// overrideMargin, for 100, 200, 400 and 800 ms, starting at 2, 5, 8 and 11 s.
constexpr TimeMs kSpikeStart[4]{2000, 5000, 8000, 11000};
constexpr TimeMs kSpikeLength[4]{100, 200, 400, 800};

Candidates SettledSpikes(TimeMs t)
{
    float b = 0.5f;
    for (std::size_t i = 0; i < 4; ++i) {
        b = t >= kSpikeStart[i] && t < kSpikeStart[i] + kSpikeLength[i] ? 0.8f : b;
    }
    return {Scored(1, 0.625f), Scored(2, b)};
}

// (b) The same spike, 200 ms long, 100 ms after A is (re)acquired. Each 3 s cycle starts
// with A absent for 200 ms; B rests at 0.45, below acquire.
Candidates FreshSpikes(TimeMs t)
{
    const TimeMs phase = t % 3000;
    Candidates c;
    if (phase >= 200) {
        c.push_back(Scored(1, 0.625f));
    }
    c.push_back(Scored(2, phase >= 300 && phase < 500 ? 0.8f : 0.45f));
    return c;
}

// (c) A sustained challenger B (0.8) arrives 100, 300, 600 or 1200 ms after A is acquired
// and stays until the 4 s cycle ends; each cycle starts with both absent for 200 ms.
constexpr TimeMs kOnsetAfterAcquire[4]{100, 300, 600, 1200};

Candidates SustainedChallengers(TimeMs t)
{
    const TimeMs phase = t % 4000;
    const auto cycle = static_cast<std::size_t>(t / 4000);
    Candidates c;
    if (phase >= 200) {
        c.push_back(Scored(1, 0.625f));
        if (phase >= 200 + kOnsetAfterAcquire[cycle]) {
            c.push_back(Scored(2, 0.8f));
        }
    }
    return c;
}

void DwellAgainstTransientAndSustainedChallengers()
{
    const ContextPreference contexts[1]{};
    std::printf("  %-22s %-6s | %-17s | %-17s | %-27s\n", "setting", "rate",
                "(a) settled spikes", "(b) fresh spikes", "(c) sustained: latency ms");
    std::printf("  %-22s %-6s | %8s %8s | %8s %8s | %6s %6s %6s %6s\n", "", "", "to B",
                "B held", "to B", "B held", "+100", "+300", "+600", "+1200");
    TimeMs settledHeldNoDwell[2]{};
    bool consistent = true;
    for (std::size_t s = 0; s < kSettingCount; ++s) {
        const AttentionProfile profile = With(kSettings[s], contexts);
        for (std::size_t r = 0; r < 2; ++r) {
            const TimeMs interval = kRates[r];
            const Run settled = RunScene(SettledSpikes, profile, interval, 14000);
            const Run fresh = RunScene(FreshSpikes, profile, interval, 12000);
            const Run sustained = RunScene(SustainedChallengers, profile, interval, 16000);
            consistent = consistent && settled.consistent && fresh.consistent &&
                         sustained.consistent;
            std::printf("  %-22s %-6s | %8d %8lld | %8d %8lld |", kSettings[s].name,
                        kRateNames[r], ChangesTo(settled, 2),
                        static_cast<long long>(HeldMs(settled, 2)), ChangesTo(fresh, 2),
                        static_cast<long long>(HeldMs(fresh, 2)));
            for (std::size_t k = 0; k < 4; ++k) {
                const TimeMs onset = static_cast<TimeMs>(k) * 4000 + 200 + kOnsetAfterAcquire[k];
                const TimeMs latency =
                    LatencyTo(sustained, 2, onset, static_cast<TimeMs>(k + 1) * 4000);
                std::printf(" %6lld", static_cast<long long>(latency));

                // Dwell delays a sustained challenger to the end of dwell, measured from the
                // acquisition; quantization adds less than two intervals. The wide margin
                // never lets a +0.175 challenger in.
                const TimeMs dwellLeft = kSettings[s].dwell > kOnsetAfterAcquire[k]
                                             ? kSettings[s].dwell - kOnsetAfterAcquire[k]
                                             : 0;
                if (s == kWide) {
                    CHECK(latency == -1);
                } else {
                    CHECK(latency >= 0 && latency < dwellLeft + 2 * interval);
                    CHECK(latency + interval > dwellLeft);
                }
            }
            std::printf("\n");

            if (s == kWide) {
                CHECK(ChangesTo(settled, 2) == 0 && ChangesTo(fresh, 2) == 0);
                continue;
            }
            // A settled target has no dwell left, so dwell never prevents the switch to a
            // spike; it only keeps B selected until B's own dwell ends.
            CHECK(ChangesTo(settled, 2) == 4);
            if (kSettings[s].dwell == 0) {
                settledHeldNoDwell[r] = HeldMs(settled, 2);
            } else {
                CHECK(HeldMs(settled, 2) > settledHeldNoDwell[r]);
            }
            // Right after an acquisition (cycle phase 200), dwell absorbs a spike (phase 300
            // to 500) unless a step falls inside the spike once dwell has ended.
            const TimeMs from = kSettings[s].dwell + 200 > 300 ? kSettings[s].dwell + 200 : 300;
            const TimeMs firstStep = (from + interval - 1) / interval * interval;
            CHECK(ChangesTo(fresh, 2) == (firstStep < 500 ? 4 : 0));
        }
    }
    CHECK(consistent);
}

// (d) Score noise larger than the switch margin: A and B are both 0.6 +- 0.15 per 50 ms
// cell, so their difference reaches +-0.3. In the second scene B's mean is 0.15 higher;
// "on B" is the share of time spent on that better source despite the noise.
void DwellAgainstNoiseLargerThanMargin()
{
    const ContextPreference contexts[1]{};
    constexpr TimeMs kDuration = 60000;
    const std::vector<float> a = CellNoise(0x5EED0D01u, kDuration, 0.15f);
    const std::vector<float> b = CellNoise(0x5EED0D02u, kDuration, 0.15f);
    const auto equal = [&a, &b](TimeMs t) {
        return Candidates{Scored(1, 0.6f + At(a, t)), Scored(2, 0.6f + At(b, t))};
    };
    const auto betterB = [&a, &b](TimeMs t) {
        return Candidates{Scored(1, 0.6f + At(a, t)), Scored(2, 0.75f + At(b, t))};
    };
    std::printf("  %-22s %-6s | %14s %10s | %14s %10s\n", "setting", "rate",
                "equal: chg/min", "overrides", "B+0.15: chg/min", "on B %");
    double changesPerMinute[kSettingCount][2]{};
    bool consistent = true;
    for (std::size_t s = 0; s < kSettingCount; ++s) {
        const AttentionProfile profile = With(kSettings[s], contexts);
        for (std::size_t r = 0; r < 2; ++r) {
            const Run same = RunScene(equal, profile, kRates[r], kDuration);
            const Run better = RunScene(betterB, profile, kRates[r], kDuration);
            consistent = consistent && same.consistent && better.consistent;
            changesPerMinute[s][r] =
                PerMinute(static_cast<int>(same.changes.size()), kDuration);
            const double onB = 100.0 * static_cast<double>(HeldMs(better, 2)) / kDuration;
            std::printf("  %-22s %-6s | %14.1f %10d | %14.1f %10.1f\n", kSettings[s].name,
                        kRateNames[r], changesPerMinute[s][r], same.overrides,
                        PerMinute(static_cast<int>(better.changes.size()), kDuration), onB);
            CHECK(onB > 80.0);  // every setting mostly stays on the better source
        }
    }
    CHECK(consistent);
    for (std::size_t r = 0; r < 2; ++r) {
        // Each longer dwell switches less often on equal noise.
        CHECK(changesPerMinute[1][r] < changesPerMinute[0][r]);
        CHECK(changesPerMinute[2][r] < changesPerMinute[1][r]);
        CHECK(changesPerMinute[3][r] < changesPerMinute[2][r]);
    }
}

} // namespace

int main()
{
    test::Run("C12 same scenes at 5 Hz and 20 Hz", C12_SameScenesAt5And20Hz);
    test::Run("C12 noise at 5 Hz and 20 Hz", C12_NoiseAt5And20Hz);
    test::Run("dwell against transient and sustained challengers",
              DwellAgainstTransientAndSustainedChallengers);
    test::Run("dwell against noise larger than the margin", DwellAgainstNoiseLargerThanMargin);
    return test::Finish("interest_rate_test");
}
