// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Core snapshot, eligibility, decision and validation scenarios.

#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using interest::AttentionProfile;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::Decision;
using interest::DecisionKind;
using interest::DecisionReason;
using interest::DecisionTrace;
using interest::Eligibility;
using interest::ObserverMemory;
using interest::ObserverSnapshot;
using interest::SpatialKind;
using interest::StepStatus;
using interest::TagMask;
using test::Harness;
using test::NonSpatial;
using test::Observer;
using test::PointAt;
using test::Profile;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr double kNaND = std::numeric_limits<double>::quiet_NaN();
constexpr double kInfD = std::numeric_limits<double>::infinity();

constexpr TagMask Tag(unsigned bit)
{
    return TagMask{1} << bit;
}

Eligibility EligibilityOf(const Harness& h, interest::CandidateId id)
{
    const interest::CandidateTrace* entry = h.Find(id);
    return entry != nullptr ? entry->eligibility : Eligibility::Eligible;
}

// Values a failed step must leave untouched.
void FillSentinels(Harness& h)
{
    h.output = Decision{DecisionKind::Switch, DecisionReason::HighestScore, 77, 66, 0.25f};
    h.trace.candidates.assign(1, interest::CandidateTrace{});
    h.trace.candidates[0].id = 99;
    h.trace.leading = 99;
    h.trace.leadingScore = 0.75f;
    h.trace.challenger = 98;
    h.trace.dwellRemainingMs = 1234;
    h.trace.acquireThreshold = 0.5f;
    h.trace.overrideMargin = 0.125f;
}

bool SentinelsIntact(const Harness& h)
{
    const Decision expected{DecisionKind::Switch, DecisionReason::HighestScore, 77, 66, 0.25f};
    return test::SameDecision(h.output, expected) && h.trace.candidates.size() == 1 &&
           h.trace.candidates[0].id == 99 && h.trace.leading == 99 &&
           h.trace.leadingScore == 0.75f && h.trace.challenger == 98 &&
           h.trace.dwellRemainingMs == 1234 && h.trace.acquireThreshold == 0.5f &&
           h.trace.overrideMargin == 0.125f;
}

void C01_EmptyCandidateList()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    Harness h;

    CHECK(h.Run(Observer(), {}, profile, 0) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::None);
    CHECK(h.output.reason == DecisionReason::NoEligibleCandidate);
    CHECK(h.output.selected == 0 && h.output.previous == 0 && h.output.rawScore == 0.f);
    CHECK(h.trace.candidates.empty() && h.trace.leading == 0);
    CHECK(h.memory.current == 0 && h.memory.hasStepped && h.memory.lastStepTime == 0);

    // With a previous selection, an empty list clears it. The reason names the
    // absent current target rather than the empty list.
    const CandidateSnapshot one[]{PointAt(7, 5, 0, 1.f, 1.f)};
    CHECK(h.Run(Observer(), one, profile, 10) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire && h.output.selected == 7);
    CHECK(h.Run(Observer(), {}, profile, 20) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Clear);
    CHECK(h.output.reason == DecisionReason::CurrentInvalid);
    CHECK(h.output.previous == 7 && h.output.selected == 0);
    CHECK(h.memory.current == 0);
}

