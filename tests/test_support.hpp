// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Minimal dependency-free test support shared by the kernel test executables:
// failure-counting checks, named test cases and small snapshot builders.

#include "interest/attention.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace test {

struct Counters {
    int failures = 0;
    int cases = 0;
    const char* currentCase = "";
};

inline Counters& State()
{
    static Counters counters;
    return counters;
}

inline void Report(bool ok, const char* expression, const char* file, int line)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED [%s] %s:%d: %s\n", State().currentCase, file, line,
                     expression);
        ++State().failures;
    }
}

inline void Run(const char* name, void (*testCase)())
{
    State().currentCase = name;
    ++State().cases;
    const int before = State().failures;
    testCase();
    std::printf("%s %s\n", State().failures == before ? "PASS" : "FAIL", name);
}

inline int Finish(const char* suite)
{
    std::printf("%s: %d case(s), %d failed check(s)\n", suite, State().cases,
                State().failures);
    return State().failures == 0 ? 0 : 1;
}

inline bool Near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

inline bool Near(float a, double b, double tolerance)
{
    return Near(static_cast<double>(a), b, tolerance);
}

// Deterministic test generator (SplitMix64), after Sebastiano Vigna (2015).
// See THIRD_PARTY_NOTICES.md for the public-domain dedication and permission notice.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}

    std::uint64_t Next()
    {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, bound).
    std::uint64_t Below(std::uint64_t bound) { return Next() % bound; }

    // Uniform in [0, 1] on a 1/1024 grid, so values are exactly representable.
    float Unit() { return static_cast<float>(Below(1025)) / 1024.f; }

    template <typename T>
    void Shuffle(std::vector<T>& values)
    {
        for (std::size_t i = values.size(); i > 1; --i) {
            const auto j = static_cast<std::size_t>(Below(i));
            T temp = values[i - 1];
            values[i - 1] = values[j];
            values[j] = temp;
        }
    }

private:
    std::uint64_t state_;
};

// Caller-owned kernel state for one observer, as a host would keep it. Run() prepares
// capacity for the submitted candidates first, as the host's setup code would.
struct Harness {
    interest::ObserverMemory memory{};
    interest::StepScratch scratch{};
    interest::Decision output{};
    interest::DecisionTrace trace{};

    interest::StepStatus Run(const interest::ObserverSnapshot& observer,
                             std::span<const interest::CandidateSnapshot> candidates,
                             const interest::AttentionProfile& profile, interest::TimeMs now)
    {
        interest::Reserve(scratch, candidates.size());
        interest::Reserve(trace, candidates.size());
        return interest::Step(observer, candidates, profile, now, memory, scratch, output,
                              &trace);
    }

    const interest::CandidateTrace* Find(interest::CandidateId id) const
    {
        for (const interest::CandidateTrace& entry : trace.candidates) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }
};

inline interest::ObserverSnapshot Observer(interest::ContextId context = 0)
{
    interest::ObserverSnapshot observer{};
    observer.id = 1;
    observer.referenceForward = interest::Vec3f{1.f, 0.f, 0.f};
    observer.context = context;
    return observer;
}

// fullRange 10, maxRange 20, angle floor 0.2, equal weights, acquire at 0.3.
// The temporal policy is reduced to its minimum for the scoring scenarios: no release
// band, no dwell, and a 0.001 margin, below their smallest deliberate score difference.
// The temporal tests configure their own policy.
inline interest::AttentionProfile Profile(std::span<const interest::ContextPreference> contexts)
{
    interest::AttentionProfile profile{};
    profile.fullRange = 10.f;
    profile.maxRange = 20.f;
    profile.angleFloor = 0.2f;
    profile.angleSharpness = 1.f;
    profile.acquireThreshold = 0.3f;
    profile.releaseThreshold = 0.3f;
    profile.switchMargin = 0.001f;
    profile.overrideMargin = 0.001f;
    profile.minDwellMs = 0;
    profile.contexts = contexts;
    return profile;
}

// Temporal-policy profile: acquire 0.5, release 0.375, switch margin 0.125, override
// margin 0.25, dwell 500 ms. Attraction-only weights make a NonSpatial candidate's raw
// score exactly its attraction (see Scored), and the dyadic values make every threshold
// and margin comparison exact.
inline interest::AttentionProfile TemporalProfile(
    std::span<const interest::ContextPreference> contexts)
{
    interest::AttentionProfile profile = Profile(contexts);
    profile.salienceWeight = 0.f;
    profile.affinityWeight = 0.f;
    profile.acquireThreshold = 0.5f;
    profile.releaseThreshold = 0.375f;
    profile.switchMargin = 0.125f;
    profile.overrideMargin = 0.25f;
    profile.minDwellMs = 500;
    return profile;
}

