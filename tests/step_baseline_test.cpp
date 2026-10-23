// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Compares B1 (stateless acquisition of the leading score) with B2 (the kernel's
// contextual scoring and temporal policy) on identical candidate streams. Novelty
// is disabled. Each step runs Step() once; B1 is read from that call's trace so both
// baselines see exactly the same scores.
//
// Ablations rerun each stream with one temporal mechanism changed in the profile:
//   no band      releaseThreshold = acquireThreshold
//   no dwell     minDwellMs = 0
//   override~off overrideMargin = 1. This only approximates disabling override:
//                a score of 1 can still override a target at 0 during dwell. Each
//                run asserts no DwellOverride occurred, making it exact here.
//   min margin   switchMargin = 0.001 (a margin cannot be zero)
// Every step is checked against the policy rules through test::ConsistentStep.
// These synthetic streams measure mechanism effects, not natural attention quality.

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

constexpr TimeMs kInterval = 100;   // all streams run at 10 Hz
constexpr CandidateId kEvent = 50;  // a clearly superior challenger, where a stream has one
constexpr int kReasons = 10;

TimeMs At(int step)
{
    return static_cast<TimeMs>(step) * kInterval;
}

struct Frame {
    TimeMs now = 0;
    std::vector<CandidateSnapshot> candidates;
};
using Stream = std::vector<Frame>;

// Target changes of one baseline over a stream, and its response to kEvent.
class Tally {
public:
    int acquires = 0;
    int switches = 0;
    int clears = 0;
    int events = 0;    // appearances of kEvent
    int captured = 0;  // appearances followed by selecting kEvent while it is present
    TimeMs latencySum = 0;
    TimeMs latencyMax = 0;

    void Record(TimeMs now, CandidateId next, bool eventPresent)
    {
        if (selected_ == 0 && next != 0) {
            ++acquires;
        } else if (selected_ != 0 && next == 0) {
            ++clears;
        } else if (selected_ != next) {
            ++switches;
        }
        if (eventPresent && !eventWasPresent_) {
            ++events;
            pending_ = true;
            onset_ = now;
        }
        if (!eventPresent) {
            pending_ = false;  // missed
        }
        if (pending_ && next == kEvent) {
            ++captured;
            latencySum += now - onset_;
            latencyMax = now - onset_ > latencyMax ? now - onset_ : latencyMax;
            pending_ = false;
        }
        eventWasPresent_ = eventPresent;
        selected_ = next;
    }

    bool SameAs(const Tally& other) const
    {
        return acquires == other.acquires && switches == other.switches &&
               clears == other.clears && events == other.events &&
               captured == other.captured && latencySum == other.latencySum &&
               latencyMax == other.latencyMax;
    }

private:
    CandidateId selected_ = 0;
    bool eventWasPresent_ = false;
    bool pending_ = false;
    TimeMs onset_ = 0;
};

struct Result {
    Tally b2;
    Tally b1;
    int differing = 0;  // steps where B2 and B1 select differently
    int reasonCounts[kReasons]{};
    bool consistent = true;
};

Result RunStream(const Stream& stream, const AttentionProfile& profile)
{
    Result result;
    Harness h;
    for (const Frame& frame : stream) {
        if (h.Run(Observer(), frame.candidates, profile, frame.now) != StepStatus::Ok ||
            !test::ConsistentStep(h, profile)) {
            result.consistent = false;
        }
        bool eventPresent = false;
        for (const CandidateSnapshot& candidate : frame.candidates) {
            eventPresent = eventPresent || candidate.id == kEvent;
        }
        const CandidateId stateless = test::StatelessSelection(h.trace);
        result.b2.Record(frame.now, h.output.selected, eventPresent);
        result.b1.Record(frame.now, stateless, eventPresent);
        result.differing += h.output.selected != stateless ? 1 : 0;
        const auto reason = static_cast<int>(h.output.reason);
        if (reason < kReasons) {
            ++result.reasonCounts[reason];
        }
    }
    return result;
}

enum Variant { kB2, kNoBand, kNoDwell, kNoOverride, kMinMargin, kVariantCount };
const char* const kVariantNames[kVariantCount]{"B2", "B2 no band", "B2 no dwell",
                                               "B2 override~off", "B2 min margin"};