void C02_InvalidDisabledAndOutOfRangeCandidates()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);

    // Every rejected candidate would outscore the valid one (id 100) if it were eligible.
    const auto strong = [](interest::CandidateId id, auto change) {
        CandidateSnapshot candidate = PointAt(id, 5, 0, 1.f, 1.f);
        change(candidate);
        return candidate;
    };
    using Edit = CandidateSnapshot&;
    std::vector<CandidateSnapshot> candidates{
        strong(0, [](Edit) {}),
        strong(1, [](Edit c) { c.attraction = kNaN; }),
        strong(2, [](Edit c) { c.salience = 1.5f; }),
        strong(3, [](Edit c) { c.perceptionStrength = -0.1f; }),
        strong(4, [](Edit c) { c.perceptionStrength = kInf; }),
        strong(5, [](Edit c) { c.maxDistance = kNaN; }),
        strong(6, [](Edit c) { c.maxDistance = -1.f; }),
        strong(7, [](Edit c) { c.spatial = static_cast<SpatialKind>(7); }),
        strong(8, [](Edit c) { c.position.x = kInfD; }),
        strong(9, [](Edit c) { c.position.y = kNaND; }),
        strong(10, [](Edit c) { c.spatial = SpatialKind::Bearing; c.bearing = {0.f, 0.f, 0.f}; }),
        strong(11, [](Edit c) { c.spatial = SpatialKind::Bearing; c.bearing = {kNaN, 1.f, 0.f}; }),
        strong(12, [](Edit c) { c.enabled = false; }),
        strong(13, [](Edit c) { c.perceptionStrength = 0.f; }),
        strong(14, [](Edit c) { c.position.x = 20.5; }),
        strong(15, [](Edit c) { c.position = {1e308, 1e308, 0.0}; }),  // distance overflows
        strong(16, [](Edit c) { c.maxDistance = 4.f; }),                  // own cutoff < 5
        strong(17, [](Edit c) { c.enabled = false; c.attraction = kNaN; }),
    };
    candidates.push_back(PointAt(100, 5, 0, 0.5f, 0.5f));

    Harness h;
    CHECK(h.Run(Observer(), candidates, profile, 0) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire && h.output.selected == 100);
    CHECK(h.trace.candidates.size() == candidates.size());
    CHECK(h.trace.candidates[0].eligibility == Eligibility::InvalidId);
    for (interest::CandidateId id = 1; id <= 7; ++id) {
        CHECK(EligibilityOf(h, id) == Eligibility::InvalidValue);
    }
    for (interest::CandidateId id = 8; id <= 11; ++id) {
        CHECK(EligibilityOf(h, id) == Eligibility::InvalidGeometry);
    }
    CHECK(EligibilityOf(h, 12) == Eligibility::Disabled);
    CHECK(EligibilityOf(h, 13) == Eligibility::NotPerceived);
    CHECK(EligibilityOf(h, 14) == Eligibility::OutOfRange);
    CHECK(EligibilityOf(h, 15) == Eligibility::OutOfRange);
    CHECK(EligibilityOf(h, 16) == Eligibility::OutOfRange);
    CHECK(EligibilityOf(h, 17) == Eligibility::InvalidValue);  // invalid input outranks Disabled
    CHECK(h.Find(14)->distance > 20.0 && std::isinf(h.Find(15)->distance));
    CHECK(h.Find(1)->rawScore == 0.f && h.Find(14)->rawScore == 0.f);

    // Without the valid candidate there is no false target.
    candidates.pop_back();
    Harness none;
    CHECK(none.Run(Observer(), candidates, profile, 0) == StepStatus::Ok);
    CHECK(none.output.kind == DecisionKind::None);
    CHECK(none.output.reason == DecisionReason::NoEligibleCandidate);
    CHECK(none.trace.leading == 0);

    // Fields unused by a spatial kind are not validated.
    CandidateSnapshot pointWithBadBearing = PointAt(1, 5, 0, 0.5f, 0.5f);
    pointWithBadBearing.bearing = {kNaN, kNaN, kNaN};
    CandidateSnapshot nonSpatialWithBadPosition = NonSpatial(2, 0.5f, 0.5f);
    nonSpatialWithBadPosition.position.x = kNaND;
    nonSpatialWithBadPosition.maxDistance = kNaN;
    const CandidateSnapshot unusedFields[]{pointWithBadBearing, nonSpatialWithBadPosition};
    Harness unused;
    CHECK(unused.Run(Observer(), unusedFields, profile, 0) == StepStatus::Ok);
    CHECK(EligibilityOf(unused, 1) == Eligibility::Eligible);
    CHECK(EligibilityOf(unused, 2) == Eligibility::Eligible);

    // Required and excluded context tags.
    ContextPreference tagged{};
    tagged.requiredTags = Tag(0);
    tagged.excludedTags = Tag(1);
    const ContextPreference taggedContexts[1]{tagged};
    const AttentionProfile tagProfile = Profile(taggedContexts);
    CandidateSnapshot untagged = PointAt(1, 5, 0, 1.f, 1.f);
    CandidateSnapshot excluded = PointAt(2, 5, 0, 1.f, 1.f);
    excluded.tags = Tag(0) | Tag(1);
    CandidateSnapshot allowed = PointAt(3, 5, 0, 0.5f, 0.5f);
    allowed.tags = Tag(0) | Tag(5);
    const CandidateSnapshot tagCandidates[]{untagged, excluded, allowed};
    Harness tags;
    CHECK(tags.Run(Observer(), tagCandidates, tagProfile, 0) == StepStatus::Ok);
    CHECK(EligibilityOf(tags, 1) == Eligibility::MissingRequiredTag);
    CHECK(EligibilityOf(tags, 2) == Eligibility::ExcludedTag);
    CHECK(tags.output.selected == 3);
}

