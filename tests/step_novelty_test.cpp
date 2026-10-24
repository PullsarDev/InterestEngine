// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Checks timestamped novelty decay, event validation, geometric gating, saturation,
// temporal selection and deterministic replay over the same candidate streams.
// Alternative compositions live only in this test; the production policy is reused.
#include "test_support.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace {
using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::DecisionKind;
using interest::Eligibility;
using interest::StepStatus;
using interest::TimeMs;
using test::Harness;
using test::Observer;
using test::Scored;

AttentionProfile NoveltyProfile(std::span<const ContextPreference> contexts)
{
    auto p = test::TemporalProfile(contexts);
    p.minDwellMs = 0;
    p.noveltyBeta = 0.75f;
    return p;
}

CandidateSnapshot Event(CandidateSnapshot c, TimeMs time, float strength = 1.f)
{
    c.eventTimeMs = time;
    c.eventStrength = strength;
    return c;
}

void CheckStep(const Harness& h, const AttentionProfile& p, TimeMs now)
{
    CHECK(test::ConsistentStep(h, p));
    CHECK(h.memory.lastStepTime == now);
    for (const auto& e : h.trace.candidates) {
        if (e.eligibility != Eligibility::Eligible) {
            CHECK(e.eventNovelty == 0 &&
                  e.noveltyContribution == 0 && e.effectiveScore == 0);
            continue;
        }
        CHECK(std::isfinite(e.rawScore) && e.rawScore >= 0 && e.rawScore <= 1);
        CHECK(std::isfinite(e.effectiveScore) && e.effectiveScore >= 0 && e.effectiveScore <= 1);
        double novelty = 0;
        if (e.eventStrength > 0.f) {
            CHECK(e.eventTimeMs <= now);
            const auto age = static_cast<std::uint64_t>(now) -
                             static_cast<std::uint64_t>(e.eventTimeMs);
            novelty = static_cast<double>(e.eventStrength) *
                std::exp2(-static_cast<double>(age) / static_cast<double>(p.noveltyHalfLifeMs));
        }
        CHECK(e.eventNovelty == novelty && novelty >= 0 && novelty <= 1);
        CHECK(e.noveltyContribution == static_cast<double>(p.noveltyBeta) *
              static_cast<double>(e.noveltyGate) * novelty);
        CHECK(e.effectiveScore == static_cast<float>((std::min)(
              1.0, static_cast<double>(e.rawScore) + e.noveltyContribution)));
        if (e.id == h.output.selected) {
            CHECK(h.output.rawScore == e.rawScore && h.output.effectiveScore == e.effectiveScore);
        }
    }
    if (h.output.selected == 0) {
        CHECK(h.output.rawScore == 0 && h.output.effectiveScore == 0);
    }
}

void C14UnchangedEventNeverRetriggers()
{
    const ContextPreference contexts[1]{};
    const auto p = NoveltyProfile(contexts);
    const CandidateSnapshot one[]{Event(Scored(1, 0.25f), -1000)};
    Harness h;
    const double expected[]{1, 0.5, 0.25, 0.125, 0.0625};
    for (int i = 0; i < 5; ++i) {
        const TimeMs now = -1000 + static_cast<TimeMs>(i) * 1000;
        CHECK(h.Run(Observer(), one, p, now) == StepStatus::Ok);
        CHECK(h.Find(1)->eventNovelty == expected[i]);
        CHECK(h.Find(1)->noveltyContribution == 0.75 * expected[i]);
        CheckStep(h, p, now);
        const auto before = h.memory;
        const auto entry = *h.Find(1);
        CHECK(h.Run(Observer(), one, p, now) == StepStatus::Ok);
        CHECK(test::SameMemory(h.memory, before));
        CHECK(test::SameEntry(*h.Find(1), entry));
        if (i == 2) {
            CHECK_DECISION(h, Keep, AboveReleaseThreshold, 1);
        }
        if (i == 3) {
            CHECK(h.output.kind == DecisionKind::None); // first call cleared; repeat stays clear
        }
    }
    CHECK(h.output.selected == 0);
    // First arrival is aged from the host event, not reception, even after ResetMemory.
    Harness late;
    CHECK(late.Run(Observer(), one, p, 3000) == StepStatus::Ok);
    CHECK(late.Find(1)->eventNovelty == 0.0625 && late.output.selected == 0);
    interest::ResetMemory(late.memory);
    CHECK(late.Run(Observer(), one, p, 3000) == StepStatus::Ok);
    CHECK(late.Find(1)->eventNovelty == 0.0625);
    // Absent event contributes zero immediately; strength zero ignores even future time.
    const CandidateSnapshot absent[]{Event(Scored(1, 0.25f),
                                     std::numeric_limits<TimeMs>::max(), 0.f)};
    CHECK(late.Run(Observer(), absent, p, 3000) == StepStatus::Ok);
    CHECK(late.Find(1)->eventNovelty == 0 && late.Find(1)->effectiveScore == 0.25f);
    CHECK(late.Run(Observer(), one, p, 4000) == StepStatus::Ok);
    CHECK(late.Find(1)->eventNovelty == 0.03125); // removing/resubmitting is not a new event

    // Population changes cannot alter absolute event age; there is no candidate cache.
    std::vector<CandidateSnapshot> crowd;
    for (CandidateId id = 1; id <= 40; ++id) {
        crowd.push_back(Event(Scored(id, 0.1f), 0, 0.125f));
    }
    Harness bounded;
    CHECK(bounded.Run(Observer(), crowd, p, 0) == StepStatus::Ok);
    CHECK(bounded.Run(Observer(), crowd, p, 1000) == StepStatus::Ok);
    CHECK(bounded.Find(40)->eventNovelty == 0.0625);
    const CandidateSnapshot returning[]{crowd.back()};
    CHECK(bounded.Run(Observer(), returning, p, 2000) == StepStatus::Ok);
    CHECK(bounded.Find(40)->eventNovelty == 0.03125);
    for (auto& c : crowd) { c.id += 40; }
    CHECK(bounded.Run(Observer(), crowd, p, 3000) == StepStatus::Ok);
    CHECK(bounded.Run(Observer(), returning, p, 4000) == StepStatus::Ok);
    CHECK(bounded.Find(40)->eventNovelty == 0.0078125);
    std::printf("  C14 novelty=1,0.5,0.25,0.125,0.0625 over 4s; no reception/population/reset spike\n");
}

