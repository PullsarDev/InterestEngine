// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Fifty similar observers share a stimulus stream; host profiles supply explicit variation.

#include "test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
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
using test::Scored;

constexpr int kObservers = 50;
constexpr TimeMs kInterval = 50;  // 20 Hz
constexpr TimeMs kDuration = 12000;

// A (id 1) rises from 0.40 to a 0.52 plateau at 4-6 s, then falls to 0.30 by 10 s. B (id 2)
// appears at 5 s at 0.45 and rises to 0.70 by 9 s. Values change once per 50 ms cell.
std::vector<CandidateSnapshot> Stimulus(TimeMs t)
{
    const TimeMs cell = t - t % 50;
    const auto ramp = [cell](TimeMs from, TimeMs to, float low, float high) {
        return low + (high - low) * static_cast<float>(cell - from) /
                         static_cast<float>(to - from);
    };
    const float a = cell < 4000   ? ramp(0, 4000, 0.40f, 0.52f)
                    : cell < 6000 ? 0.52f
                    : cell < 10000 ? ramp(6000, 10000, 0.52f, 0.30f)
                                   : 0.30f;
    std::vector<CandidateSnapshot> candidates{Scored(1, a)};
    if (cell >= 5000) {
        candidates.push_back(Scored(2, cell < 9000 ? ramp(5000, 9000, 0.45f, 0.70f) : 0.70f));
    }
    return candidates;
}

// What the host would derive once per character from its stored variation seed, and keep
// in that character's profile. Base values are test::TemporalProfile's.
AttentionProfile VariedProfile(std::span<const ContextPreference> contexts, std::uint64_t seed)
{
    test::Random rng(seed);
    AttentionProfile profile = test::TemporalProfile(contexts);
    profile.acquireThreshold = 0.47f + 0.06f * rng.Unit();
    profile.releaseThreshold = profile.acquireThreshold - 0.05f - 0.05f * rng.Unit();
    profile.switchMargin = 0.1f + 0.05f * rng.Unit();
    profile.minDwellMs = 300 + static_cast<TimeMs>(400.f * rng.Unit());
    return profile;
}

struct Change {
    TimeMs time;
    CandidateId selected;
    DecisionReason reason;
    bool operator==(const Change&) const = default;
};
using Timeline = std::vector<Change>;

struct Member {
    interest::ObserverId id;
    AttentionProfile profile;
    TimeMs phase;  // the host's evaluation offset within the update interval
};

struct Population {
    std::vector<Timeline> timelines;
    bool consistent = true;
};

Population RunPopulation(const std::vector<Member>& observers)
{
    Population result;
    for (const Member& o : observers) {
        Harness h;
        Timeline timeline;
        interest::ObserverSnapshot snapshot = test::Observer();
        snapshot.id = o.id;
        for (TimeMs now = o.phase; now < kDuration; now += kInterval) {
            const std::vector<CandidateSnapshot> candidates = Stimulus(now);
            if (h.Run(snapshot, candidates, o.profile, now) != StepStatus::Ok ||
                !test::ConsistentStep(h, o.profile)) {
                result.consistent = false;
            }
            if (h.output.kind != DecisionKind::Keep && h.output.kind != DecisionKind::None) {
                timeline.push_back({now, h.output.selected, h.output.reason});
            }
        }
        result.timelines.push_back(timeline);
    }
    return result;
}

struct Summary {
    int distinctChoices = 0;     // distinct sequences of selected IDs
    int distinctTimelines = 0;   // distinct sequences of (time, id, reason)
    int largestSimultaneous = 0; // most observers changing target at the same time
    TimeMs firstAcquireSpread = 0;
    int neverAcquiredA = 0;
};

Summary Summarize(const Population& population)
{
    std::vector<std::vector<CandidateId>> choices;
    std::vector<Timeline> timelines;
    std::map<TimeMs, int> changesAt;
    TimeMs earliest = kDuration;
    TimeMs latest = 0;
    Summary summary;
    for (const Timeline& timeline : population.timelines) {
        std::vector<CandidateId> sequence;
        bool acquiredA = false;
        for (const Change& change : timeline) {
            sequence.push_back(change.selected);
            ++changesAt[change.time];
            acquiredA = acquiredA || change.selected == 1;
        }
        summary.neverAcquiredA += acquiredA ? 0 : 1;
        if (!timeline.empty()) {
            earliest = std::min(earliest, timeline.front().time);
            latest = std::max(latest, timeline.front().time);
        }
        if (std::find(choices.begin(), choices.end(), sequence) == choices.end()) {
            choices.push_back(sequence);
        }
        if (std::find(timelines.begin(), timelines.end(), timeline) == timelines.end()) {
            timelines.push_back(timeline);
        }
    }
    for (const auto& entry : changesAt) {
        summary.largestSimultaneous = std::max(summary.largestSimultaneous, entry.second);
    }
    summary.distinctChoices = static_cast<int>(choices.size());
    summary.distinctTimelines = static_cast<int>(timelines.size());
    summary.firstAcquireSpread = latest >= earliest ? latest - earliest : 0;
    return summary;
}