void C03_AcquisitionAndNoTarget()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    const CandidateSnapshot winner = PointAt(1, 5, 0, 0.9f, 0.9f);  // (0.9+0.9+0.5)/3
    const CandidateSnapshot weak = PointAt(2, 5, 0, 0.f, 0.f);      // 0.5/3 < 0.3

    const CandidateSnapshot both[]{weak, winner};
    Harness h;
    CHECK(h.Run(Observer(), both, profile, 0) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire);
    CHECK(h.output.reason == DecisionReason::HighestScore);
    CHECK(h.output.selected == 1 && h.output.previous == 0);
    CHECK(test::Near(h.output.rawScore, 2.3 / 3.0, 1e-6));
    CHECK(h.output.rawScore == h.Find(1)->rawScore);
    CHECK(h.trace.leading == 1 && h.trace.leadingScore == h.output.rawScore);
    CHECK(h.trace.acquireThreshold == 0.3f);

    const CandidateSnapshot onlyWeak[]{weak};
    Harness below;
    CHECK(below.Run(Observer(), onlyWeak, profile, 0) == StepStatus::Ok);
    CHECK(below.output.kind == DecisionKind::None);
    CHECK(below.output.reason == DecisionReason::BelowAcquireThreshold);
    CHECK(below.output.selected == 0 && below.output.rawScore == 0.f);
    CHECK(below.trace.leading == 2);
    CHECK(test::Near(below.trace.leadingScore, 0.5 / 3.0, 1e-6));
    CHECK(below.memory.current == 0);

    // The threshold is inclusive. Attraction-only weights and a NonSpatial candidate
    // make the raw score exactly the attraction value.
    AttentionProfile exact = Profile(contexts);
    exact.salienceWeight = 0.f;
    exact.affinityWeight = 0.f;
    exact.angleFloor = 0.f;
    const CandidateSnapshot atThreshold[]{NonSpatial(5, exact.acquireThreshold, 0.f)};
    Harness at;
    CHECK(at.Run(Observer(), atThreshold, exact, 0) == StepStatus::Ok);
    CHECK(at.trace.leadingScore == exact.acquireThreshold);
    CHECK(at.output.kind == DecisionKind::Acquire && at.output.selected == 5);

    const CandidateSnapshot justBelow[]{
        NonSpatial(5, std::nextafter(exact.acquireThreshold, 0.f), 0.f)};
    Harness under;
    CHECK(under.Run(Observer(), justBelow, exact, 0) == StepStatus::Ok);
    CHECK(under.output.kind == DecisionKind::None);
    CHECK(under.output.reason == DecisionReason::BelowAcquireThreshold);
}

void C04_PermutationsAndExactTies()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);

    CandidateSnapshot bearing = NonSpatial(6, 0.9f, 0.9f);
    bearing.spatial = SpatialKind::Bearing;
    bearing.bearing = {0.f, 1.f, 0.f};
    CandidateSnapshot disabled = NonSpatial(3, 1.f, 1.f);
    disabled.enabled = false;
    // Ids 9 and 4 tie exactly at the top score (identical inputs, identical path).
    std::vector<CandidateSnapshot> candidates{
        NonSpatial(9, 0.8f, 0.8f), NonSpatial(4, 0.8f, 0.8f), PointAt(2, 5, 3, 0.7f, 0.7f),
        bearing, PointAt(11, 15, -2, 1.f, 1.f), disabled};

    Harness reference;
    CHECK(reference.Run(Observer(), candidates, profile, 0) == StepStatus::Ok);
    CHECK(reference.output.selected == 4);
    CHECK(reference.Find(9)->rawScore == reference.Find(4)->rawScore);

    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    int permutations = 0;
    bool allSame = true;
    do {
        std::vector<CandidateSnapshot> permuted;
        for (std::size_t from : order) {
            permuted.push_back(candidates[from]);
        }
        Harness h;
        if (h.Run(Observer(), permuted, profile, 0) != StepStatus::Ok ||
            !test::SameDecision(h.output, reference.output) ||
            !test::SameMemory(h.memory, reference.memory) ||
            h.trace.leading != reference.trace.leading) {
            allSame = false;
        }
        for (std::size_t i = 0; i < permuted.size(); ++i) {
            const interest::CandidateTrace& entry = h.trace.candidates[i];
            if (entry.id != permuted[i].id ||
                !test::SameEntry(entry, *reference.Find(entry.id))) {
                allSame = false;
            }
        }
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 720);
    CHECK(allSame);
}

void C05_StaticAndLiveUpdatedSnapshots()
{
    ContextPreference neutral{};
    ContextPreference likesB{};
    likesB.affinityTags = Tag(1);
    likesB.tagAffinity[1] = 1.f;
    likesB.fallbackAffinity = 0.f;
    const ContextPreference contexts[2]{neutral, likesB};
    const AttentionProfile profile = Profile(contexts);

    CandidateSnapshot a = PointAt(1, 5, 0, 0.6f, 0.2f);
    a.tags = Tag(0);
    CandidateSnapshot b = PointAt(2, 5, 1, 0.6f, 0.2f);  // slightly off-axis
    b.tags = Tag(1);
    Harness h;

    CandidateSnapshot step1[]{a, b};
    CHECK(h.Run(Observer(0), step1, profile, 0) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire && h.output.selected == 1);

    b.salience = 1.f;  // host submits B's new salience in the next snapshot
    CandidateSnapshot step2[]{a, b};
    CHECK(h.Run(Observer(0), step2, profile, 100) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Switch);
    CHECK(h.output.selected == 2 && h.output.previous == 1);

    b.salience = 0.2f;
    CandidateSnapshot step3[]{a, b};
    CHECK(h.Run(Observer(0), step3, profile, 200) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Switch && h.output.selected == 1);

    // Same candidates, the observer's context changes.
    CHECK(h.Run(Observer(1), step3, profile, 300) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Switch && h.output.selected == 2);
    CHECK(h.Find(1)->affinity == 0.f && h.Find(2)->affinity == 1.f);

    CHECK(h.Run(Observer(0), step3, profile, 400) == StepStatus::Ok);
    CHECK(h.output.selected == 1);

    // A candidate omitted from the snapshot is no longer eligible.
    CandidateSnapshot step6[]{b};
    CHECK(h.Run(Observer(0), step6, profile, 500) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Switch && h.output.selected == 2);
}