void EventValidationAndExtremeAges()
{
    constexpr TimeMs lo = std::numeric_limits<TimeMs>::min();
    constexpr TimeMs hi = std::numeric_limits<TimeMs>::max();
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    Harness h;
    const CandidateSnapshot extreme[]{Event(Scored(1, 0.25f), lo)};
    CHECK(h.Run(Observer(), extreme, p, lo) == StepStatus::Ok);
    CHECK(h.Find(1)->eventNovelty == 1);
    CHECK(h.Run(Observer(), extreme, p, hi) == StepStatus::Ok);
    CHECK(h.Find(1)->eventNovelty == 0);
    p.noveltyHalfLifeMs = hi;
    Harness slow;
    CHECK(slow.Run(Observer(), extreme, p, hi) == StepStatus::Ok);
    CHECK(test::Near(slow.Find(1)->eventNovelty, 0.25, 1e-14));
    p.noveltyHalfLifeMs = 1;
    const CandidateSnapshot negative[]{Event(Scored(1, 0.25f), -10)};
    CHECK(h.Run(Observer(), {}, p, hi) == StepStatus::Ok);
    Harness fast;
    CHECK(fast.Run(Observer(), negative, p, -9) == StepStatus::Ok);
    CHECK(fast.Find(1)->eventNovelty == 0.5);
    // Whole/split observation schedules give exactly the same age-based decay.
    Harness split;
    CHECK(split.Run(Observer(), negative, p, -10) == StepStatus::Ok);
    CHECK(split.Run(Observer(), negative, p, -9) == StepStatus::Ok);
    CHECK(split.Find(1)->eventNovelty == fast.Find(1)->eventNovelty);

    p = NoveltyProfile(contexts);
    p.minDwellMs = 10000;
    const CandidateSnapshot good[]{Event(Scored(1, 0.8f), 0)};
    const float badStrengths[]{-0.1f, 1.01f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()};
    for (float beta : {0.f, 0.75f}) {
        p.noveltyBeta = beta;
        for (float strength : badStrengths) {
            Harness invalid;
            CHECK(invalid.Run(Observer(), good, p, 0) == StepStatus::Ok);
            const CandidateSnapshot bad[]{Event(Scored(1, 0.8f), 0, strength)};
            CHECK(invalid.Run(Observer(), bad, p, 100) == StepStatus::Ok);
            CHECK(invalid.Find(1)->eligibility == Eligibility::InvalidEvent);
            CHECK_DECISION(invalid, Clear, CurrentInvalid, 0);
            CheckStep(invalid, p, 100);
        }
        for (TimeMs future : {TimeMs{101}, hi}) {
            Harness invalid;
            CHECK(invalid.Run(Observer(), good, p, 0) == StepStatus::Ok);
            const CandidateSnapshot bad[]{Event(Scored(1, 0.8f), future), Scored(2, 0.625f)};
            CHECK(invalid.Run(Observer(), bad, p, 100) == StepStatus::Ok);
            CHECK(invalid.Find(1)->eligibility == Eligibility::InvalidEvent);
            CHECK_DECISION(invalid, Switch, CurrentInvalid, 2);
            CHECK(invalid.trace.dwellRemainingMs == 9900);
            CheckStep(invalid, p, 100);
        }
    }
    const CandidateSnapshot duplicates[]{Event(Scored(1, 1.f), hi),
        Event(Scored(1, 1.f), lo), Event(Scored(1, 1.f), 0)};
    Harness duplicated;
    CHECK(duplicated.Run(Observer(), duplicates, p, 100) == StepStatus::Ok);
    for (const auto& e : duplicated.trace.candidates) {
        CHECK(e.eligibility == Eligibility::DuplicateId);
    }
    CHECK(duplicated.memory.current == 0);
    CHECK(std::string_view(interest::ToString(Eligibility::InvalidEvent)) == "InvalidEvent");
}

