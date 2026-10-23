// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Temporal-policy scenarios: hysteresis, dwell, switching, override and invalid targets.
// Coverage includes dwell boundaries, invalidation during dwell, exact ties,
// explicit time and deterministic replay over generated multi-step streams.
// Most cases use test::TemporalProfile (acquire 0.5, release 0.375, switch margin
// 0.125, override margin 0.25, dwell 500 ms) with test::Scored candidates, whose raw
// score equals the supplied value. These dyadic values make threshold and margin
// boundary comparisons exact.

#include "test_support.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace {

using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::DecisionKind;
using interest::DecisionReason;
using interest::ObserverMemory;
using interest::StepStatus;
using interest::TagMask;
using interest::TimeMs;
using test::Harness;
using test::Observer;
using test::Scored;
using test::StatelessSelection;
using test::TemporalProfile;

TimeMs At(int step)
{
    return static_cast<TimeMs>(step) * 100;  // 10 Hz
}

// C08: two sources trade the lead between 0.71 and 0.69 on every step, at
// 10 Hz for 10 s. Stateless selection flips on every step; the policy never switches.
void C08_NearEqualScoreNoise()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    Harness h;
    int switches = 0;
    int statelessSwitches = 0;
    CandidateId statelessPrevious = 0;
    for (int step = 0; step < 100; ++step) {
        const bool firstLeads = step % 2 == 0;
        const CandidateSnapshot candidates[]{Scored(1, firstLeads ? 0.71f : 0.69f),
                                             Scored(2, firstLeads ? 0.69f : 0.71f)};
        CHECK(h.Run(Observer(), candidates, profile, At(step)) == StepStatus::Ok);
        if (step == 0) {
            CHECK_DECISION(h, Acquire, HighestScore, 1);
        } else if (firstLeads) {
            CHECK_DECISION(h, Keep, HighestScore, 1);
        } else {
            // Also during dwell: dwell is not the reason, the lead is below switchMargin.
            CHECK_DECISION(h, Keep, InsufficientAdvantage, 1);
        }
        switches += h.output.kind == DecisionKind::Switch ? 1 : 0;
        const CandidateId stateless = StatelessSelection(h.trace);
        statelessSwitches += step > 0 && stateless != statelessPrevious ? 1 : 0;
        statelessPrevious = stateless;
    }
    CHECK(switches == 0);
    CHECK(statelessSwitches == 99);
}

// C09: one source alternates between 0.5625 and 0.4375, around acquire 0.5 and never
// below release 0.375.
void C09_AcquireReleaseThresholdOscillation()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const auto scoreAt = [](int step) { return step % 2 == 1 ? 0.5625f : 0.4375f; };

    Harness h;
    int statelessChanges = 0;
    CandidateId statelessPrevious = 0;
    for (int step = 0; step <= 20; ++step) {
        const CandidateSnapshot candidates[]{Scored(1, scoreAt(step))};
        CHECK(h.Run(Observer(), candidates, profile, At(step)) == StepStatus::Ok);
        if (step == 0) {
            // The band only holds a current target; acquisition still needs 0.5.
            CHECK_DECISION(h, None, BelowAcquireThreshold, 0);
        } else if (step == 1) {
            CHECK_DECISION(h, Acquire, HighestScore, 1);
        } else if (step % 2 == 1) {
            CHECK_DECISION(h, Keep, HighestScore, 1);
        } else {
            CHECK_DECISION(h, Keep, AboveReleaseThreshold, 1);  // inside dwell as well
        }
        const CandidateId stateless = StatelessSelection(h.trace);
        statelessChanges += stateless != statelessPrevious ? 1 : 0;
        statelessPrevious = stateless;
    }
    CHECK(statelessChanges == 20);  // stateless selection acquires or clears on every step

    // Falling below release clears it, since dwell has long passed.
    const CandidateSnapshot low[]{Scored(1, 0.25f)};
    CHECK(h.Run(Observer(), low, profile, At(21)) == StepStatus::Ok);
    CHECK_DECISION(h, Clear, BelowReleaseThreshold, 0);
    CHECK(h.memory.current == 0 && h.memory.focusStartTime == 0);

    // Ablation: without the band (release == acquire), dwell only slows the flicker. The
    // source is acquired at 100, 700, 1300 and 1900 ms and released when each dwell ends.
    AttentionProfile noBand = profile;
    noBand.releaseThreshold = noBand.acquireThreshold;
    Harness ablated;
    int clears = 0;
    int dwellHolds = 0;
    for (int step = 0; step <= 20; ++step) {
        const CandidateSnapshot candidates[]{Scored(1, scoreAt(step))};
        CHECK(ablated.Run(Observer(), candidates, noBand, At(step)) == StepStatus::Ok);
        clears += ablated.output.kind == DecisionKind::Clear ? 1 : 0;
        dwellHolds += ablated.output.reason == DecisionReason::MinimumDwell ? 1 : 0;
    }
    CHECK(clears == 3);
    CHECK(dwellHolds == 7);
}

