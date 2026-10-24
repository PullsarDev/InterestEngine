// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

#include "interest/attention.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace interest {
namespace {

struct Vec3 { double x, y, z; };

// Observer quantities shared by every candidate in one step.
struct ObserverFrame {
    Vec3 position;
    Vec3 forward;  // unit length
};

bool IsUnitInterval(float value) noexcept
{
    return std::isfinite(value) && value >= 0.f && value <= 1.f;
}

bool IsFiniteNonNegative(float value) noexcept
{
    return std::isfinite(value) && value >= 0.f;
}

bool IsFinite(const Vec3d& v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double Dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Rejects zero-length and non-finite vectors instead of guessing a direction.
bool TryNormalize(const Vec3f& v, Vec3& out) noexcept
{
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return false;
    }
    const Vec3 d{static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
    const double length = std::sqrt(Dot(d, d));
    if (!(length > 0.0)) {
        return false;
    }
    out = Vec3{d.x / length, d.y / length, d.z / length};
    return true;
}

bool IsValidSpatialKind(SpatialKind kind) noexcept
{
    return kind == SpatialKind::Point || kind == SpatialKind::Bearing ||
           kind == SpatialKind::NonSpatial;
}

bool IsValidContext(const ContextPreference& context) noexcept
{
    if ((context.requiredTags & context.excludedTags) != 0) {
        return false;  // no candidate could ever be eligible
    }
    if (!IsUnitInterval(context.fallbackAffinity)) {
        return false;
    }
    return std::all_of(context.tagAffinity.begin(), context.tagAffinity.end(), IsUnitInterval);
}

bool TryMakeObserverFrame(const ObserverSnapshot& observer, const AttentionProfile& profile,
                          ObserverFrame& out) noexcept
{
    if (!IsFinite(observer.position) ||
        static_cast<std::size_t>(observer.context) >= profile.contexts.size()) {
        return false;
    }
    Vec3 forward{};
    if (!TryNormalize(observer.referenceForward, forward)) {
        return false;
    }
    out = ObserverFrame{Vec3{observer.position.x, observer.position.y, observer.position.z},
                        forward};
    return true;
}

double SmoothStep(double edge0, double edge1, double x) noexcept
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double AngleGate(double cosPhi, const AttentionProfile& profile) noexcept
{
    // Rounding can push a dot product of unit vectors just outside [-1,1]; pow() of a
    // negative base would then be NaN.
    const double base = (1.0 + std::clamp(cosPhi, -1.0, 1.0)) * 0.5;
    const double angleFloor = static_cast<double>(profile.angleFloor);
    return angleFloor +
           (1.0 - angleFloor) * std::pow(base, static_cast<double>(profile.angleSharpness));
}

float ContextAffinity(TagMask tags, const ContextPreference& context) noexcept
{
    TagMask matched = tags & context.affinityTags;
    if (matched == 0) {
        return context.fallbackAffinity;
    }
    float affinity = 0.f;
    while (matched != 0) {
        const auto bit = static_cast<std::size_t>(std::countr_zero(matched));
        if (context.tagAffinity[bit] > affinity) {
            affinity = context.tagAffinity[bit];
        }
        matched &= matched - 1;  // clear the lowest set bit
    }
    return affinity;
}

bool HasInvalidValue(const CandidateSnapshot& candidate) noexcept
{
    if (!IsUnitInterval(candidate.attraction) || !IsUnitInterval(candidate.salience) ||
        !IsUnitInterval(candidate.perceptionStrength) ||
        !IsValidSpatialKind(candidate.spatial)) {
        return true;
    }
    return candidate.spatial == SpatialKind::Point &&
           !IsFiniteNonNegative(candidate.maxDistance);
}

// Fills `out` with the eligibility and score of one candidate. This is the only
// scoring path; the trace records exactly these values.
void EvaluateCandidate(const CandidateSnapshot& candidate, bool duplicate,
                       const ObserverFrame& observer, const AttentionProfile& profile,
                       const ContextPreference& context, TimeMs now, CandidateTrace& out) noexcept
{
    out = CandidateTrace{};
    out.id = candidate.id;
    out.attraction = candidate.attraction;
    out.salience = candidate.salience;
    out.perceptionStrength = candidate.perceptionStrength;
    out.eventTimeMs = candidate.eventTimeMs;
    out.eventStrength = candidate.eventStrength;

    const auto reject = [&out](Eligibility reason) noexcept { out.eligibility = reason; };

    if (candidate.id == kNoCandidate) {
        return reject(Eligibility::InvalidId);
    }
    if (duplicate) {
        return reject(Eligibility::DuplicateId);
    }
    if (HasInvalidValue(candidate)) {
        return reject(Eligibility::InvalidValue);
    }
    Vec3 bearing{};
    const bool geometryValid =
        candidate.spatial == SpatialKind::Point     ? IsFinite(candidate.position)
        : candidate.spatial == SpatialKind::Bearing ? TryNormalize(candidate.bearing, bearing)
                                                    : true;
    if (!geometryValid) {
        return reject(Eligibility::InvalidGeometry);
    }
    if (!IsUnitInterval(candidate.eventStrength) ||
        !IsUnitInterval(candidate.eventDirectionality) ||
        (candidate.eventStrength > 0.f && candidate.eventTimeMs > now)) {
        return reject(Eligibility::InvalidEvent);
    }
    if (!candidate.enabled) {
        return reject(Eligibility::Disabled);
    }
    if (candidate.perceptionStrength == 0.f) {
        return reject(Eligibility::NotPerceived);
    }
    if ((candidate.tags & context.requiredTags) != context.requiredTags) {
        return reject(Eligibility::MissingRequiredTag);
    }
    if ((candidate.tags & context.excludedTags) != 0) {
        return reject(Eligibility::ExcludedTag);
    }

    double distanceGate = 1.0;
    double cosPhi = 1.0;  // NonSpatial: no angle penalty
    if (candidate.spatial == SpatialKind::Point) {
        const Vec3 offset{candidate.position.x - observer.position.x,
                          candidate.position.y - observer.position.y,
                          candidate.position.z - observer.position.z};
        const double distance = std::sqrt(Dot(offset, offset));
        out.distance = distance;
        const double cutoff = static_cast<double>(
            candidate.maxDistance > 0.f ? candidate.maxDistance : profile.maxRange);
        if (distance > cutoff) {  // an overflowed (infinite) distance is out of range too
            return reject(Eligibility::OutOfRange);
        }
        const double fullRange = static_cast<double>(profile.fullRange);
        if (fullRange < cutoff) {
            distanceGate = 1.0 - SmoothStep(fullRange, cutoff, distance);
        }
        if (distance > 0.0) {  // a coincident point keeps cosPhi = 1
            cosPhi = Dot(offset, observer.forward) / distance;
        }
    } else if (candidate.spatial == SpatialKind::Bearing) {
        cosPhi = Dot(bearing, observer.forward);
    }

    const double angleGate = AngleGate(cosPhi, profile);
    const double gate =
        distanceGate * angleGate * static_cast<double>(candidate.perceptionStrength);

    const float affinity = ContextAffinity(candidate.tags, context);
    const double wAttraction = static_cast<double>(profile.attractionWeight);
    const double wSalience = static_cast<double>(profile.salienceWeight);
    const double wAffinity = static_cast<double>(profile.affinityWeight);
    const double preference =
        (wAttraction * static_cast<double>(candidate.attraction) +
         wSalience * static_cast<double>(candidate.salience) +
         wAffinity * static_cast<double>(affinity)) /
        (wAttraction + wSalience + wAffinity);

    out.eligibility = Eligibility::Eligible;
    out.distanceGate = static_cast<float>(distanceGate);
    out.angleGate = static_cast<float>(angleGate);
    out.gate = static_cast<float>(gate);
    // Reuse the legacy rounded gate exactly at the default. Recomputing from the
    // rounded trace factors would change novelty and possibly temporal decisions.
    out.noveltyGate = out.gate;
    if (candidate.eventDirectionality != 1.f) {
        const double noveltyAngleGate =
            1.0 - static_cast<double>(candidate.eventDirectionality) * (1.0 - angleGate);
        out.noveltyGate = static_cast<float>(
            distanceGate * noveltyAngleGate * static_cast<double>(candidate.perceptionStrength));
    }
    out.affinity = affinity;
    out.preference = static_cast<float>(preference);
    // Selection compares this float, so the trace shows exactly the compared value.
    out.rawScore = static_cast<float>(gate * preference);
}

DecisionKind ClassifyDecision(CandidateId previous, CandidateId selected) noexcept
{
    if (selected == kNoCandidate) {
        return previous == kNoCandidate ? DecisionKind::None : DecisionKind::Clear;
    }
    if (previous == kNoCandidate) {
        return DecisionKind::Acquire;
    }
    return previous == selected ? DecisionKind::Keep : DecisionKind::Switch;
}

// A candidate and its effective score; id 0 means none.
struct Ranked {
    CandidateId id{};
    float score{};
};

// Higher score first, then lower ID. Eligible IDs are unique and nonzero, so this is a
// strict order and the winner does not depend on candidate order.
bool Outranks(CandidateId id, float score, const Ranked& other) noexcept
{
    return other.id == kNoCandidate || score > other.score ||
           (score == other.score && id < other.id);
}

// What the temporal policy compares, gathered in the single evaluation pass.
struct PolicyInput {
    Ranked leading;                   // best eligible candidate
    Ranked challenger;                // best eligible candidate other than the current target
    bool currentEligible = false;
    float currentScore = 0.f;
    TimeMs dwellRemainingMs = 0;
};

struct Choice {
    CandidateId selected = kNoCandidate;
    float score = 0.f;
    DecisionReason reason = DecisionReason::NoEligibleCandidate;
};

TimeMs DwellRemaining(TimeMs now, TimeMs focusStartTime, TimeMs minDwellMs) noexcept
{
    // Memory written by Step always has focusStartTime <= now. A later focus start can
    // only come from caller-edited memory; it gets no dwell, rather than a wrapped elapsed
    // time that could look short.
    if (now < focusStartTime) {
        return 0;
    }
    // The unsigned difference is now the exact elapsed time across the whole int64 range,
    // where a signed subtraction could overflow.
    const std::uint64_t elapsed =
        static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(focusStartTime);
    const auto dwell = static_cast<std::uint64_t>(minDwellMs);
    return elapsed < dwell ? static_cast<TimeMs>(dwell - elapsed) : 0;
}

// Whether the challenger leads the current score by at least `margin`. The difference is
// taken in double, so with a positive margin an exact tie never passes, and a challenger
// that passes always scores strictly higher than the current target.
bool LeadsBy(const Ranked& challenger, float currentScore, float margin) noexcept
{
    return static_cast<double>(challenger.score) - static_cast<double>(currentScore) >=
           static_cast<double>(margin);
}

// The ordered temporal policy. It only ever selects the leading candidate, keeps
// the current target, or selects nothing.
Choice ApplyTemporalPolicy(CandidateId current, const PolicyInput& in,
                           const AttentionProfile& profile) noexcept
{
    const bool challengerQualifies = in.challenger.id != kNoCandidate &&
                                     in.challenger.score >= profile.acquireThreshold;
    const auto take = [](const Ranked& target, DecisionReason reason) noexcept {
        return Choice{target.id, target.score, reason};
    };
    const auto keep = [&](DecisionReason reason) noexcept {
        return Choice{current, in.currentScore, reason};
    };
    // Drops the current target: a qualifying challenger replaces it, or nothing does.
    const auto fallBack = [&](DecisionReason reason) noexcept {
        return challengerQualifies ? take(in.challenger, reason)
                                   : Choice{kNoCandidate, 0.f, reason};
    };

    if (current == kNoCandidate) {
        if (in.leading.id == kNoCandidate) {
            return Choice{kNoCandidate, 0.f, DecisionReason::NoEligibleCandidate};
        }
        if (in.leading.score < profile.acquireThreshold) {
            return Choice{kNoCandidate, 0.f, DecisionReason::BelowAcquireThreshold};
        }
        return take(in.leading, DecisionReason::HighestScore);
    }
    if (!in.currentEligible) {
        return fallBack(DecisionReason::CurrentInvalid);  // dwell never protects it
    }

    // What would happen without dwell.
    const bool belowRelease = in.currentScore < profile.releaseThreshold;
    const bool betterChallenger =
        challengerQualifies && LeadsBy(in.challenger, in.currentScore, profile.switchMargin);
    if (!belowRelease && !betterChallenger) {
        if (in.leading.id != current) {
            return keep(DecisionReason::InsufficientAdvantage);
        }
        return keep(in.currentScore < profile.acquireThreshold
                        ? DecisionReason::AboveReleaseThreshold
                        : DecisionReason::HighestScore);
    }
    if (in.dwellRemainingMs > 0) {
        // overrideMargin >= switchMargin, so an override is always also a better challenger.
        if (challengerQualifies &&
            LeadsBy(in.challenger, in.currentScore, profile.overrideMargin)) {
            return take(in.challenger, DecisionReason::DwellOverride);
        }
        return keep(DecisionReason::MinimumDwell);
    }
    return belowRelease ? fallBack(DecisionReason::BelowReleaseThreshold)
                        : take(in.challenger, DecisionReason::BetterCandidate);
}

} // namespace

StepStatus ValidateProfile(const AttentionProfile& profile) noexcept
{
    const bool weightsValid = IsFiniteNonNegative(profile.attractionWeight) &&
                              IsFiniteNonNegative(profile.salienceWeight) &&
                              IsFiniteNonNegative(profile.affinityWeight) &&
                              static_cast<double>(profile.attractionWeight) +
                                      static_cast<double>(profile.salienceWeight) +
                                      static_cast<double>(profile.affinityWeight) >
                                  0.0;
    const bool rangesValid = IsFiniteNonNegative(profile.fullRange) &&
                             std::isfinite(profile.maxRange) && profile.maxRange > 0.f;
    const bool angleValid = IsUnitInterval(profile.angleFloor) &&
                            IsFiniteNonNegative(profile.angleSharpness);
    // A zero threshold would let a zero-score candidate become a target.
    const bool thresholdValid = IsUnitInterval(profile.acquireThreshold) &&
                                profile.acquireThreshold > 0.f;
    // Likewise a zero release threshold would keep a zero-score target forever. Equal
    // thresholds are allowed: no hysteresis band.
    const bool releaseValid = std::isfinite(profile.releaseThreshold) &&
                              profile.releaseThreshold > 0.f &&
                              profile.releaseThreshold <= profile.acquireThreshold;
    const bool marginsValid = IsUnitInterval(profile.switchMargin) &&
                              profile.switchMargin > 0.f &&
                              IsUnitInterval(profile.overrideMargin) &&
                              profile.overrideMargin >= profile.switchMargin;
    if (!weightsValid || !rangesValid || !angleValid || !thresholdValid || !releaseValid ||
        !marginsValid || profile.minDwellMs < 0 || !IsUnitInterval(profile.noveltyBeta) ||
        profile.noveltyHalfLifeMs <= 0 || profile.contexts.empty() ||
        !std::all_of(profile.contexts.begin(), profile.contexts.end(), IsValidContext)) {
        return StepStatus::InvalidProfile;
    }
    return StepStatus::Ok;
}

StepStatus Step(const ObserverSnapshot& observer,
                std::span<const CandidateSnapshot> candidates,
                const AttentionProfile& profile,
                TimeMs now,
                ObserverMemory& memory,
                StepScratch& scratch,
                Decision& output,
                DecisionTrace* trace) noexcept
{
    if (ValidateProfile(profile) != StepStatus::Ok) {
        return StepStatus::InvalidProfile;
    }
    ObserverFrame frame{};
    if (!TryMakeObserverFrame(observer, profile, frame)) {
        return StepStatus::InvalidObserver;
    }
    if (memory.hasStepped && now < memory.lastStepTime) {
        return StepStatus::TimeWentBackwards;
    }
    // Within reserved capacity, clear/push_back/resize below cannot reallocate.
    if (scratch.sortedIds.capacity() < candidates.size() ||
        (trace != nullptr && trace->candidates.capacity() < candidates.size())) {
        return StepStatus::InsufficientCapacity;
    }
    const ContextPreference& context =
        profile.contexts[static_cast<std::size_t>(observer.context)];

    // Sorted IDs let every occurrence of a duplicated ID be found independently of order.
    scratch.sortedIds.clear();
    for (const CandidateSnapshot& candidate : candidates) {
        scratch.sortedIds.push_back(candidate.id);
    }
    std::sort(scratch.sortedIds.begin(), scratch.sortedIds.end());

    if (trace != nullptr) {
        trace->candidates.resize(candidates.size());
    }

    float leadingRaw = 0.f;
    float currentRaw = 0.f;

    const CandidateId current = memory.current;
    PolicyInput policy{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const CandidateSnapshot& candidate = candidates[i];
        const auto range =
            std::equal_range(scratch.sortedIds.begin(), scratch.sortedIds.end(), candidate.id);
        const bool duplicate = range.second - range.first > 1;

        CandidateTrace evaluation{};
        EvaluateCandidate(candidate, duplicate, frame, profile, context, now, evaluation);
        if (evaluation.eligibility == Eligibility::Eligible) {
            if (candidate.eventStrength > 0.f) {
                // Event time was validated <= now. Unsigned subtraction is the exact
                // age across the entire signed time range. No cache/arrival timestamp.
                const auto age = static_cast<std::uint64_t>(now) -
                                 static_cast<std::uint64_t>(candidate.eventTimeMs);
                evaluation.eventNovelty = static_cast<double>(candidate.eventStrength) *
                    std::exp2(-static_cast<double>(age) /
                              static_cast<double>(profile.noveltyHalfLifeMs));
            }
            evaluation.noveltyContribution = static_cast<double>(profile.noveltyBeta) *
                static_cast<double>(evaluation.noveltyGate) * evaluation.eventNovelty;
            // Deliberate utility saturation, not a repair of invalid input. Round
            // once so tiny score differences enter the temporal policy.
            evaluation.effectiveScore = static_cast<float>((std::min)(
                1.0, static_cast<double>(evaluation.rawScore) + evaluation.noveltyContribution));
        }
        if (trace != nullptr) {
            trace->candidates[i] = evaluation;
        }
        if (evaluation.eligibility != Eligibility::Eligible) {
            continue;
        }
        if (Outranks(evaluation.id, evaluation.effectiveScore, policy.leading)) {
            policy.leading = Ranked{evaluation.id, evaluation.effectiveScore};
            leadingRaw = evaluation.rawScore;
        }
        if (evaluation.id == current) {
            policy.currentEligible = true;
            policy.currentScore = evaluation.effectiveScore;
            currentRaw = evaluation.rawScore;
        } else if (Outranks(evaluation.id, evaluation.effectiveScore, policy.challenger)) {
            policy.challenger = Ranked{evaluation.id, evaluation.effectiveScore};
        }
    }
    if (current != kNoCandidate) {
        policy.dwellRemainingMs = DwellRemaining(now, memory.focusStartTime, profile.minDwellMs);
    }

    const Choice choice = ApplyTemporalPolicy(current, policy, profile);
    Decision decision{};
    decision.previous = current;
    decision.selected = choice.selected;
    decision.effectiveScore = choice.score;
    decision.rawScore = choice.selected == kNoCandidate ? 0.f
                      : choice.selected == current ? currentRaw : leadingRaw;
    decision.reason = choice.reason;
    decision.kind = ClassifyDecision(decision.previous, decision.selected);

    if (trace != nullptr) {
        trace->leading = policy.leading.id;
        trace->leadingScore = policy.leading.score;
        trace->currentEligible = policy.currentEligible;
        trace->currentScore = policy.currentScore;
        trace->challenger = policy.challenger.id;
        trace->challengerScore = policy.challenger.score;
        trace->dwellRemainingMs = policy.dwellRemainingMs;
        trace->acquireThreshold = profile.acquireThreshold;
        trace->releaseThreshold = profile.releaseThreshold;
        trace->switchMargin = profile.switchMargin;
        trace->overrideMargin = profile.overrideMargin;
    }
    if (decision.selected == kNoCandidate) {
        memory.focusStartTime = 0;
    } else if (decision.selected != current) {
        memory.focusStartTime = now;
    }
    memory.current = decision.selected;
    memory.lastStepTime = now;
    memory.hasStepped = true;
    output = decision;
    return StepStatus::Ok;
}

void ResetMemory(ObserverMemory& memory) noexcept
{
    memory = ObserverMemory{};
}

void Reserve(StepScratch& scratch, std::size_t candidateCapacity)
{
    scratch.sortedIds.reserve(candidateCapacity);
}

void Reserve(DecisionTrace& trace, std::size_t candidateCapacity)
{
    trace.candidates.reserve(candidateCapacity);
}

const char* ToString(StepStatus value) noexcept
{
    switch (value) {
    case StepStatus::Ok: return "Ok";
    case StepStatus::TimeWentBackwards: return "TimeWentBackwards";
    case StepStatus::InvalidObserver: return "InvalidObserver";
    case StepStatus::InvalidProfile: return "InvalidProfile";
    case StepStatus::InsufficientCapacity: return "InsufficientCapacity";
    }
    return "Unknown";
}

const char* ToString(DecisionKind value) noexcept
{
    switch (value) {
    case DecisionKind::None: return "None";
    case DecisionKind::Acquire: return "Acquire";
    case DecisionKind::Keep: return "Keep";
    case DecisionKind::Switch: return "Switch";
    case DecisionKind::Clear: return "Clear";
    }
    return "Unknown";
}

const char* ToString(DecisionReason value) noexcept
{
    switch (value) {
    case DecisionReason::NoEligibleCandidate: return "NoEligibleCandidate";
    case DecisionReason::BelowAcquireThreshold: return "BelowAcquireThreshold";
    case DecisionReason::HighestScore: return "HighestScore";
    case DecisionReason::CurrentInvalid: return "CurrentInvalid";
    case DecisionReason::BelowReleaseThreshold: return "BelowReleaseThreshold";
    case DecisionReason::BetterCandidate: return "BetterCandidate";
    case DecisionReason::DwellOverride: return "DwellOverride";
    case DecisionReason::MinimumDwell: return "MinimumDwell";
    case DecisionReason::InsufficientAdvantage: return "InsufficientAdvantage";
    case DecisionReason::AboveReleaseThreshold: return "AboveReleaseThreshold";
    }
    return "Unknown";
}

const char* ToString(Eligibility value) noexcept
{
    switch (value) {
    case Eligibility::Eligible: return "Eligible";
    case Eligibility::InvalidId: return "InvalidId";
    case Eligibility::DuplicateId: return "DuplicateId";
    case Eligibility::InvalidValue: return "InvalidValue";
    case Eligibility::InvalidGeometry: return "InvalidGeometry";
    case Eligibility::Disabled: return "Disabled";
    case Eligibility::NotPerceived: return "NotPerceived";
    case Eligibility::MissingRequiredTag: return "MissingRequiredTag";
    case Eligibility::ExcludedTag: return "ExcludedTag";
    case Eligibility::OutOfRange: return "OutOfRange";
    case Eligibility::InvalidEvent: return "InvalidEvent";
    }
    return "Unknown";
}

} // namespace interest