void OrdinaryPolicyCapturesAndReturns()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.minDwellMs = 500;
    for (TimeMs interval : {TimeMs{50}, TimeMs{200}}) {
        Harness h, b2;
        auto off = p; off.noveltyBeta = 0;
        TimeMs captured = -1;
        TimeMs returned = -1;
        int switches = 0;
        for (TimeMs now = 0; now <= 5000; now += interval) {
            const CandidateSnapshot frame[]{Scored(1, 0.625f), now < 150
                ? Scored(2, 0.25f) : Event(Scored(2, 0.25f), 150)};
            CHECK(h.Run(Observer(), frame, p, now) == StepStatus::Ok);
            CHECK(b2.Run(Observer(), frame, off, now) == StepStatus::Ok);
            CHECK(b2.output.selected == 1);
            CheckStep(h, p, now);
            if (h.output.kind == DecisionKind::Switch) {
                ++switches;
                if (h.output.selected == 2) {
                    captured = now;
                    CHECK_DECISION(h, Switch, DwellOverride, 2);
                } else {
                    returned = now;
                    CHECK_DECISION(h, Switch, BetterCandidate, 1);
                }
            }
        }
        CHECK(captured >= 150 && captured - 150 < interval);
        CHECK(returned >= 1700 && returned <= 2000 && switches == 2);
        std::printf("  event capture/normal return %lldHz: latency=%lldms return=%lldms "
                    "switches=%d; B2 misses event\n", static_cast<long long>(1000 / interval),
                    static_cast<long long>(captured - 150), static_cast<long long>(returned), switches);
    }
}

void SpatialGatingComparison()
{
    const ContextPreference contexts[1]{};
    const auto p = NoveltyProfile(contexts);
    auto alternativeProfile = p;
    alternativeProfile.noveltyBeta = 0;
    const CandidateSnapshot incumbent[]{Scored(1, 0.6f)};
    struct Row { const char* name; CandidateSnapshot c; bool eligible; bool takeFull; bool takeLess; };
    const Row rows[]{
        {"front positive control", test::PointAt(2, 5, 0, 0.4f, 0), true, true, true},
        {"far 19/20", test::PointAt(2, 19, 0, 0.4f, 0), true, false, true},
        {"behind", test::PointAt(2, -5, 0, 0.4f, 0), true, false, true},
        {"at cutoff", test::PointAt(2, 20, 0, 0.4f, 0), true, false, true},
        {"outside cutoff", test::PointAt(2, 21, 0, 0.4f, 0), false, false, false},
        {"behind bearing", [] { auto c = Scored(2, 0.4f); c.spatial = interest::SpatialKind::Bearing;
                                 c.bearing = {-1.f, 0, 0}; return c; }(), true, false, true},
        {"nonspatial positive control", Scored(2, 0.4f), true, true, true}
    };
    int falseFull = 0;
    int falseLess = 0;
    for (const auto& row : rows) {
        const CandidateSnapshot frame[]{incumbent[0], Event(row.c, 100)};
        Harness full, less;
        CHECK(full.Run(Observer(), incumbent, p, 0) == StepStatus::Ok);
        CHECK(less.Run(Observer(), incumbent, alternativeProfile, 0) == StepStatus::Ok);
        CHECK(full.Run(Observer(), frame, p, 100) == StepStatus::Ok);
        CheckStep(full, p, 100);
        const auto& e = *full.Find(2);
        CHECK((e.eligibility == Eligibility::Eligible) == row.eligible);
        // Test-only composition from the same raw score and decayed event. Feed
        // synthetic scores through the actual temporal policy; no second policy/scorer API.
        const float lessScore = static_cast<float>((std::min)(1.0, static_cast<double>(e.rawScore) +
            static_cast<double>(p.noveltyBeta) * static_cast<double>(e.perceptionStrength) * e.eventNovelty));
        CandidateSnapshot scores[]{Scored(1, full.Find(1)->effectiveScore), Scored(2, lessScore)};
        scores[1].enabled = row.eligible;
        CHECK(less.Run(Observer(), scores, alternativeProfile, 100) == StepStatus::Ok);
        CHECK((full.output.selected == 2) == row.takeFull);
        CHECK((less.output.selected == 2) == row.takeLess);
        const bool distractor = row.takeLess && !row.takeFull;
        falseFull += distractor && full.output.selected == 2 ? 1 : 0;
        falseLess += distractor && less.output.selected == 2 ? 1 : 0;
        std::printf("  spatial %-26s gate=%.6f raw=%.6f full=%.6f less=%.6f "
                    "selected=%llu/%llu\n", row.name, static_cast<double>(e.gate),
                    static_cast<double>(e.rawScore), static_cast<double>(e.effectiveScore),
                    static_cast<double>(lessScore), static_cast<unsigned long long>(full.output.selected),
                    static_cast<unsigned long long>(less.output.selected));
    }
    CHECK(falseFull == 0 && falseLess == 4);
    std::printf("  labelled low-access distractors: full=%d/4 less=%d/4; reversing labels "
                "makes full gating miss 4 relevant events (a real tradeoff)\n", falseFull, falseLess);
}