// C10: the current target becomes invalid while dwell still runs. Every kind of
// invalidation drops it on that same step, replacing it when a candidate qualifies.
void C10_CurrentTargetInvalidDuringDwell()
{
    constexpr unsigned kExcludedBit = 3;
    ContextPreference neutral{};
    ContextPreference excluding{};
    excluding.excludedTags = TagMask{1} << kExcludedBit;
    const ContextPreference contexts[2]{neutral, excluding};
    const AttentionProfile profile = TemporalProfile(contexts);

    // A Point straight ahead within fullRange: its gate is 1, so it scores its attraction.
    CandidateSnapshot target = test::PointAt(1, 5, 0, 0.75f, 0.f);
    target.tags = TagMask{1} << kExcludedBit;
    const auto edited = [&target](auto edit) {
        CandidateSnapshot copy = target;
        edit(copy);
        return copy;
    };
    using Edit = CandidateSnapshot&;
    struct Variant {
        const char* name;
        std::vector<CandidateSnapshot> candidates;  // the target's entries at 1100 ms
        interest::ContextId context;
    };
    const std::vector<Variant> variants{
        {"absent", {}, 0},
        {"disabled", {edited([](Edit c) { c.enabled = false; })}, 0},
        {"not perceived", {edited([](Edit c) { c.perceptionStrength = 0.f; })}, 0},
        {"invalid value",
         {edited([](Edit c) { c.attraction = std::numeric_limits<float>::quiet_NaN(); })}, 0},
        {"invalid geometry",
         {edited([](Edit c) { c.position.x = std::numeric_limits<double>::infinity(); })}, 0},
        {"out of range", {edited([](Edit c) { c.position.x = 25.0; })}, 0},
        {"duplicated id", {target, target}, 0},
        {"excluded by the new context", {target}, 1},
    };
    const CandidateSnapshot qualifying = Scored(2, 0.625f);
    const CandidateSnapshot weak = Scored(2, 0.4375f);  // eligible, below acquire

    for (const Variant& variant : variants) {
        for (const bool withFallback : {true, false}) {
            const int failuresBefore = test::State().failures;
            const CandidateSnapshot& other = withFallback ? qualifying : weak;
            Harness h;
            const CandidateSnapshot start[]{target, other};
            CHECK(h.Run(Observer(), start, profile, 1000) == StepStatus::Ok);
            CHECK_DECISION(h, Acquire, HighestScore, 1);

            std::vector<CandidateSnapshot> next = variant.candidates;
            next.push_back(other);
            CHECK(h.Run(Observer(variant.context), next, profile, 1100) == StepStatus::Ok);
            if (withFallback) {
                CHECK_DECISION(h, Switch, CurrentInvalid, 2);
                CHECK(h.output.rawScore == 0.625f && h.memory.focusStartTime == 1100);
            } else {
                CHECK_DECISION(h, Clear, CurrentInvalid, 0);
                CHECK(h.memory.current == 0 && h.memory.focusStartTime == 0);
            }
            // The trace shows that dwell was still running and did not protect it.
            CHECK(!h.trace.currentEligible && h.trace.currentScore == 0.f);
            CHECK(h.trace.dwellRemainingMs == 400);
            if (test::State().failures != failuresBefore) {
                std::fprintf(stderr, "  variant: %s, %s fallback\n", variant.name,
                             withFallback ? "with" : "without");
            }
        }
    }

    // Contrast: a still-valid target below release is held until dwell ends. Its
    // challenger leads by more than overrideMargin but is below acquireThreshold, so it
    // cannot override (C11a) or replace it afterwards.
    Harness valid;
    const CandidateSnapshot first[]{Scored(1, 0.75f), Scored(2, 0.4375f)};
    CHECK(valid.Run(Observer(), first, profile, 1000) == StepStatus::Ok);
    const CandidateSnapshot faded[]{Scored(1, 0.125f), Scored(2, 0.4375f)};
    CHECK(valid.Run(Observer(), faded, profile, 1100) == StepStatus::Ok);
    CHECK_DECISION(valid, Keep, MinimumDwell, 1);
    CHECK(valid.trace.currentEligible && valid.trace.currentScore == 0.125f);
    CHECK(valid.trace.dwellRemainingMs == 400);
    CHECK(valid.Run(Observer(), faded, profile, 1499) == StepStatus::Ok);
    CHECK_DECISION(valid, Keep, MinimumDwell, 1);
    CHECK(valid.Run(Observer(), faded, profile, 1500) == StepStatus::Ok);
    CHECK_DECISION(valid, Clear, BelowReleaseThreshold, 0);
}