void C06_IdenticalInputsStateAndTime()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    const CandidateSnapshot candidates[]{PointAt(1, 5, 0, 0.7f, 0.4f),
                                         PointAt(2, 9, 6, 0.9f, 0.9f),
                                         NonSpatial(3, 0.2f, 0.3f)};
    Harness h;
    CHECK(h.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);

    Harness first = h;
    Harness second = h;
    CHECK(first.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);
    CHECK(second.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);
    CHECK(test::SameDecision(first.output, second.output));
    CHECK(test::SameMemory(first.memory, second.memory));
    CHECK(first.trace.candidates.size() == second.trace.candidates.size());
    for (std::size_t i = 0; i < first.trace.candidates.size(); ++i) {
        CHECK(test::SameEntry(first.trace.candidates[i], second.trace.candidates[i]));
    }
    // A repeated step at an equal timestamp is valid and changes nothing.
    CHECK(first.output.kind == DecisionKind::Keep);
    CHECK(test::SameMemory(first.memory, h.memory));

    // Tracing does not change the decision or the next memory.
    ObserverMemory memory = h.memory;
    interest::StepScratch scratch;
    interest::Reserve(scratch, 3);
    Decision untraced{};
    CHECK(interest::Step(Observer(), candidates, profile, 100, memory, scratch, untraced) ==
          StepStatus::Ok);
    CHECK(test::SameDecision(untraced, first.output));
    CHECK(test::SameMemory(memory, first.memory));
}