void SaturationAndPolicyBoundaries()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.noveltyBeta = 1;
    const CandidateSnapshot both[]{Event(Scored(1, 1.f), 0), Event(Scored(2, 0.75f), 0)};
    Harness h;
    CHECK(h.Run(Observer(), std::span<const CandidateSnapshot>(both + 1, 1), p, 0) == StepStatus::Ok);
    CHECK(h.Run(Observer(), both, p, 0) == StepStatus::Ok);
    CHECK(h.Find(1)->effectiveScore == 1 && h.Find(2)->effectiveScore == 1);
    CHECK(static_cast<double>(h.Find(1)->rawScore) + h.Find(1)->noveltyContribution == 2);
    CHECK_DECISION(h, Keep, InsufficientAdvantage, 2); // saturation tie cannot switch incumbent
    Harness fresh;
    CHECK(fresh.Run(Observer(), both, p, 0) == StepStatus::Ok);
    CHECK(fresh.output.selected == 1); // tie acquisition goes to lower ID
    CheckStep(h, p, 0);

    p = NoveltyProfile(contexts);
    p.minDwellMs = 10000;
    const CandidateSnapshot current[]{Scored(1, 0.625f)};
    Harness protectedTarget;
    CHECK(protectedTarget.Run(Observer(), current, p, 0) == StepStatus::Ok);
    // A weaker event produces a +.125 lead: switch margin passes but dwell holds.
    const CandidateSnapshot modest[]{current[0], Event(Scored(2, 0.375f), 100, 0.5f)};
    CHECK(protectedTarget.Run(Observer(), modest, p, 100) == StepStatus::Ok);
    CHECK_DECISION(protectedTarget, Keep, MinimumDwell, 1);
    const CandidateSnapshot strong[]{current[0], Event(Scored(2, 0.375f), 100)};
    CHECK(protectedTarget.Run(Observer(), strong, p, 100) == StepStatus::Ok);
    CHECK_DECISION(protectedTarget, Switch, DwellOverride, 2);
    // Strong event never fabricates perception or rescues an invalid gate.
    auto hidden = strong[1]; hidden.perceptionStrength = 0;
    const CandidateSnapshot invalid[]{current[0], hidden};
    CHECK(protectedTarget.Run(Observer(), invalid, p, 100) == StepStatus::Ok);
    CHECK_DECISION(protectedTarget, Switch, CurrentInvalid, 1);
    CheckStep(protectedTarget, p, 100);

    // A score lead alone does not bypass acquire, even if it passes overrideMargin.
    p.acquireThreshold = 0.75f;
    Harness threshold;
    const CandidateSnapshot first[]{Scored(1, 0.8f)};
    CHECK(threshold.Run(Observer(), first, p, 0) == StepStatus::Ok);
    const CandidateSnapshot below[]{Scored(1, 0.375f), Event(Scored(2, 0.25f), 100, 0.5f)};
    CHECK(threshold.Run(Observer(), below, p, 100) == StepStatus::Ok);
    CHECK_DECISION(threshold, Keep, InsufficientAdvantage, 1);
    CheckStep(threshold, p, 100);
}