// C11: a challenger that persists above switchMargin replaces the target once dwell
// ends; one below the margin, or below acquireThreshold, never does.
void C11_PersistentChallenger()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const CandidateSnapshot alone[]{Scored(1, 0.625f)};

    Harness h;
    const auto pair = [&h, &profile](float first, float second, TimeMs now) {
        const CandidateSnapshot candidates[]{Scored(1, first), Scored(2, second)};
        return h.Run(Observer(), candidates, profile, now);
    };
    CHECK(h.Run(Observer(), alone, profile, 0) == StepStatus::Ok);
    CHECK_DECISION(h, Acquire, HighestScore, 1);

    // Leads by 0.0625: not enough, with or without dwell. B1 would already switch.
    CHECK(pair(0.625f, 0.6875f, 100) == StepStatus::Ok);
    CHECK_DECISION(h, Keep, InsufficientAdvantage, 1);
    CHECK(StatelessSelection(h.trace) == 2);
    CHECK(h.trace.challenger == 2 && h.trace.challengerScore == 0.6875f);
    CHECK(h.trace.currentEligible && h.trace.currentScore == 0.625f);
    CHECK(h.trace.dwellRemainingMs == 400);

    // Leads by exactly switchMargin from 200 ms: dwell defers the switch to 500 ms.
    for (const TimeMs now : {TimeMs{200}, TimeMs{300}, TimeMs{499}}) {
        CHECK(pair(0.625f, 0.75f, now) == StepStatus::Ok);
        CHECK_DECISION(h, Keep, MinimumDwell, 1);
        CHECK(h.trace.dwellRemainingMs == 500 - now);
    }
    CHECK(pair(0.625f, 0.75f, 500) == StepStatus::Ok);
    CHECK_DECISION(h, Switch, BetterCandidate, 2);
    CHECK(h.output.previous == 1 && h.output.rawScore == 0.75f);
    CHECK(h.trace.dwellRemainingMs == 0 && h.memory.focusStartTime == 500);

    // The former target now needs the same margin to come back.
    CHECK(pair(0.8125f, 0.75f, 1100) == StepStatus::Ok);
    CHECK_DECISION(h, Keep, InsufficientAdvantage, 2);
    CHECK(pair(0.875f, 0.75f, 1200) == StepStatus::Ok);
    CHECK_DECISION(h, Switch, BetterCandidate, 1);

    // One float step below the margin, the challenger never takes over.
    Harness below;
    CHECK(below.Run(Observer(), alone, profile, 0) == StepStatus::Ok);
    const CandidateSnapshot almost[]{Scored(1, 0.625f), Scored(2, std::nextafter(0.75f, 0.f))};
    bool held = true;
    for (int step = 1; step <= 30; ++step) {
        held = held && below.Run(Observer(), almost, profile, At(step)) == StepStatus::Ok &&
               below.output.kind == DecisionKind::Keep &&
               below.output.reason == DecisionReason::InsufficientAdvantage;
    }
    CHECK(held);

    // A challenger leading by the margin but below acquireThreshold cannot replace a
    // target held in the release band, where B1 would select nothing.
    AttentionProfile smallMargin = profile;
    smallMargin.switchMargin = 0.0625f;
    Harness band;
    const CandidateSnapshot atAcquire[]{Scored(1, 0.5f)};
    CHECK(band.Run(Observer(), atAcquire, smallMargin, 0) == StepStatus::Ok);
    CHECK_DECISION(band, Acquire, HighestScore, 1);
    const CandidateSnapshot inBand[]{Scored(1, 0.375f), Scored(2, 0.46875f)};
    CHECK(band.Run(Observer(), inBand, smallMargin, 1000) == StepStatus::Ok);
    CHECK_DECISION(band, Keep, InsufficientAdvantage, 1);
    CHECK(StatelessSelection(band.trace) == 0);
}