void C07_InvalidTimeObserverAndProfile()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    const CandidateSnapshot candidates[]{PointAt(1, 5, 0, 0.9f, 0.9f)};

    Harness h;
    CHECK(h.Run(Observer(), candidates, profile, -50) == StepStatus::Ok);  // any first time
    CHECK(h.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);
    const ObserverMemory before = h.memory;

    // Backward time.
    FillSentinels(h);
    CHECK(h.Run(Observer(), candidates, profile, 99) == StepStatus::TimeWentBackwards);
    CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));

    // Invalid observers.
    const auto observer = [](auto change) {
        ObserverSnapshot o = Observer();
        change(o);
        return o;
    };
    using EditObserver = ObserverSnapshot&;
    const std::vector<ObserverSnapshot> badObservers{
        observer([](EditObserver o) { o.position.x = kNaND; }),
        observer([](EditObserver o) { o.position.z = -kInfD; }),
        observer([](EditObserver o) { o.referenceForward = {0.f, 0.f, 0.f}; }),
        observer([](EditObserver o) { o.referenceForward = {kNaN, 1.f, 0.f}; }),
        observer([](EditObserver o) { o.referenceForward = {kInf, 0.f, 0.f}; }),
        observer([](EditObserver o) { o.context = 1; }),  // outside the profile's contexts
    };
    for (const ObserverSnapshot& bad : badObservers) {
        CHECK(h.Run(bad, candidates, profile, 200) == StepStatus::InvalidObserver);
        CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));
    }

    // Invalid profiles: ValidateProfile and Step agree, nothing is mutated.
    std::vector<AttentionProfile> badProfiles;
    const auto addBadProfile = [&](auto mutate) {
        AttentionProfile p = profile;
        mutate(p);
        badProfiles.push_back(p);
    };
    addBadProfile([](AttentionProfile& p) { p.attractionWeight = -1.f; });
    addBadProfile([](AttentionProfile& p) { p.salienceWeight = kNaN; });
    addBadProfile([](AttentionProfile& p) { p.affinityWeight = kInf; });
    addBadProfile([](AttentionProfile& p) {
        p.attractionWeight = 0.f;
        p.salienceWeight = 0.f;
        p.affinityWeight = 0.f;
    });
    addBadProfile([](AttentionProfile& p) { p.fullRange = -1.f; });
    addBadProfile([](AttentionProfile& p) { p.fullRange = kNaN; });
    addBadProfile([](AttentionProfile& p) { p.maxRange = 0.f; });
    addBadProfile([](AttentionProfile& p) { p.maxRange = -5.f; });
    addBadProfile([](AttentionProfile& p) { p.maxRange = kInf; });
    addBadProfile([](AttentionProfile& p) { p.angleFloor = 1.5f; });
    addBadProfile([](AttentionProfile& p) { p.angleFloor = -0.1f; });
    addBadProfile([](AttentionProfile& p) { p.angleSharpness = -1.f; });
    addBadProfile([](AttentionProfile& p) { p.angleSharpness = kInf; });
    addBadProfile([](AttentionProfile& p) { p.acquireThreshold = 0.f; });
    addBadProfile([](AttentionProfile& p) { p.acquireThreshold = 1.5f; });
    addBadProfile([](AttentionProfile& p) { p.acquireThreshold = kNaN; });
    // Temporal policy.
    addBadProfile([](AttentionProfile& p) { p.releaseThreshold = 0.f; });
    addBadProfile([](AttentionProfile& p) { p.releaseThreshold = -0.1f; });
    addBadProfile([](AttentionProfile& p) { p.releaseThreshold = kNaN; });
    addBadProfile([](AttentionProfile& p) { p.releaseThreshold = std::nextafter(0.3f, 1.f); });
    addBadProfile([](AttentionProfile& p) {  // above an otherwise valid acquire threshold
        p.acquireThreshold = 0.4f;
        p.releaseThreshold = 0.5f;
    });
    addBadProfile([](AttentionProfile& p) { p.switchMargin = 0.f; });
    addBadProfile([](AttentionProfile& p) { p.switchMargin = -0.1f; });
    addBadProfile([](AttentionProfile& p) { p.switchMargin = kNaN; });
    addBadProfile([](AttentionProfile& p) {
        p.switchMargin = 1.5f;
        p.overrideMargin = 1.5f;
    });
    addBadProfile([](AttentionProfile& p) {  // override below the switch margin
        p.switchMargin = 0.2f;
        p.overrideMargin = 0.1f;
    });
    addBadProfile([](AttentionProfile& p) { p.overrideMargin = 1.5f; });
    addBadProfile([](AttentionProfile& p) { p.overrideMargin = kInf; });
    addBadProfile([](AttentionProfile& p) { p.minDwellMs = -1; });
    addBadProfile([](AttentionProfile& p) { p.contexts = {}; });
    ContextPreference overlapping{};
    overlapping.requiredTags = Tag(3);
    overlapping.excludedTags = Tag(3) | Tag(4);
    ContextPreference badFallback{};
    badFallback.fallbackAffinity = 1.5f;
    ContextPreference badAffinity{};
    badAffinity.tagAffinity[63] = kNaN;  // in an entry no observer uses yet
    const ContextPreference overlapContexts[1]{overlapping};
    const ContextPreference fallbackContexts[1]{badFallback};
    const ContextPreference affinityContexts[2]{ContextPreference{}, badAffinity};
    addBadProfile([&](AttentionProfile& p) { p.contexts = overlapContexts; });
    addBadProfile([&](AttentionProfile& p) { p.contexts = fallbackContexts; });
    addBadProfile([&](AttentionProfile& p) { p.contexts = affinityContexts; });

    CHECK(interest::ValidateProfile(profile) == StepStatus::Ok);
    for (const AttentionProfile& bad : badProfiles) {
        CHECK(interest::ValidateProfile(bad) == StepStatus::InvalidProfile);
        CHECK(h.Run(Observer(), candidates, bad, 200) == StepStatus::InvalidProfile);
        CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));
    }

    // Validation order is profile, observer, time.
    CHECK(h.Run(badObservers[0], candidates, badProfiles[0], 0) == StepStatus::InvalidProfile);
    CHECK(h.Run(badObservers[0], candidates, profile, 0) == StepStatus::InvalidObserver);
    CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));

    // After the failures the memory still accepts a valid step, and a reset forgets time.
    CHECK(h.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);
    interest::ResetMemory(h.memory);
    CHECK(h.memory.current == 0 && !h.memory.hasStepped);
    CHECK(h.Run(Observer(), candidates, profile, 0) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire);
}