AttentionProfile Ablate(AttentionProfile profile, int variant)
{
    switch (variant) {
    case kNoBand: profile.releaseThreshold = profile.acquireThreshold; break;
    case kNoDwell: profile.minDwellMs = 0; break;
    case kNoOverride: profile.overrideMargin = 1.f; break;
    case kMinMargin: profile.switchMargin = 0.001f; break;
    default: break;
    }
    return profile;
}

struct Comparison {
    Tally b1;
    Result runs[kVariantCount];
};

void PrintRow(const char* name, const Tally& tally, double minutes, int differing)
{
    const int changes = tally.acquires + tally.switches + tally.clears;
    std::printf("    %-15s %5d %5d %5d %8.1f %6d %6d %8.1f %6lld %8d\n", name, tally.acquires,
                tally.switches, tally.clears, changes / minutes, tally.events, tally.captured,
                tally.captured > 0 ? static_cast<double>(tally.latencySum) / tally.captured : 0.0,
                static_cast<long long>(tally.latencyMax), differing);
}

// Runs B2 and its ablations on one stream, checks every step, and prints the table.
Comparison Compare(const char* title, const Stream& stream, const AttentionProfile& profile)
{
    Comparison comparison;
    bool consistent = true;
    bool sameB1 = true;
    for (int variant = 0; variant < kVariantCount; ++variant) {
        comparison.runs[variant] = RunStream(stream, Ablate(profile, variant));
        consistent = consistent && comparison.runs[variant].consistent;
        if (variant == kNoOverride) {
            CHECK(comparison.runs[variant]
                      .reasonCounts[static_cast<int>(DecisionReason::DwellOverride)] == 0);
        }
        // B1 depends only on scores and acquireThreshold, which no ablation changes.
        sameB1 = sameB1 && comparison.runs[variant].b1.SameAs(comparison.runs[kB2].b1);
    }
    comparison.b1 = comparison.runs[kB2].b1;
    CHECK(consistent);
    CHECK(sameB1);

    const double minutes =
        static_cast<double>(stream.back().now - stream.front().now + kInterval) / 60000.0;
    std::printf("  %s (%zu steps)\n", title, stream.size());
    std::printf("    %-15s %5s %5s %5s %8s %6s %6s %8s %6s %8s\n", "baseline", "acq", "sw", "clr",
                "chg/min", "events", "taken", "mean ms", "max ms", "!= B1");
    PrintRow("B1 stateless", comparison.b1, minutes, 0);
    for (int variant = 0; variant < kVariantCount; ++variant) {
        PrintRow(kVariantNames[variant], comparison.runs[variant].b2, minutes,
                 comparison.runs[variant].differing);
    }
    return comparison;
}

const Tally& B2(const Comparison& c, int variant)
{
    return c.runs[variant].b2;
}

// C08: two sources trade the lead between 0.71 and 0.69 on every step, for 60 s.
void AlternatingPair()
{
    const ContextPreference contexts[1]{};
    Stream stream;
    for (int step = 0; step < 600; ++step) {
        const bool firstLeads = step % 2 == 0;
        stream.push_back({At(step), {Scored(1, firstLeads ? 0.71f : 0.69f),
                                     Scored(2, firstLeads ? 0.69f : 0.71f)}});
    }
    const Comparison c = Compare("C08 pair alternating 0.71/0.69", stream,
                                 test::TemporalProfile(contexts));
    CHECK(c.b1.switches == 599);
    for (int variant : {kB2, kNoBand, kNoDwell, kNoOverride}) {
        CHECK(B2(c, variant).acquires == 1 && B2(c, variant).switches == 0);
    }
    // Dwell alone only paces the flicker: one switch each time a 500 ms dwell ends.
    CHECK(B2(c, kMinMargin).switches == 119);
}