// C11a: a challenger leading by overrideMargin and reaching acquireThreshold replaces the
// target within one step, even during dwell.
void C11a_StrongChallengerDuringDwell()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const CandidateSnapshot alone[]{Scored(1, 0.625f)};
    const CandidateSnapshot strong[]{Scored(1, 0.625f), Scored(2, 0.875f)};  // leads by 0.25

    Harness h;
    CHECK(h.Run(Observer(), alone, profile, 0) == StepStatus::Ok);
    CHECK(h.Run(Observer(), strong, profile, 100) == StepStatus::Ok);
    CHECK_DECISION(h, Switch, DwellOverride, 2);
    CHECK(h.trace.dwellRemainingMs == 400 && h.memory.focusStartTime == 100);

    // The new target has its own dwell: a lead of only switchMargin waits for it.
    const CandidateSnapshot third[]{Scored(1, 0.625f), Scored(2, 0.875f), Scored(3, 1.f)};
    CHECK(h.Run(Observer(), third, profile, 200) == StepStatus::Ok);
    CHECK_DECISION(h, Keep, MinimumDwell, 2);
    CHECK(h.trace.dwellRemainingMs == 400);
    CHECK(h.Run(Observer(), third, profile, 600) == StepStatus::Ok);
    CHECK_DECISION(h, Switch, BetterCandidate, 3);

    // One float step below overrideMargin, the switch waits for dwell to end.
    Harness below;
    CHECK(below.Run(Observer(), alone, profile, 0) == StepStatus::Ok);
    const CandidateSnapshot almost[]{Scored(1, 0.625f), Scored(2, std::nextafter(0.875f, 0.f))};
    CHECK(below.Run(Observer(), almost, profile, 100) == StepStatus::Ok);
    CHECK_DECISION(below, Keep, MinimumDwell, 1);
    CHECK(below.Run(Observer(), almost, profile, 500) == StepStatus::Ok);
    CHECK_DECISION(below, Switch, BetterCandidate, 2);

    // Ablation: overrideMargin = 1 cannot be met against a target scoring 0.625, so the
    // same strong challenger waits 400 ms for dwell to end instead of switching at once.
    AttentionProfile noOverride = profile;
    noOverride.overrideMargin = 1.f;
    Harness ablated;
    CHECK(ablated.Run(Observer(), alone, noOverride, 0) == StepStatus::Ok);
    bool waited = true;
    for (int step = 1; step <= 4; ++step) {
        waited = waited && ablated.Run(Observer(), strong, noOverride, At(step)) == StepStatus::Ok &&
                 ablated.output.reason == DecisionReason::MinimumDwell;
    }
    CHECK(waited);
    CHECK(ablated.Run(Observer(), strong, noOverride, At(5)) == StepStatus::Ok);
    CHECK_DECISION(ablated, Switch, BetterCandidate, 2);

    // overrideMargin = 1 only approximates "no override": a challenger at 1 still
    // overrides a valid target that has dropped to 0 during dwell.
    Harness edge;
    CHECK(edge.Run(Observer(), alone, noOverride, 0) == StepStatus::Ok);
    const CandidateSnapshot zeroAndOne[]{Scored(1, 0.f), Scored(2, 1.f)};
    CHECK(edge.Run(Observer(), zeroAndOne, noOverride, 100) == StepStatus::Ok);
    CHECK_DECISION(edge, Switch, DwellOverride, 2);
}