// Step never grows caller buffers: too little prepared capacity is a non-mutating error.
void CapacityContract()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    const CandidateSnapshot candidates[]{PointAt(1, 5, 0, 0.9f, 0.9f), NonSpatial(2, 0.5f, 0.5f),
                                         NonSpatial(3, 0.4f, 0.4f)};
    const CandidateSnapshot fewer[]{NonSpatial(2, 0.5f, 0.5f)};

    Harness h;
    CHECK(h.Run(Observer(), candidates, profile, 100) == StepStatus::Ok);  // reserves 3
    const ObserverMemory before = h.memory;

    interest::StepScratch unprepared;
    DecisionTrace unpreparedTrace;
    FillSentinels(h);
    const auto step = [&](interest::StepScratch& scratch, DecisionTrace* trace,
                          interest::TimeMs now) {
        return interest::Step(Observer(), candidates, profile, now, h.memory, scratch,
                              h.output, trace);
    };
    // Neither buffer, or only one of them, prepared.
    CHECK(step(unprepared, &h.trace, 200) == StepStatus::InsufficientCapacity);
    CHECK(step(h.scratch, &unpreparedTrace, 200) == StepStatus::InsufficientCapacity);
    CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));
    CHECK(unprepared.sortedIds.capacity() == 0 && unpreparedTrace.candidates.capacity() == 0);

    // Partially prepared scratch.
    interest::StepScratch tooSmall;
    interest::Reserve(tooSmall, 2);
    CHECK(step(tooSmall, nullptr, 200) == StepStatus::InsufficientCapacity);
    CHECK(test::SameMemory(h.memory, before) && SentinelsIntact(h));

    // Earlier validation failures take precedence over capacity.
    CHECK(step(unprepared, &h.trace, 99) == StepStatus::TimeWentBackwards);
    CHECK(SentinelsIntact(h));

    // No candidates need no capacity; no trace needs no trace capacity.
    CHECK(interest::Step(Observer(), {}, profile, 200, h.memory, unprepared, h.output) ==
          StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Clear);
    interest::StepScratch exact;
    interest::Reserve(exact, 3);
    CHECK(step(exact, nullptr, 300) == StepStatus::Ok);
    CHECK(h.output.kind == DecisionKind::Acquire && h.output.selected == 1);

    // Prepared buffers are reused as-is: capacity and storage never change.
    interest::StepScratch scratch;
    DecisionTrace trace;
    interest::Reserve(scratch, 3);
    interest::Reserve(trace, 3);
    interest::Reserve(scratch, 1);  // never shrinks
    const std::size_t scratchCapacity = scratch.sortedIds.capacity();
    const std::size_t traceCapacity = trace.candidates.capacity();
    const interest::CandidateId* scratchData = scratch.sortedIds.data();
    const interest::CandidateTrace* traceData = trace.candidates.data();
    bool stable = scratchCapacity >= 3 && traceCapacity >= 3;
    for (interest::TimeMs now = 400; now < 500; ++now) {
        const std::span<const CandidateSnapshot> input =
            now % 2 == 0 ? std::span<const CandidateSnapshot>(candidates)
                         : std::span<const CandidateSnapshot>(fewer);
        if (interest::Step(Observer(), input, profile, now, h.memory, scratch, h.output,
                           &trace) != StepStatus::Ok ||
            scratch.sortedIds.capacity() != scratchCapacity ||
            trace.candidates.capacity() != traceCapacity ||
            scratch.sortedIds.data() != scratchData || trace.candidates.data() != traceData ||
            trace.candidates.size() != input.size()) {
            stable = false;
        }
    }
    CHECK(stable);
}

void C07a_DuplicateCandidateIds()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    const CandidateSnapshot b = PointAt(2, 5, 0, 0.7f, 0.7f);
    const CandidateSnapshot c = PointAt(3, 7, 2, 0.5f, 0.5f);

    // Two entries share id 1; the strong one would otherwise win.
    const CandidateSnapshot pair[]{PointAt(1, 5, 0, 1.f, 1.f), b, PointAt(1, 9, 0, 0.1f, 0.1f),
                                   c};
    Harness h;
    CHECK(h.Run(Observer(), pair, profile, 0) == StepStatus::Ok);
    CHECK(h.trace.candidates[0].eligibility == Eligibility::DuplicateId);
    CHECK(h.trace.candidates[2].eligibility == Eligibility::DuplicateId);
    CHECK(h.trace.candidates[0].rawScore == 0.f && h.trace.candidates[2].rawScore == 0.f);
    CHECK(h.output.selected == 2);

    // Unrelated candidates are evaluated exactly as without the duplicates.
    const CandidateSnapshot unrelated[]{b, c};
    Harness clean;
    CHECK(clean.Run(Observer(), unrelated, profile, 0) == StepStatus::Ok);
    CHECK(test::SameEntry(*h.Find(2), *clean.Find(2)));
    CHECK(test::SameEntry(*h.Find(3), *clean.Find(3)));
    CHECK(test::SameDecision(h.output, clean.output));

    // Three occurrences, including a disabled one and one with an invalid value: all
    // are DuplicateId.
    CandidateSnapshot disabledCopy = PointAt(1, 5, 0, 1.f, 1.f);
    disabledCopy.enabled = false;
    CandidateSnapshot invalidCopy = PointAt(1, 5, 0, 1.f, 1.f);
    invalidCopy.attraction = kNaN;
    const CandidateSnapshot triple[]{disabledCopy, b, PointAt(1, 5, 0, 1.f, 1.f), invalidCopy};
    Harness three;
    CHECK(three.Run(Observer(), triple, profile, 0) == StepStatus::Ok);
    CHECK(three.trace.candidates[0].eligibility == Eligibility::DuplicateId);
    CHECK(three.trace.candidates[2].eligibility == Eligibility::DuplicateId);
    CHECK(three.trace.candidates[3].eligibility == Eligibility::DuplicateId);
    CHECK(three.output.selected == 2);

    // A 3-way and a 2-way duplicate group in every order (7! = 5040 permutations).
    const std::vector<CandidateSnapshot> groups{
        PointAt(1, 5, 0, 1.f, 1.f), PointAt(1, 6, 0, 0.9f, 0.9f), NonSpatial(1, 1.f, 1.f),
        PointAt(5, 5, 0, 1.f, 1.f), NonSpatial(5, 0.95f, 0.95f), c, PointAt(4, 6, 1, 0.6f, 0.6f)};
    Harness groupReference;
    CHECK(groupReference.Run(Observer(), groups, profile, 0) == StepStatus::Ok);
    CHECK(groupReference.output.selected == 4);
    std::vector<std::size_t> order(groups.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    int permutations = 0;
    bool allSame = true;
    do {
        std::vector<CandidateSnapshot> permuted;
        for (std::size_t from : order) {
            permuted.push_back(groups[from]);
        }
        Harness run;
        if (run.Run(Observer(), permuted, profile, 0) != StepStatus::Ok ||
            !test::SameDecision(run.output, groupReference.output)) {
            allSame = false;
        }
        for (const interest::CandidateTrace& entry : run.trace.candidates) {
            const bool duplicated = entry.id == 1 || entry.id == 5;
            if (duplicated != (entry.eligibility == Eligibility::DuplicateId) ||
                !test::SameEntry(entry, *groupReference.Find(entry.id))) {
                allSame = false;
            }
        }
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 5040);
    CHECK(allSame);

    // A selected candidate whose id becomes duplicated is no longer selectable.
    Harness selected;
    const CandidateSnapshot first[]{PointAt(1, 5, 0, 1.f, 1.f), b};
    CHECK(selected.Run(Observer(), first, profile, 0) == StepStatus::Ok);
    CHECK(selected.output.selected == 1);
    const CandidateSnapshot second[]{PointAt(1, 5, 0, 1.f, 1.f), b, PointAt(1, 5, 0, 1.f, 1.f)};
    CHECK(selected.Run(Observer(), second, profile, 100) == StepStatus::Ok);
    CHECK(selected.output.kind == DecisionKind::Switch && selected.output.selected == 2);

    // Repeated id 0 entries are reported as InvalidId, not DuplicateId.
    const CandidateSnapshot zeros[]{NonSpatial(0, 1.f, 1.f), NonSpatial(0, 1.f, 1.f)};
    Harness zero;
    CHECK(zero.Run(Observer(), zeros, profile, 0) == StepStatus::Ok);
    CHECK(zero.trace.candidates[0].eligibility == Eligibility::InvalidId);
    CHECK(zero.trace.candidates[1].eligibility == Eligibility::InvalidId);
}