void ErrorsPreserveAllBuffers()
{
    const ContextPreference contexts[1]{};
    const auto p = NoveltyProfile(contexts);
    CandidateSnapshot frame[]{Event(Scored(1, 0.4f), 0)};
    frame[0].eventDirectionality = 0.5f;
    Harness h;
    CHECK(h.Run(Observer(), frame, p, 1000) == StepStatus::Ok);
    const Harness before = h;
    const auto* scratchData = h.scratch.sortedIds.data();
    const auto* traceData = h.trace.candidates.data();
    const auto scratchCapacity = h.scratch.sortedIds.capacity();
    const auto traceCapacity = h.trace.candidates.capacity();
    const auto unchanged = [&] {
        CHECK(test::SameMemory(h.memory, before.memory) && test::SameDecision(h.output, before.output));
        CHECK(test::SamePolicyTrace(h.trace, before.trace));
        CHECK(h.trace.candidates.size() == before.trace.candidates.size());
        CHECK(test::SameEntry(h.trace.candidates[0], before.trace.candidates[0]));
        CHECK(h.scratch.sortedIds == before.scratch.sortedIds && h.scratch.sortedIds.data() == scratchData);
        CHECK(h.trace.candidates.data() == traceData && h.trace.candidates.capacity() == traceCapacity);
        CHECK(h.scratch.sortedIds.capacity() == scratchCapacity);
    };
    for (float beta : {-1.f, 1.01f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = p; bad.noveltyBeta = beta;
        CHECK(interest::ValidateProfile(bad) == StepStatus::InvalidProfile);
        CHECK(h.Run(Observer(), frame, bad, 2000) == StepStatus::InvalidProfile);
        unchanged();
    }
    for (TimeMs halfLife : {TimeMs{0}, TimeMs{-1}, std::numeric_limits<TimeMs>::min()}) {
        auto bad = p; bad.noveltyHalfLifeMs = halfLife;
        CHECK(interest::ValidateProfile(bad) == StepStatus::InvalidProfile);
        CHECK(h.Run(Observer(), frame, bad, 2000) == StepStatus::InvalidProfile);
        unchanged();
    }
    auto invalid = Observer(); invalid.referenceForward = {};
    CHECK(h.Run(invalid, frame, p, 2000) == StepStatus::InvalidObserver);
    unchanged();
    CHECK(h.Run(Observer(), frame, p, 999) == StepStatus::TimeWentBackwards);
    unchanged();
    const std::vector<CandidateSnapshot> oversized(scratchCapacity + 1, frame[0]);
    CHECK(interest::Step(Observer(), oversized, p, 2000, h.memory, h.scratch, h.output,
                         &h.trace) == StepStatus::InsufficientCapacity);
    unchanged();
    interest::DecisionTrace shortTrace;
    CHECK(interest::Step(Observer(), frame, p, 2000, h.memory, h.scratch, h.output,
                         &shortTrace) == StepStatus::InsufficientCapacity);
    CHECK(shortTrace.candidates.empty() && shortTrace.candidates.capacity() == 0);
    unchanged();
}

void ReplayPermutationsAndAblation()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.minDwellMs = 500;
    auto off = p; off.noveltyBeta = 0;
    Harness h, shuffled, untraced, noveltyOff, withoutEvents;
    test::Random rng(0xC140015);
    TimeMs now = -10000;
    for (int step = 0; step < 2000; ++step) {
        now += static_cast<TimeMs>(rng.Below(201));
        std::vector<CandidateSnapshot> frame;
        for (int i = 0; i < 40; ++i) {
            auto c = Event(Scored(1 + rng.Below(64), rng.Unit()),
                           now - static_cast<TimeMs>(rng.Below(10001)), rng.Unit());
            c.spatial = static_cast<interest::SpatialKind>(rng.Below(3));
            c.position = {static_cast<double>(rng.Below(41)) - 20, 0, 0};
            c.bearing = {rng.Below(2) == 0 ? -1.f : 1.f, 0, 0};
            c.perceptionStrength = rng.Unit();
            c.eventDirectionality = static_cast<float>(step % 5) / 4.f;
            c.enabled = rng.Below(20) != 0;
            frame.push_back(c);
        }
        Harness replay;
        replay.memory = h.memory;
        CHECK(h.Run(Observer(), frame, p, now) == StepStatus::Ok);
        CHECK(replay.Run(Observer(), frame, p, now) == StepStatus::Ok);
        CHECK(test::SameMemory(h.memory, replay.memory) && test::SameDecision(h.output, replay.output));
        CHECK(test::SamePolicyTrace(h.trace, replay.trace));
        for (std::size_t i = 0; i < frame.size(); ++i) {
            CHECK(test::SameEntry(h.trace.candidates[i], replay.trace.candidates[i]));
        }
        CheckStep(h, p, now);
        interest::Reserve(untraced.scratch, frame.size());
        CHECK(interest::Step(Observer(), frame, p, now, untraced.memory, untraced.scratch,
                             untraced.output) == StepStatus::Ok);
        CHECK(test::SameMemory(h.memory, untraced.memory) && test::SameDecision(h.output, untraced.output));
        CHECK(noveltyOff.Run(Observer(), frame, off, now) == StepStatus::Ok);
        auto absent = frame;
        for (auto& c : absent) { c.eventStrength = 0; }
        CHECK(withoutEvents.Run(Observer(), absent, off, now) == StepStatus::Ok);
        CHECK(test::SameMemory(noveltyOff.memory, withoutEvents.memory));
        CHECK(test::SameDecision(noveltyOff.output, withoutEvents.output));
        CHECK(test::SamePolicyTrace(noveltyOff.trace, withoutEvents.trace));
        rng.Shuffle(frame);
        CHECK(shuffled.Run(Observer(), frame, p, now) == StepStatus::Ok);
        CHECK(test::SameMemory(h.memory, shuffled.memory) && test::SameDecision(h.output, shuffled.output));
        CHECK(test::SamePolicyTrace(h.trace, shuffled.trace));
        for (const auto& e : shuffled.trace.candidates) {
            if (e.eligibility == Eligibility::Eligible) {
                CHECK(test::SameEntry(e, *h.Find(e.id)));
            }
        }
    }
    std::printf("  2000 x 40 event inputs: copy replay, shuffled replay, trace off, beta-zero "
                "B2 decision/memory ablation identical\n");
}