// An exact tie never replaces the current target, whatever the IDs and order: B1
// prefers the lower ID, the policy keeps its target.
void ExactTiesKeepTheCurrentTarget()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    Harness h;
    const CandidateSnapshot first[]{Scored(5, 0.625f)};
    CHECK(h.Run(Observer(), first, profile, 0) == StepStatus::Ok);
    const CandidateSnapshot tied[]{Scored(3, 0.625f), Scored(5, 0.625f)};
    const CandidateSnapshot reversed[]{Scored(5, 0.625f), Scored(3, 0.625f)};
    bool kept = true;
    for (int step = 1; step <= 50; ++step) {
        const std::span<const CandidateSnapshot> candidates =
            step % 2 == 0 ? std::span<const CandidateSnapshot>(tied)
                          : std::span<const CandidateSnapshot>(reversed);
        kept = kept && h.Run(Observer(), candidates, profile, At(step)) == StepStatus::Ok &&
               h.output.kind == DecisionKind::Keep && h.output.selected == 5 &&
               h.output.reason == DecisionReason::InsufficientAdvantage &&
               StatelessSelection(h.trace) == 3;
    }
    CHECK(kept);

    // The lead is compared as a difference, so even the smallest normal float margin
    // separates a tie from a one-ulp lead. (0.625f + margin would round back to 0.625f.)
    AttentionProfile tiny = profile;
    tiny.switchMargin = std::numeric_limits<float>::min();
    tiny.overrideMargin = tiny.switchMargin;
    tiny.minDwellMs = 0;
    CHECK(0.625f + tiny.switchMargin == 0.625f);
    Harness exact;
    CHECK(exact.Run(Observer(), first, tiny, 0) == StepStatus::Ok);
    CHECK(exact.Run(Observer(), tied, tiny, 100) == StepStatus::Ok);
    CHECK_DECISION(exact, Keep, InsufficientAdvantage, 5);
    const CandidateSnapshot ulpAhead[]{Scored(3, std::nextafter(0.625f, 1.f)), Scored(5, 0.625f)};
    CHECK(exact.Run(Observer(), ulpAhead, tiny, 200) == StepStatus::Ok);
    CHECK_DECISION(exact, Switch, BetterCandidate, 3);
}