// C08 with random noise: three sources at 0.70 +- 0.04, for 60 s. No lead reaches the
// 0.125 margin.
void NoisyTrio()
{
    const ContextPreference contexts[1]{};
    test::Random rng(0x5EED0B01u);
    Stream stream;
    for (int step = 0; step < 600; ++step) {
        Frame frame{At(step), {}};
        for (CandidateId id = 1; id <= 3; ++id) {
            frame.candidates.push_back(Scored(id, 0.66f + 0.08f * rng.Unit()));
        }
        stream.push_back(frame);
    }
    const Comparison c =
        Compare("C08 three sources at 0.70 +- 0.04", stream, test::TemporalProfile(contexts));
    CHECK(c.b1.switches > 300);
    for (int variant : {kB2, kNoBand, kNoDwell, kNoOverride}) {
        CHECK(B2(c, variant).acquires == 1 && B2(c, variant).switches == 0);
    }
    // Without the margin, dwell allows at most one switch per 500 ms: 120 in 60 s.
    CHECK(B2(c, kMinMargin).switches > 0 && B2(c, kMinMargin).switches <= 120);
}

// C09: one source at 0.5 +- 0.1, around acquire 0.5 and above release 0.375, for 60 s.
void SourceAtThreshold()
{
    const ContextPreference contexts[1]{};
    test::Random rng(0x5EED0B02u);
    Stream stream;
    for (int step = 0; step < 600; ++step) {
        stream.push_back({At(step), {Scored(1, 0.4f + 0.2f * rng.Unit())}});
    }
    const Comparison c =
        Compare("C09 one source at 0.5 +- 0.1", stream, test::TemporalProfile(contexts));
    CHECK(c.b1.clears > 100);
    for (int variant : {kB2, kNoDwell, kNoOverride, kMinMargin}) {
        CHECK(B2(c, variant).acquires == 1 && B2(c, variant).clears == 0);
    }
    // Without the band, dwell paces the flicker but does not stop it: each clear needs a
    // 500 ms dwell and a step to reacquire, so at most 100 in 60 s.
    CHECK(B2(c, kNoBand).clears > 10 && B2(c, kNoBand).clears <= 100);
}

// C11/C11a: in each 3 s cycle a target (0.625) is acquired, then a challenger appears
// 100-600 ms later, while dwell may still run.
Stream ChallengerCycles(float challengerScore)
{
    Stream stream;
    int step = 0;
    for (int delay = 1; delay <= 6; ++delay) {
        stream.push_back({At(step++), {}});  // clears the previous cycle
        stream.push_back({At(step++), {Scored(1, 0.625f)}});
        for (int i = 1; i < 29; ++i) {
            Frame frame{At(step++), {Scored(1, 0.625f)}};
            if (i >= delay) {
                frame.candidates.push_back(Scored(kEvent, challengerScore));
            }
            stream.push_back(frame);
        }
    }
    return stream;
}

void PersistentChallenger()
{
    const ContextPreference contexts[1]{};
    const Comparison c = Compare("C11 challenger leading by switchMargin (0.75 vs 0.625)",
                                 ChallengerCycles(0.75f), test::TemporalProfile(contexts));
    CHECK(c.b1.captured == 6 && c.b1.latencyMax == 0);
    // Dwell delays the switch to its end: latencies 400, 300, 200, 100, 0 and 0 ms.
    for (int variant : {kB2, kNoBand, kNoOverride, kMinMargin}) {
        CHECK(B2(c, variant).captured == 6);
        CHECK(B2(c, variant).latencyMax == 400 && B2(c, variant).latencySum == 1000);
    }
    CHECK(B2(c, kNoDwell).captured == 6 && B2(c, kNoDwell).latencyMax == 0);
}

void StrongChallenger()
{
    const ContextPreference contexts[1]{};
    const Comparison c = Compare("C11a challenger leading by overrideMargin (0.875 vs 0.625)",
                                 ChallengerCycles(0.875f), test::TemporalProfile(contexts));
    for (int variant : {kB2, kNoBand, kNoDwell, kMinMargin}) {
        CHECK(B2(c, variant).captured == 6 && B2(c, variant).latencyMax == 0);
    }
    // Without the override it waits for dwell like the weaker challenger.
    CHECK(B2(c, kNoOverride).captured == 6);
    CHECK(B2(c, kNoOverride).latencyMax == 400 && B2(c, kNoOverride).latencySum == 1000);
}