// Stateless selection of policy scores read from this step's trace. With novelty
// disabled this is B1; otherwise it is the effective-score argmax.
inline interest::CandidateId StatelessSelection(const interest::DecisionTrace& trace)
{
    return trace.leading != interest::kNoCandidate &&
                   trace.leadingScore >= trace.acquireThreshold
               ? trace.leading
               : interest::kNoCandidate;
}

inline interest::CandidateSnapshot PointAt(interest::CandidateId id, double x, double y,
                                           float attraction, float salience)
{
    interest::CandidateSnapshot candidate{};
    candidate.id = id;
    candidate.spatial = interest::SpatialKind::Point;
    candidate.position = interest::Vec3d{x, y, 0.0};
    candidate.attraction = attraction;
    candidate.salience = salience;
    return candidate;
}

inline interest::CandidateSnapshot NonSpatial(interest::CandidateId id, float attraction,
                                              float salience)
{
    interest::CandidateSnapshot candidate{};
    candidate.id = id;
    candidate.spatial = interest::SpatialKind::NonSpatial;
    candidate.attraction = attraction;
    candidate.salience = salience;
    return candidate;
}

// Under TemporalProfile, raw score == `score` exactly.
inline interest::CandidateSnapshot Scored(interest::CandidateId id, float score)
{
    return NonSpatial(id, score, 0.f);
}

inline bool SameDecision(const interest::Decision& a, const interest::Decision& b)
{
    return a.kind == b.kind && a.reason == b.reason && a.selected == b.selected &&
           a.previous == b.previous && a.rawScore == b.rawScore &&
           a.effectiveScore == b.effectiveScore;
}

inline bool SameMemory(const interest::ObserverMemory& a, const interest::ObserverMemory& b)
{
    return a.current == b.current && a.focusStartTime == b.focusStartTime &&
           a.lastStepTime == b.lastStepTime && a.hasStepped == b.hasStepped;
}

// The decision kinds each reason may label (see DecisionReason).
inline bool ReasonMatchesKind(interest::DecisionKind kind, interest::DecisionReason reason)
{
    using K = interest::DecisionKind;
    using R = interest::DecisionReason;
    switch (reason) {
    case R::NoEligibleCandidate:
    case R::BelowAcquireThreshold: return kind == K::None;
    case R::HighestScore: return kind == K::Acquire || kind == K::Keep;
    case R::CurrentInvalid:
    case R::BelowReleaseThreshold: return kind == K::Switch || kind == K::Clear;
    case R::BetterCandidate:
    case R::DwellOverride: return kind == K::Switch;
    case R::MinimumDwell:
    case R::InsufficientAdvantage:
    case R::AboveReleaseThreshold: return kind == K::Keep;
    }
    return false;
}

// A Keep whose reason says the policy holds a target the effective argmax would not select.
inline bool IsHold(const interest::Decision& decision)
{
    using R = interest::DecisionReason;
    return decision.kind == interest::DecisionKind::Keep &&
           (decision.reason == R::MinimumDwell || decision.reason == R::InsufficientAdvantage ||
            decision.reason == R::AboveReleaseThreshold);
}

// Compares the step-level trace fields (not the per-candidate entries).
inline bool SamePolicyTrace(const interest::DecisionTrace& a, const interest::DecisionTrace& b)
{
    return a.leading == b.leading && a.leadingScore == b.leadingScore &&
           a.currentEligible == b.currentEligible && a.currentScore == b.currentScore &&
           a.challenger == b.challenger && a.challengerScore == b.challengerScore &&
           a.dwellRemainingMs == b.dwellRemainingMs && a.acquireThreshold == b.acquireThreshold &&
           a.releaseThreshold == b.releaseThreshold && a.switchMargin == b.switchMargin &&
           a.overrideMargin == b.overrideMargin;
}

inline bool SameEntry(const interest::CandidateTrace& a, const interest::CandidateTrace& b)
{
    // Bitwise-equal floats are expected: the same inputs take the same path.
    return a.id == b.id && a.eligibility == b.eligibility && a.distance == b.distance &&
           a.distanceGate == b.distanceGate && a.angleGate == b.angleGate &&
           a.gate == b.gate && a.affinity == b.affinity && a.preference == b.preference &&
           a.rawScore == b.rawScore &&
           a.effectiveScore == b.effectiveScore && a.eventTimeMs == b.eventTimeMs &&
           a.eventStrength == b.eventStrength &&
           a.eventNovelty == b.eventNovelty && a.noveltyContribution == b.noveltyContribution;
}