// Dwell uses only host time, checked at each step: repeated timestamps do not advance
// it, late steps act late, and backward time changes nothing.
void ExplicitTimeSemantics()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const CandidateSnapshot alone[]{Scored(1, 0.625f)};
    const CandidateSnapshot challenged[]{Scored(1, 0.625f), Scored(2, 0.75f)};

    Harness h;
    CHECK(h.Run(Observer(), alone, profile, 100) == StepStatus::Ok);
    CHECK(h.Run(Observer(), challenged, profile, 100) == StepStatus::Ok);
    const ObserverMemory afterFirst = h.memory;
    CHECK(afterFirst.focusStartTime == 100 && afterFirst.lastStepTime == 100);
    bool frozen = true;
    for (int repeat = 0; repeat < 10; ++repeat) {
        frozen = frozen && h.Run(Observer(), challenged, profile, 100) == StepStatus::Ok &&
                 h.output.reason == DecisionReason::MinimumDwell &&
                 h.trace.dwellRemainingMs == 500 && test::SameMemory(h.memory, afterFirst);
    }
    CHECK(frozen);

    CHECK(h.Run(Observer(), challenged, profile, 350) == StepStatus::Ok);
    CHECK(h.trace.dwellRemainingMs == 250);
    CHECK(h.Run(Observer(), challenged, profile, 599) == StepStatus::Ok);
    CHECK_DECISION(h, Keep, MinimumDwell, 1);
    CHECK(h.trace.dwellRemainingMs == 1);
    // The next step comes late; the switch happens then. Resolution is the step interval.
    CHECK(h.Run(Observer(), challenged, profile, 950) == StepStatus::Ok);
    CHECK_DECISION(h, Switch, BetterCandidate, 2);
    CHECK(h.memory.focusStartTime == 950);

    const ObserverMemory before = h.memory;
    CHECK(h.Run(Observer(), alone, profile, 949) == StepStatus::TimeWentBackwards);
    CHECK(test::SameMemory(h.memory, before));

    interest::ResetMemory(h.memory);
    CHECK(h.memory.current == 0 && h.memory.focusStartTime == 0 && !h.memory.hasStepped);

    // With no dwell the same challenger switches on its first step, even at the same time.
    AttentionProfile noDwell = profile;
    noDwell.minDwellMs = 0;
    Harness immediate;
    CHECK(immediate.Run(Observer(), alone, noDwell, 100) == StepStatus::Ok);
    CHECK(immediate.Run(Observer(), challenged, noDwell, 100) == StepStatus::Ok);
    CHECK_DECISION(immediate, Switch, BetterCandidate, 2);
    CHECK(immediate.trace.dwellRemainingMs == 0);
}

// Elapsed time is exact across the whole int64 range, without overflow at its limits.
void ExtremeTimestamps()
{
    constexpr TimeMs kMin = std::numeric_limits<TimeMs>::min();
    constexpr TimeMs kMax = std::numeric_limits<TimeMs>::max();
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const CandidateSnapshot alone[]{Scored(1, 0.625f)};
    const CandidateSnapshot challenged[]{Scored(1, 0.625f), Scored(2, 0.75f)};

    Harness wide;
    CHECK(wide.Run(Observer(), alone, profile, kMin) == StepStatus::Ok);
    CHECK(wide.memory.focusStartTime == kMin);
    CHECK(wide.Run(Observer(), challenged, profile, kMax) == StepStatus::Ok);
    CHECK_DECISION(wide, Switch, BetterCandidate, 2);
    CHECK(wide.trace.dwellRemainingMs == 0);

    Harness late;
    CHECK(late.Run(Observer(), alone, profile, kMax - 100) == StepStatus::Ok);
    CHECK(late.Run(Observer(), challenged, profile, kMax) == StepStatus::Ok);
    CHECK_DECISION(late, Keep, MinimumDwell, 1);
    CHECK(late.trace.dwellRemainingMs == 400);

    // The longest dwell, from the earliest time, ends exactly kMax ms later.
    AttentionProfile longest = profile;
    longest.minDwellMs = kMax;
    Harness forever;
    CHECK(forever.Run(Observer(), alone, longest, kMin) == StepStatus::Ok);
    CHECK(forever.Run(Observer(), challenged, longest, kMin + 5) == StepStatus::Ok);
    CHECK_DECISION(forever, Keep, MinimumDwell, 1);
    CHECK(forever.trace.dwellRemainingMs == kMax - 5);
    CHECK(forever.Run(Observer(), challenged, longest, -2) == StepStatus::Ok);
    CHECK(forever.trace.dwellRemainingMs == 1);
    CHECK(forever.Run(Observer(), challenged, longest, -1) == StepStatus::Ok);  // kMin + kMax
    CHECK_DECISION(forever, Switch, BetterCandidate, 2);
}