void Print(const char* condition, const Summary& s)
{
    std::printf("  %-34s %8d %10d %13d %12lld %10d\n", condition, s.distinctChoices,
                s.distinctTimelines, s.largestSimultaneous,
                static_cast<long long>(s.firstAcquireSpread), s.neverAcquiredA);
}

void FiftyObservers()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile shared = test::TemporalProfile(contexts);
    std::vector<Member> identical;
    std::vector<Member> staggered;
    std::vector<Member> varied;
    std::vector<Member> variedStaggered;
    std::vector<Member> renumbered;
    for (int k = 0; k < kObservers; ++k) {
        const auto id = static_cast<interest::ObserverId>(k + 1);
        const auto phase = static_cast<TimeMs>(k) % kInterval;  // 0..49 ms
        const AttentionProfile profile =
            VariedProfile(contexts, 0xC11B0000u + static_cast<std::uint64_t>(k));
        identical.push_back({id, shared, 0});
        staggered.push_back({id, shared, phase});
        varied.push_back({id, profile, 0});
        variedStaggered.push_back({id, profile, phase});
        renumbered.push_back({static_cast<interest::ObserverId>(9000 - k), profile, 0});
    }

    const Population lockstep = RunPopulation(identical);
    const Population phased = RunPopulation(staggered);
    const Population diverse = RunPopulation(varied);
    const Population both = RunPopulation(variedStaggered);
    const Population again = RunPopulation(varied);
    const Population renamed = RunPopulation(renumbered);
    CHECK(lockstep.consistent && phased.consistent && diverse.consistent && both.consistent);

    std::printf("  %-34s %8s %10s %13s %12s %10s\n", "condition (50 observers)", "choices",
                "timelines", "simultaneous", "spread ms", "never A");
    const Summary a = Summarize(lockstep);
    const Summary b = Summarize(phased);
    const Summary c = Summarize(diverse);
    const Summary d = Summarize(both);
    Print("identical profiles, synchronous", a);
    Print("identical profiles, staggered", b);
    Print("varied profiles, synchronous", c);
    Print("varied profiles, staggered", d);

    // Lockstep: one timeline shared by all 50.
    CHECK(a.distinctTimelines == 1 && a.largestSimultaneous == kObservers);
    CHECK(!lockstep.timelines[0].empty());

    // Staggering desynchronizes timing only: each observer makes the synchronous choices,
    // shifted by exactly its phase, because it sees the same 50 ms cells.
    CHECK(b.distinctChoices == 1 && b.largestSimultaneous == 1);
    bool shifted = true;
    const Timeline& reference = lockstep.timelines[0];
    for (std::size_t k = 0; k < staggered.size(); ++k) {
        const Timeline& timeline = phased.timelines[k];
        shifted = shifted && timeline.size() == reference.size();
        for (std::size_t i = 0; shifted && i < reference.size(); ++i) {
            shifted = timeline[i].time == reference[i].time + staggered[k].phase &&
                      timeline[i].selected == reference[i].selected &&
                      timeline[i].reason == reference[i].reason;
        }
    }
    CHECK(shifted);
    CHECK(b.firstAcquireSpread == kInterval - 1);

    // Parameter variation changes choices as well as timing near the thresholds. A's 0.52
    // plateau is below some observers' acquire thresholds (0.47-0.53), so they first attend
    // B, later; and A rises by only 0.03 per second, so first acquisitions spread over more
    // than a second.
    CHECK(c.distinctChoices > 1 && c.neverAcquiredA > 0 && c.neverAcquiredA < kObservers);
    CHECK(c.largestSimultaneous < kObservers / 2);
    CHECK(c.firstAcquireSpread > 1000);

    // The variation is deterministic and tied to the seeds, not to observer IDs.
    CHECK(again.timelines == diverse.timelines);
    CHECK(renamed.timelines == diverse.timelines);
}

} // namespace

int main()
{
    test::Run("C11b fifty observers", FiftyObservers);
    return test::Finish("interest_observers_test");
}