void DirectionalityGatesOnlyNovelty()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.angleFloor = 0.25f;
    // Both geometry kinds with angular gates, plus the NonSpatial identity case.
    for (auto kind : {interest::SpatialKind::Point, interest::SpatialKind::Bearing,
                      interest::SpatialKind::NonSpatial}) {
        for (double sign : {-1.0, 1.0}) {
            float previousGate = 1.f;
            float previousRaw = -1.f;
            for (float d : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
                auto c = Event(Scored(1, 0.25f), 0);
                c.spatial = kind;
                c.position = {15.0 * sign, 0, 0}; // distance falloff .5
                c.bearing = {static_cast<float>(sign), 0, 0};
                c.perceptionStrength = 0.5f;
                c.eventDirectionality = d;
                Harness h;
                CHECK(h.Run(Observer(), std::span(&c, 1), p, 1000) == StepStatus::Ok);
                const auto& e = *h.Find(1);
                const double distance = kind == interest::SpatialKind::Point ? 0.5 : 1.0;
                const double angle = kind == interest::SpatialKind::NonSpatial || sign > 0 ? 1.0 : 0.25;
                CHECK(e.distanceGate == distance && e.angleGate == angle);
                CHECK(e.noveltyGate == distance * (1.0 - d * (1.0 - angle)) * 0.5);
                CHECK(e.gate == distance * angle * 0.5);
                CHECK(e.rawScore == distance * angle * 0.5 * 0.25);
                CHECK(e.noveltyGate <= previousGate);
                CHECK(previousRaw < 0 || e.rawScore == previousRaw);
                CHECK(e.eventNovelty == 0.5);
                if (d == 1.f) { CHECK(e.noveltyGate == e.gate); }
                previousGate = e.noveltyGate;
                previousRaw = e.rawScore;
                CheckStep(h, p, 1000);
            }
        }
    }
    for (float d : {0.f, 0.5f, 1.f}) {
        for (double distance : {20.0, std::nextafter(20.0, 21.0), 21.0}) {
            auto c = Event(test::PointAt(1, -distance, 0, 1.f, 1.f), 0);
            c.eventDirectionality = d;
            Harness h;
            CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
            CHECK(h.Find(1)->eligibility == (distance > 20 ? Eligibility::OutOfRange : Eligibility::Eligible));
            CHECK(h.Find(1)->noveltyGate == 0 && h.Find(1)->effectiveScore == 0);
            c.maxDistance = 10.f;
            CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
            CHECK(h.Find(1)->eligibility == Eligibility::OutOfRange);
            c.position = {-5, 0, 0};
            c.perceptionStrength = 0;
            CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
            CHECK(h.Find(1)->eligibility == Eligibility::NotPerceived);
            CHECK(h.Find(1)->noveltyGate == 0 && h.Find(1)->effectiveScore == 0);
        }
    }
}