// Memory written by Step never has a focus start after `now`; caller-edited memory can.
// Such a focus start gives no dwell, including the case where the unsigned elapsed time
// would wrap to 1 ms (focus start INT64_MAX, now INT64_MIN, found in an extreme-time regression).
void CallerEditedFutureFocusStart()
{
    constexpr TimeMs kMin = std::numeric_limits<TimeMs>::min();
    constexpr TimeMs kMax = std::numeric_limits<TimeMs>::max();
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = TemporalProfile(contexts);
    const CandidateSnapshot challenged[]{Scored(1, 0.625f), Scored(2, 0.75f)};

    struct Edited {
        TimeMs focusStart;
        TimeMs lastStep;
        bool hasStepped;
        TimeMs now;
        TimeMs expectedDwell;
    };
    const Edited cases[]{
        {kMax, kMin, true, kMin, 0},    // wrapped difference would be 1 ms
        {1001, 0, true, 1000, 0},       // 1 ms in the future
        {kMax, 0, false, kMin, 0},      // never stepped, so any time is accepted
        {1000, 1000, true, 1000, 500},  // control: a focus start equal to now has full dwell
    };
    for (const Edited& c : cases) {
        Harness h;
        h.memory.current = 1;
        h.memory.focusStartTime = c.focusStart;
        h.memory.lastStepTime = c.lastStep;
        h.memory.hasStepped = c.hasStepped;
        CHECK(h.Run(Observer(), challenged, profile, c.now) == StepStatus::Ok);
        CHECK(h.trace.dwellRemainingMs == c.expectedDwell);
        if (c.expectedDwell == 0) {
            CHECK_DECISION(h, Switch, BetterCandidate, 2);
            CHECK(h.memory.focusStartTime == c.now);  // the switch writes a valid start
        } else {
            CHECK_DECISION(h, Keep, MinimumDwell, 1);
        }
    }
}