// Recomputes the policy inputs from the per-candidate entries and checks the decision
// against B1 and the rules of the policy. True when every rule holds.
inline bool ConsistentStep(const Harness& h, const interest::AttentionProfile& profile)
{
    const interest::Decision& d = h.output;
    const interest::DecisionTrace& t = h.trace;
    interest::CandidateId leading = 0;
    interest::CandidateId challenger = 0;
    float leadingScore = 0.f;
    float challengerScore = 0.f;
    float currentScore = 0.f;
    bool currentEligible = false;
    for (const interest::CandidateTrace& e : t.candidates) {
        if (e.eligibility != interest::Eligibility::Eligible) {
            continue;
        }
        if (leading == 0 || e.effectiveScore > leadingScore ||
            (e.effectiveScore == leadingScore && e.id < leading)) {
            leading = e.id;
            leadingScore = e.effectiveScore;
        }
        if (e.id == d.previous) {
            currentEligible = true;
            currentScore = e.effectiveScore;
        } else if (challenger == 0 || e.effectiveScore > challengerScore ||
                   (e.effectiveScore == challengerScore && e.id < challenger)) {
            challenger = e.id;
            challengerScore = e.effectiveScore;
        }
    }
    bool ok = leading == t.leading && leadingScore == t.leadingScore &&
              challenger == t.challenger && challengerScore == t.challengerScore &&
              currentEligible == t.currentEligible && currentScore == t.currentScore;
    ok = ok && ReasonMatchesKind(d.kind, d.reason);
    // The temporal policy departs from the effective argmax exactly on these holds.
    ok = ok && (d.selected != StatelessSelection(t)) == IsHold(d);

    const double lead = static_cast<double>(d.effectiveScore) - static_cast<double>(currentScore);
    switch (d.reason) {
    case interest::DecisionReason::MinimumDwell: ok = ok && t.dwellRemainingMs > 0; break;
    case interest::DecisionReason::DwellOverride:
        ok = ok && t.dwellRemainingMs > 0 && lead >= static_cast<double>(profile.overrideMargin);
        break;
    case interest::DecisionReason::BetterCandidate:
        ok = ok && t.dwellRemainingMs == 0 && lead >= static_cast<double>(profile.switchMargin);
        break;
    case interest::DecisionReason::BelowReleaseThreshold:
        ok = ok && t.dwellRemainingMs == 0 && currentScore < profile.releaseThreshold;
        break;
    case interest::DecisionReason::CurrentInvalid:
        ok = ok && d.previous != 0 && !currentEligible;
        break;
    default: break;
    }
    if (d.kind == interest::DecisionKind::Keep) {
        // Only an eligible target is kept, and below release only while dwell runs.
        ok = ok && currentEligible && d.effectiveScore == currentScore &&
             (currentScore >= profile.releaseThreshold ||
              d.reason == interest::DecisionReason::MinimumDwell);
    }
    if (d.kind == interest::DecisionKind::Acquire || d.kind == interest::DecisionKind::Switch) {
        ok = ok && d.effectiveScore >= profile.acquireThreshold;
    }
    return ok;
}

inline void ExpectDecision(const interest::Decision& decision, interest::DecisionKind kind,
                           interest::DecisionReason reason, interest::CandidateId selected,
                           const char* file, int line)
{
    const bool ok =
        decision.kind == kind && decision.reason == reason && decision.selected == selected;
    if (!ok) {
        std::fprintf(stderr, "  got %s/%s selected %llu, expected %s/%s selected %llu\n",
                     interest::ToString(decision.kind), interest::ToString(decision.reason),
                     static_cast<unsigned long long>(decision.selected),
                     interest::ToString(kind), interest::ToString(reason),
                     static_cast<unsigned long long>(selected));
    }
    Report(ok, "decision kind/reason/selected", file, line);
}

} // namespace test

#define CHECK(condition) ::test::Report((condition), #condition, __FILE__, __LINE__)

// CHECK_DECISION(harness, Keep, MinimumDwell, 1)
#define CHECK_DECISION(harness, kind, reason, selected)                                     \
    ::test::ExpectDecision((harness).output, ::interest::DecisionKind::kind,                \
                           ::interest::DecisionReason::reason, (selected), __FILE__, __LINE__)