void DefaultPreservesLegacyRoundedGate()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    auto off = p; off.noveltyBeta = 0;
    test::Random rng(0xD120010);
    int differentRounding = 0;
    for (int i = 0; i < 1000; ++i) {
        auto c = Event(test::PointAt(1, 10.0 + 8.0 * rng.Unit(), rng.Unit(),
                                     rng.Unit(), rng.Unit()), 0, rng.Unit());
        c.perceptionStrength = 0.1f + 0.9f * rng.Unit();
        CHECK(c.eventDirectionality == 1.f);
        Harness h, base, explicitDefault;
        CHECK(base.Run(Observer(), std::span(&c, 1), off, 1000) == StepStatus::Ok);
        CHECK(h.Run(Observer(), std::span(&c, 1), p, 1000) == StepStatus::Ok);
        const auto& e = *h.Find(1);
        auto expected = *base.Find(1);
        // Legacy composition uses this rounded gate, never a product of trace factors.
        expected.noveltyContribution = static_cast<double>(p.noveltyBeta) *
                                      static_cast<double>(expected.gate) * expected.eventNovelty;
        expected.effectiveScore = static_cast<float>((std::min)(
            1.0, static_cast<double>(expected.rawScore) + expected.noveltyContribution));
        CHECK(test::SameEntry(e, expected));
        CHECK(e.attraction == expected.attraction && e.salience == expected.salience &&
              e.perceptionStrength == expected.perceptionStrength);
        CHECK(e.noveltyGate == e.gate);
        const float recombined = static_cast<float>(static_cast<double>(e.distanceGate) *
            static_cast<double>(e.angleGate) * static_cast<double>(e.perceptionStrength));
        differentRounding += recombined != e.gate ? 1 : 0;
        c.eventDirectionality = 1.f;
        CHECK(explicitDefault.Run(Observer(), std::span(&c, 1), p, 1000) == StepStatus::Ok);
        CHECK(test::SameEntry(e, *explicitDefault.Find(1)));
        CHECK(test::SameDecision(h.output, explicitDefault.output));
        CHECK(test::SamePolicyTrace(h.trace, explicitDefault.trace));
        CHECK(test::SameMemory(h.memory, explicitDefault.memory));
        CheckStep(h, p, 1000);
    }
    CHECK(differentRounding > 100); // this fixture detects a rounded-factor recomputation
}

void InvalidDirectionalityAndPrecedence()
{
    ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.minDwellMs = 10000;
    const float bad[]{-0.01f, 1.01f, -std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (float beta : {0.f, 0.75f}) {
        p.noveltyBeta = beta;
        for (float strength : {0.f, 1.f}) {
            for (float d : bad) {
                Harness h;
                const CandidateSnapshot first[]{Scored(1, 0.8f)};
                CHECK(h.Run(Observer(), first, p, 0) == StepStatus::Ok);
                CandidateSnapshot frame[]{Event(first[0], 0, strength), Scored(2, 0.625f)};
                frame[0].eventDirectionality = d;
                CHECK(h.Run(Observer(), frame, p, 100) == StepStatus::Ok);
                CHECK(h.Find(1)->eligibility == Eligibility::InvalidEvent);
                CHECK_DECISION(h, Switch, CurrentInvalid, 2);
                CHECK(h.Find(1)->noveltyGate == 0);
                CheckStep(h, p, 100);
            }
        }
    }
    // Earlier checks still win; later eligibility failures never hide InvalidEvent.
    auto c = Event(test::PointAt(1, 30, 0, 1.f, 1.f), 0);
    c.eventDirectionality = -1;
    const auto check = [&](const CandidateSnapshot& value, Eligibility expected) {
        Harness h;
        CHECK(h.Run(Observer(), std::span(&value, 1), p, 0) == StepStatus::Ok);
        CHECK(h.trace.candidates[0].eligibility == expected);
    };
    check(c, Eligibility::InvalidEvent);
    c.enabled = false; check(c, Eligibility::InvalidEvent);
    c.perceptionStrength = 0; check(c, Eligibility::InvalidEvent);
    contexts[0].requiredTags = 1; check(c, Eligibility::InvalidEvent);
    contexts[0].requiredTags = 0; contexts[0].excludedTags = 1; c.tags = 1;
    check(c, Eligibility::InvalidEvent);
    c.position.x = std::numeric_limits<double>::quiet_NaN();
    check(c, Eligibility::InvalidGeometry);
    c.attraction = -1; check(c, Eligibility::InvalidValue);
    c.id = 0; check(c, Eligibility::InvalidId);
    c.id = 1;
    const CandidateSnapshot duplicates[]{c, c};
    Harness h;
    CHECK(h.Run(Observer(), duplicates, p, 0) == StepStatus::Ok);
    for (const auto& e : h.trace.candidates) { CHECK(e.eligibility == Eligibility::DuplicateId); }
}

void DirectionalCaptureDecayAndConditionalReturn()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.angleFloor = 0;
    p.minDwellMs = 500;
    for (float d : {0.f, 0.5f, 1.f}) {
        for (float previousScore : {0.625f, 0.25f}) {
            Harness h;
            const CandidateSnapshot first[]{Scored(1, 0.625f)};
            CHECK(h.Run(Observer(), first, p, 0) == StepStatus::Ok);
            auto event = Event(test::PointAt(2, -5, 0, 1.f, 1.f), 100);
            event.eventDirectionality = d;
            CandidateSnapshot frame[]{Scored(1, previousScore), event};
            CHECK(h.Run(Observer(), frame, p, 100) == StepStatus::Ok);
            CHECK(h.Find(2)->rawScore == 0);
            CHECK(h.Find(2)->effectiveScore == 0.75f * (1.f - d));
            const bool captured = d == 0.f && previousScore == 0.25f;
            CHECK((h.output.selected == 2) == captured);
            if (captured) { CHECK_DECISION(h, Switch, DwellOverride, 2); }
            CHECK(h.Run(Observer(), frame, p, 1100) == StepStatus::Ok);
            CHECK(h.Find(2)->eventNovelty == 0.5);
            CHECK(h.Run(Observer(), frame, p, 2100) == StepStatus::Ok);
            CHECK(h.Find(2)->eventNovelty == 0.25);
            if (captured) {
                CHECK_DECISION(h, Clear, BelowReleaseThreshold, 0);
                // Return requires the old target to qualify for acquisition again.
                frame[0].attraction = 0.625f;
                CHECK(h.Run(Observer(), frame, p, 2200) == StepStatus::Ok);
                CHECK_DECISION(h, Acquire, HighestScore, 1);
            }
            CheckStep(h, p, captured ? 2200 : 2100);
        }
        // A qualifying previous target returns directly after a captured event fades.
        Harness h;
        const CandidateSnapshot first[]{Scored(1, 0.625f)};
        CHECK(h.Run(Observer(), first, p, 0) == StepStatus::Ok);
        auto event = Event(test::PointAt(2, -5, 0, 1.f, 1.f), 500);
        event.eventDirectionality = d;
        const CandidateSnapshot frame[]{first[0], event};
        CHECK(h.Run(Observer(), frame, p, 500) == StepStatus::Ok);
        CHECK((h.output.selected == 2) == (d == 0.f));
        CHECK(h.Run(Observer(), frame, p, 2500) == StepStatus::Ok);
        CHECK(h.output.selected == 1);
        if (d == 0.f) { CHECK_DECISION(h, Switch, BelowReleaseThreshold, 1); }
        CheckStep(h, p, 2500);
    }
}