void C07b_GazeDoesNotFeedBackIntoAngleScore()
{
    const ContextPreference contexts[1]{};
    AttentionProfile profile = Profile(contexts);
    profile.angleSharpness = 2.f;  // make the angle term matter

    // Body forward +x. A is 36.9 degrees left, B 53.1 degrees right but more appealing.
    const CandidateSnapshot a = PointAt(1, 8, 6, 0.5f, 0.5f);
    const CandidateSnapshot b = PointAt(2, 6, -8, 0.8f, 0.8f);
    const CandidateSnapshot candidates[]{a, b};
    const ObserverSnapshot body = Observer();

    Harness h;
    CHECK(h.Run(body, candidates, profile, 0) == StepStatus::Ok);
    CHECK(h.output.selected == 2);
    const DecisionTrace first = h.trace;

    // Being selected gives B no later angle advantage: Step has no gaze input and derives
    // none from its previous selection. However the host's head turns toward B, the
    // snapshot keeps the body baseline and every score stays bitwise identical.
    for (interest::TimeMs now = 100; now <= 400; now += 100) {
        CHECK(h.Run(body, candidates, profile, now) == StepStatus::Ok);
        CHECK(h.output.kind == DecisionKind::Keep && h.output.selected == 2);
        for (std::size_t i = 0; i < first.candidates.size(); ++i) {
            CHECK(test::SameEntry(h.trace.candidates[i], first.candidates[i]));
        }
    }

    // Control: the forbidden wiring (gaze as referenceForward) lets whichever target the
    // head already looks at win, so the invariance above is not vacuous.
    ObserverSnapshot gazeAtA = body;
    gazeAtA.referenceForward = {0.8f, 0.6f, 0.f};
    Harness wiredA;
    CHECK(wiredA.Run(gazeAtA, candidates, profile, 0) == StepStatus::Ok);
    CHECK(wiredA.output.selected == 1);
    CHECK(test::Near(wiredA.Find(1)->angleGate, 1.0, 1e-6));
    CHECK(wiredA.Find(1)->angleGate > first.candidates[0].angleGate);

    ObserverSnapshot gazeAtB = body;
    gazeAtB.referenceForward = {0.6f, -0.8f, 0.f};
    Harness wiredB;
    CHECK(wiredB.Run(gazeAtB, candidates, profile, 0) == StepStatus::Ok);
    CHECK(wiredB.output.selected == 2);
    CHECK(wiredB.Find(2)->rawScore > first.candidates[1].rawScore);
}