// Four sources whose interest drifts slowly in [0.2, 0.8], with +-0.05 noise per step and
// each missing from 2% of steps; once in every 30 s block, a sudden event (0.95) for 1 s.
// Ten minutes at 10 Hz.
Stream MixedStream()
{
    test::Random rng(0x5EED0B03u);
    float base[4]{0.45f, 0.55f, 0.62f, 0.7f};
    int eventStart = 0;
    Stream stream;
    for (int step = 0; step < 6000; ++step) {
        Frame frame{At(step), {}};
        for (std::size_t i = 0; i < 4; ++i) {
            float drifted = base[i] + 0.02f * (rng.Unit() - 0.5f);
            drifted = drifted < 0.2f ? 0.4f - drifted : (drifted > 0.8f ? 1.6f - drifted : drifted);
            base[i] = drifted;
            const float noise = 0.1f * (rng.Unit() - 0.5f);
            if (rng.Below(50) != 0) {
                frame.candidates.push_back(Scored(static_cast<CandidateId>(i + 1), drifted + noise));
            }
        }
        if (step % 300 == 0) {
            eventStart = step + static_cast<int>(rng.Below(290));
        }
        if (step >= eventStart && step < eventStart + 10) {
            frame.candidates.push_back(Scored(kEvent, 0.95f));
        }
        stream.push_back(frame);
    }
    return stream;
}

void MixedStreamComparison()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = test::TemporalProfile(contexts);
    const Comparison c = Compare("generated: drifting sources, dropouts and sudden events",
                                 MixedStream(), profile);
    std::printf("    B2 reasons:");
    for (int reason = 0; reason < kReasons; ++reason) {
        std::printf(" %s %d", interest::ToString(static_cast<DecisionReason>(reason)),
                    c.runs[kB2].reasonCounts[reason]);
    }
    std::printf("\n");

    // Recorded outcomes for this stream; the exact counts are printed above.
    const Tally& stable = B2(c, kB2);
    const int statelessChanges = c.b1.acquires + c.b1.switches + c.b1.clears;
    const int stableChanges = stable.acquires + stable.switches + stable.clears;
    CHECK(stableChanges * 3 < statelessChanges);
    CHECK(stable.clears * 3 < c.b1.clears);
    CHECK(c.b1.captured == c.b1.events && c.b1.latencyMax == 0);
    // The event (0.95) leads any target below 0.825 by the switch margin, so it is taken
    // at once or when dwell ends. A target above 0.825 would hold it off; none did here.
    CHECK(stable.events == 20 && stable.captured == 20);
    CHECK(stable.latencyMax <= profile.minDwellMs);
}

// The policy rules hold on every step for random valid profiles over the generated stream.
void RulesHoldForRandomProfiles()
{
    const ContextPreference contexts[1]{};
    const Stream stream = MixedStream();
    test::Random rng(0x5EED0B04u);
    bool valid = true;
    bool consistent = true;
    for (int i = 0; i < 25; ++i) {
        AttentionProfile profile = test::TemporalProfile(contexts);
        profile.acquireThreshold = 0.3f + 0.6f * rng.Unit();
        profile.releaseThreshold = profile.acquireThreshold * (0.5f + 0.5f * rng.Unit());
        profile.switchMargin = 0.001f + 0.3f * rng.Unit();
        profile.overrideMargin = profile.switchMargin + (1.f - profile.switchMargin) * rng.Unit();
        profile.minDwellMs = static_cast<TimeMs>(rng.Below(2000));
        valid = valid && interest::ValidateProfile(profile) == StepStatus::Ok;
        consistent = consistent && RunStream(stream, profile).consistent;
    }
    CHECK(valid);
    CHECK(consistent);
}

} // namespace

int main()
{
    test::Run("C08 alternating pair", AlternatingPair);
    test::Run("C08 noisy trio", NoisyTrio);
    test::Run("C09 source at the acquire threshold", SourceAtThreshold);
    test::Run("C11 persistent challenger", PersistentChallenger);
    test::Run("C11a strong challenger", StrongChallenger);
    test::Run("generated mixed stream", MixedStreamComparison);
    test::Run("policy rules hold for random profiles", RulesHoldForRandomProfiles);
    return test::Finish("interest_baseline_test");
}