void EnableAndResetPreserveEventAge()
{
    const ContextPreference contexts[1]{};
    auto p = NoveltyProfile(contexts);
    p.noveltyBeta = 0;
    const CandidateSnapshot frame[]{Event(Scored(1, 0.8f), 0)};
    Harness h;
    CHECK(h.Run(Observer(), frame, p, 0) == StepStatus::Ok);
    CHECK(h.Run(Observer(), frame, p, 2000) == StepStatus::Ok);
    CHECK(h.Find(1)->effectiveScore == 0.8f && h.Find(1)->eventNovelty == 0.25);
    p.noveltyBeta = 0.75f;
    CHECK(h.Run(Observer(), frame, p, 2000) == StepStatus::Ok);
    CHECK(h.Find(1)->effectiveScore == 0.9875f && h.Find(1)->eventNovelty == 0.25);
    interest::ResetMemory(h.memory);
    CHECK(h.Run(Observer(), frame, p, 2000) == StepStatus::Ok);
    CHECK(h.Find(1)->effectiveScore == 0.9875f && h.Find(1)->eventNovelty == 0.25);
    CheckStep(h, p, 2000);
    std::printf("  novelty age survives enable/reset; observer=%zu candidate=%zu trace=%zu bytes\n",
                sizeof(interest::ObserverMemory), sizeof(CandidateSnapshot), sizeof(interest::CandidateTrace));
}

} // namespace

int main()
{
    test::Run("directionality gates novelty independently of raw scoring", DirectionalityGatesOnlyNovelty);
    test::Run("default preserves legacy rounded gate and all trace fields", DefaultPreservesLegacyRoundedGate);
    test::Run("invalid directionality and eligibility precedence", InvalidDirectionalityAndPrecedence);
    test::Run("directional capture, decay and conditional return", DirectionalCaptureDecayAndConditionalReturn);
    test::Run("C14 unchanged timestamp decays without retrigger", C14UnchangedEventNeverRetriggers);
    test::Run("event validation and extreme signed ages", EventValidationAndExtremeAges);
    test::Run("event capture and ordinary-policy return", OrdinaryPolicyCapturesAndReturns);
    test::Run("spatial versus perception-only novelty", SpatialGatingComparison);
    test::Run("saturation and preserved temporal boundaries", SaturationAndPolicyBoundaries);
    test::Run("errors preserve novelty traces and every buffer", ErrorsPreserveAllBuffers);
    test::Run("copy replay, permutations, trace off and novelty-off", ReplayPermutationsAndAblation);
    test::Run("enable and reset preserve absolute event age", EnableAndResetPreserveEventAge);
    return test::Finish("interest_novelty_test");
}