// Generated streams under random policy profiles, with ties, dropouts, disabled,
// excluded and duplicated entries, empty steps, and repeated and irregular timestamps.
// A replay, a shuffled candidate order and an untraced run must give the same decision
// and memory at every step; every reason must label a kind it allows, and every reason
// must occur.
void GeneratedStreamsAreDeterministic()
{
    ContextPreference neutral{};
    ContextPreference excluding{};
    excluding.excludedTags = TagMask{1};
    const ContextPreference contexts[2]{neutral, excluding};

    test::Random rng(0x5EED0005u);
    test::Random shuffler(0x5EED0006u);
    constexpr int kReasons = 10;
    int reasonCounts[kReasons]{};
    int steps = 0;
    bool deterministic = true;
    bool labeled = true;
    for (int stream = 0; stream < 40; ++stream) {
        AttentionProfile profile = TemporalProfile(contexts);
        profile.acquireThreshold = 0.25f + 0.5f * rng.Unit();
        profile.releaseThreshold = profile.acquireThreshold * (0.5f + 0.5f * rng.Unit());
        profile.switchMargin = 0.01f + 0.2f * rng.Unit();
        profile.overrideMargin = profile.switchMargin + 0.3f * rng.Unit();
        profile.minDwellMs = static_cast<TimeMs>(rng.Below(800));
        CHECK(interest::ValidateProfile(profile) == StepStatus::Ok);

        Harness reference;
        Harness replay;
        Harness shuffled;
        ObserverMemory untracedMemory{};
        interest::StepScratch untracedScratch;
        interest::Reserve(untracedScratch, 12);
        interest::Decision untraced{};
        TimeMs now = 0;
        for (int step = 0; step < 400; ++step, ++steps) {
            now += rng.Below(4) == 0 ? 0 : static_cast<TimeMs>(rng.Below(300));
            const interest::ObserverSnapshot observer =
                Observer(static_cast<interest::ContextId>(rng.Below(8) == 0 ? 1 : 0));
            std::vector<CandidateSnapshot> candidates;
            const bool empty = rng.Below(25) == 0;
            for (CandidateId id = 1; id <= 6 && !empty; ++id) {
                if (rng.Below(5) == 0) {
                    continue;  // absent this step
                }
                // Scores on a 1/8 grid, so exact ties are common.
                CandidateSnapshot c = Scored(id, static_cast<float>(rng.Below(9)) / 8.f);
                c.tags = rng.Below(10) == 0 ? TagMask{1} : TagMask{0};
                c.enabled = rng.Below(20) != 0;
                candidates.push_back(c);
                if (rng.Below(30) == 0) {
                    candidates.push_back(c);  // duplicated id
                }
            }
            std::vector<CandidateSnapshot> permuted = candidates;
            shuffler.Shuffle(permuted);

            if (reference.Run(observer, candidates, profile, now) != StepStatus::Ok ||
                replay.Run(observer, candidates, profile, now) != StepStatus::Ok ||
                shuffled.Run(observer, permuted, profile, now) != StepStatus::Ok ||
                interest::Step(observer, candidates, profile, now, untracedMemory,
                               untracedScratch, untraced) != StepStatus::Ok) {
                deterministic = false;
                continue;
            }
            bool same = test::SameDecision(reference.output, replay.output) &&
                        test::SameMemory(reference.memory, replay.memory) &&
                        test::SamePolicyTrace(reference.trace, replay.trace) &&
                        test::SameDecision(reference.output, shuffled.output) &&
                        test::SameMemory(reference.memory, shuffled.memory) &&
                        test::SamePolicyTrace(reference.trace, shuffled.trace) &&
                        test::SameDecision(reference.output, untraced) &&
                        test::SameMemory(reference.memory, untracedMemory);
            for (std::size_t i = 0; i < candidates.size() && same; ++i) {
                const interest::CandidateTrace& entry = reference.trace.candidates[i];
                same = test::SameEntry(entry, replay.trace.candidates[i]) &&
                       test::SameEntry(entry, *shuffled.Find(entry.id));
            }
            deterministic = deterministic && same;
            labeled = labeled && test::ReasonMatchesKind(reference.output.kind,
                                                         reference.output.reason);
            const auto reason = static_cast<int>(reference.output.reason);
            if (reason < kReasons) {
                ++reasonCounts[reason];
            }
        }
    }
    CHECK(deterministic);
    CHECK(labeled);
    std::printf("  generated steps: %d\n ", steps);
    for (int reason = 0; reason < kReasons; ++reason) {
        std::printf(" %s %d", interest::ToString(static_cast<DecisionReason>(reason)),
                    reasonCounts[reason]);
        CHECK(reasonCounts[reason] > 0);  // the generator reaches every policy path
    }
    std::printf("\n");
}

} // namespace

int main()
{
    test::Run("C08 near-equal score noise", C08_NearEqualScoreNoise);
    test::Run("C09 acquire/release threshold oscillation",
              C09_AcquireReleaseThresholdOscillation);
    test::Run("C10 current target invalid during dwell", C10_CurrentTargetInvalidDuringDwell);
    test::Run("C11 persistent challenger", C11_PersistentChallenger);
    test::Run("C11a strong challenger during dwell", C11a_StrongChallengerDuringDwell);
    test::Run("exact ties keep the current target", ExactTiesKeepTheCurrentTarget);
    test::Run("explicit time semantics", ExplicitTimeSemantics);
    test::Run("extreme timestamps", ExtremeTimestamps);
    test::Run("caller-edited future focus start", CallerEditedFutureFocusStart);
    test::Run("generated streams are deterministic", GeneratedStreamsAreDeterministic);
    return test::Finish("interest_temporal_test");
}