void C16_ObserversInDifferentContexts()
{
    constexpr unsigned kShop = 0;
    constexpr unsigned kPerson = 1;
    ContextPreference shopper{};
    shopper.affinityTags = Tag(kShop);
    shopper.tagAffinity[kShop] = 1.f;
    shopper.fallbackAffinity = 0.2f;
    ContextPreference guard{};
    guard.affinityTags = Tag(kPerson);
    guard.tagAffinity[kPerson] = 1.f;
    guard.fallbackAffinity = 0.2f;
    guard.excludedTags = Tag(kShop);
    const ContextPreference contexts[2]{shopper, guard};
    const AttentionProfile profile = Profile(contexts);

    CandidateSnapshot display = PointAt(10, 6, 1, 0.5f, 0.3f);
    display.tags = Tag(kShop);
    CandidateSnapshot passerBy = PointAt(20, 6, -1, 0.5f, 0.3f);
    passerBy.tags = Tag(kPerson);
    const CandidateSnapshot candidates[]{display, passerBy};

    Harness shopperState;
    Harness guardState;
    CHECK(shopperState.Run(Observer(0), candidates, profile, 0) == StepStatus::Ok);
    CHECK(guardState.Run(Observer(1), candidates, profile, 0) == StepStatus::Ok);
    CHECK(shopperState.output.selected == 10);
    CHECK(guardState.output.selected == 20);
    CHECK(shopperState.Find(10)->affinity == 1.f && shopperState.Find(20)->affinity == 0.2f);
    CHECK(guardState.Find(10)->eligibility == Eligibility::ExcludedTag);
    CHECK(guardState.Find(20)->affinity == 1.f);
}

void DecisionLabelsAcrossSteps()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);
    Harness h;
    const auto expect = [&h](DecisionKind kind, DecisionReason reason,
                             interest::CandidateId selected, interest::CandidateId previous) {
        CHECK(h.output.kind == kind);
        CHECK(h.output.reason == reason);
        CHECK(h.output.selected == selected && h.output.previous == previous);
        CHECK(h.memory.current == selected);
    };

    const CandidateSnapshot low[]{NonSpatial(1, 0.f, 0.f)};
    const CandidateSnapshot one[]{NonSpatial(1, 0.9f, 0.9f), NonSpatial(2, 0.5f, 0.5f)};
    const CandidateSnapshot two[]{NonSpatial(1, 0.5f, 0.5f), NonSpatial(2, 0.9f, 0.9f)};

    CHECK(h.Run(Observer(), low, profile, 0) == StepStatus::Ok);
    expect(DecisionKind::None, DecisionReason::BelowAcquireThreshold, 0, 0);
    CHECK(h.Run(Observer(), one, profile, 100) == StepStatus::Ok);
    expect(DecisionKind::Acquire, DecisionReason::HighestScore, 1, 0);
    CHECK(h.Run(Observer(), one, profile, 200) == StepStatus::Ok);
    expect(DecisionKind::Keep, DecisionReason::HighestScore, 1, 1);
    // Switch and Clear name the policy rule: candidate 2 leads by more than the
    // switch margin, and then target 2 is absent from the snapshot.
    CHECK(h.Run(Observer(), two, profile, 300) == StepStatus::Ok);
    expect(DecisionKind::Switch, DecisionReason::BetterCandidate, 2, 1);
    CHECK(h.Run(Observer(), low, profile, 400) == StepStatus::Ok);
    expect(DecisionKind::Clear, DecisionReason::CurrentInvalid, 0, 2);
    CHECK(h.Run(Observer(), {}, profile, 500) == StepStatus::Ok);
    expect(DecisionKind::None, DecisionReason::NoEligibleCandidate, 0, 0);
}

} // namespace

int main()
{
    test::Run("C01 empty candidate list", C01_EmptyCandidateList);
    test::Run("C02 invalid, disabled and out-of-range candidates",
              C02_InvalidDisabledAndOutOfRangeCandidates);
    test::Run("C03 acquisition and no-target behavior", C03_AcquisitionAndNoTarget);
    test::Run("C04 candidate permutations and exact ties", C04_PermutationsAndExactTies);
    test::Run("C05 static and live-updated snapshots", C05_StaticAndLiveUpdatedSnapshots);
    test::Run("C06 identical inputs, state and time", C06_IdenticalInputsStateAndTime);
    test::Run("C07 invalid time, observer and profile", C07_InvalidTimeObserverAndProfile);
    test::Run("capacity contract", CapacityContract);
    test::Run("C07a duplicate candidate ids", C07a_DuplicateCandidateIds);
    test::Run("C07b gaze does not feed back into angle score",
              C07b_GazeDoesNotFeedBackIntoAngleScore);
    test::Run("C16 observers in different contexts", C16_ObserversInDifferentContexts);
    test::Run("decision labels across steps", DecisionLabelsAcrossSteps);
    return test::Finish("interest_scenario_test");
}
