// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Attention-selection kernel; see README.md for an integration overview.
//
// The host supplies plain value snapshots, a profile, explicit time and caller-owned
// memory/scratch; Step() returns one decision and, optionally, a trace of the same
// computation. Scoring is stateless. The temporal policy then decides
// whether to keep, replace or release the current target:
//   - No current target: acquire the leading candidate if it reaches acquireThreshold.
//   - Current target absent or ineligible: replace it with the leading candidate if
//     that reaches acquireThreshold, else clear. Dwell never protects it.
//   - During minimum dwell: keep the current target, unless a challenger reaches
//     acquireThreshold and leads it by overrideMargin.
//   - After dwell: release it below releaseThreshold (falling back to a challenger that
//     reaches acquireThreshold), or switch to a challenger that reaches acquireThreshold
//     and leads it by switchMargin. Otherwise keep it.
// Dwell is measured in host time and checked only at steps, so its resolution is the
// host's update interval. Optional spatially gated timestamped novelty is applied
// before that policy. No candidate or event history is retained by the kernel.
//
// Standard library only. Step is noexcept and uses no RTTI, global state, hidden clock or RNG.
//
// Capacity contract: Step never allocates. Before stepping, the caller prepares its
// StepScratch (and DecisionTrace, if one is passed) with Reserve() for the largest
// candidate count it will submit. A step with more candidates than the prepared capacity
// returns InsufficientCapacity and mutates nothing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace interest {

using ObserverId  = std::uint64_t;
using CandidateId = std::uint64_t;  // 0 = no candidate
using TimeMs      = std::int64_t;   // host-supplied, monotonic non-decreasing milliseconds
using TagMask     = std::uint64_t;  // host-defined tag bit mapping
using ContextId   = std::uint8_t;   // index into AttentionProfile::contexts

inline constexpr CandidateId kNoCandidate = 0;
inline constexpr std::size_t kTagCount = 64;  // one affinity entry per TagMask bit

struct Vec3d { double x{}, y{}, z{}; };
struct Vec3f { float  x{}, y{}, z{}; };

enum class SpatialKind : std::uint8_t { Point, Bearing, NonSpatial };

struct ObserverSnapshot {
    ObserverId id{};                  // host bookkeeping only; Step never reads it
    Vec3d position{};
    // Host-owned body/travel/attention baseline; any finite nonzero length (normalized
    // internally). Must NOT be the head/eye gaze driven by the selected target, which
    // would let the incumbent reinforce its own angle score.
    Vec3f referenceForward{1.f, 0.f, 0.f};
    ContextId context{};
};

struct CandidateSnapshot {
    CandidateId id{};                 // nonzero, unique within one Step call
    SpatialKind spatial{SpatialKind::Point};
    Vec3d position{};                 // Point only
    Vec3f bearing{};                  // Bearing only: world-space direction, any finite nonzero length
    float attraction{0.5f};           // [0,1] relatively persistent inherent appeal
    float salience{0.5f};             // [0,1] momentary perceptual prominence
    float perceptionStrength{1.f};    // [0,1]; 0 = not perceived
    float maxDistance{0.f};           // Point only; >= 0, 0 = profile maxRange
    TagMask tags{};
    bool enabled{true};
    // Latest host-authored event: immutable time/strength for an unchanged event.
    // Strength 0 means absent (time ignored). Positive strength requires time <= now.
    // Resubmit while relevant; omission removes the term. No event history is retained.
    TimeMs eventTimeMs{};
    float eventStrength{};            // finite [0,1], validated even when noveltyBeta == 0
    // Novelty's angular gating only; raw preference scoring is unchanged.
    // 1 preserves legacy gating, 0 ignores angle, intermediate values interpolate.
    // Finite [0,1], validated even for absent events or noveltyBeta == 0.
    // The host still controls perception strength, event generation and candidate range.
    float eventDirectionality{1.f};
};

// Per-context eligibility and preference. Selected by ObserverSnapshot::context.
struct ContextPreference {
    TagMask requiredTags{};           // candidate must carry all of these
    TagMask excludedTags{};           // candidate must carry none of these
    TagMask affinityTags{};           // tag bits whose tagAffinity entry applies
    std::array<float, kTagCount> tagAffinity{};  // [0,1], indexed by tag bit
    // [0,1] affinity for a candidate carrying no tag in affinityTags. This covers both
    // untagged candidates and candidates whose tags all lack a configured affinity.
    float fallbackAffinity{0.5f};
};

struct AttentionProfile {
    // Weighted-mean preference weights: finite, >= 0, with a positive sum.
    float attractionWeight{1.f};
    float salienceWeight{1.f};
    float affinityWeight{1.f};
    // Point distance gate in host distance units. Falloff runs from fullRange to the
    // cutoff (candidate maxDistance, or maxRange when that is 0); fullRange >= cutoff
    // means no falloff before the cutoff. maxRange has no default and must be set > 0.
    float fullRange{0.f};             // >= 0
    float maxRange{0.f};              // > 0
    float angleFloor{0.f};            // [0,1] angle gate for a stimulus directly behind
    float angleSharpness{1.f};        // >= 0
    float acquireThreshold{0.f};      // (0,1]; no default, must be set
    // Temporal policy. The thresholds and margins have no defaults and must be set.
    // (0, acquireThreshold]; once dwell ends, a current target scoring below this is
    // released. Equal to acquireThreshold means no hysteresis band.
    float releaseThreshold{0.f};
    // (0,1]; lead over the current target's score that a challenger needs to replace it.
    // Must be positive: with no margin, two exactly tied candidates would swap every step.
    float switchMargin{0.f};
    // [switchMargin,1]; lead that lets a challenger replace the target during dwell.
    float overrideMargin{0.f};
    // >= 0; after a target is selected, it is kept this long unless it becomes ineligible
    // or a challenger meets overrideMargin. 0 means no dwell.
    TimeMs minDwellMs{0};
    // Caller-owned; must outlive the Step call. At least one entry.
    std::span<const ContextPreference> contexts;
    // Novelty: effective = min(1, rawScore + beta * noveltyGate * eventNovelty).
    // noveltyGate retains distance/perception gating; eventDirectionality controls angle.
    // eventNovelty = eventStrength * 2^(-age / halfLife), from explicit host time.
    float noveltyBeta{0.f};           // [0,1]; 0 disables the contribution, not validation
    TimeMs noveltyHalfLifeMs{1000};    // strictly positive
};

enum class StepStatus : std::uint8_t {
    Ok,
    TimeWentBackwards,
    InvalidObserver,
    InvalidProfile,
    InsufficientCapacity,  // more candidates than the prepared scratch or trace capacity
};

enum class DecisionKind : std::uint8_t {
    None,     // nothing selected, nothing was selected before
    Acquire,  // nothing was selected before; a candidate is selected now
    Keep,     // the previously selected candidate is selected again
    Switch,   // a different candidate than the previous one is selected
    Clear,    // a candidate was selected before; nothing is selected now
};

// Why the policy reached its decision. The "current" target is the one selected by the
// previous successful step (Decision::previous). A challenger is the leading eligible
// candidate other than the current target. Every Keep reason except HighestScore marks
// a step where a stateless argmax of effective scores would have chosen differently.
enum class DecisionReason : std::uint8_t {
    NoEligibleCandidate,    // None: no current target and every candidate was rejected (or none)
    BelowAcquireThreshold,  // None: no current target; the leading candidate is below acquireThreshold
    HighestScore,           // Acquire, or Keep: the selected candidate leads and reaches acquireThreshold
    CurrentInvalid,         // Switch/Clear: the current target is absent or ineligible, even during dwell
    BelowReleaseThreshold,  // Switch/Clear: after dwell, the current target is below releaseThreshold
    BetterCandidate,        // Switch: after dwell, a challenger reaches acquireThreshold and leads by switchMargin
    DwellOverride,          // Switch: during dwell, a challenger reaches acquireThreshold and leads by overrideMargin
    MinimumDwell,           // Keep: dwell defers a release or switch that would otherwise happen
    InsufficientAdvantage,  // Keep: a challenger outranks the current target but does not qualify to replace it
    AboveReleaseThreshold,  // Keep: the current target leads, below acquireThreshold but not below releaseThreshold
};

struct Decision {
    DecisionKind kind{DecisionKind::None};
    DecisionReason reason{DecisionReason::NoEligibleCandidate};
    CandidateId selected{};           // 0 for None/Clear
    CandidateId previous{};           // selection of the previous successful step, 0 if none
    float rawScore{};                 // selected candidate's raw score; 0 when nothing is selected
    float effectiveScore{};           // [0,1] saturated raw + novelty score used by the temporal policy
};

// Caller-owned per-observer state. Changes only on a successful Step. Start from a
// value-initialized memory or ResetMemory() and leave updates to Step, which keeps
// focusStartTime <= lastStepTime while current != 0; when there is no current target,
// focusStartTime is reset to 0 (including if lastStepTime is negative). Step does
// not validate edited memory. The one edited state it defines is a focusStartTime later
// than `now`: the current target then has no dwell.
struct ObserverMemory {
    CandidateId current{};            // last selection, 0 if none
    TimeMs focusStartTime{};          // step time at which `current` was selected; 0 if none
    TimeMs lastStepTime{};
    bool hasStepped{false};
    // Temporal state only. Novelty is derived from the supplied event's absolute
    // age; ResetMemory does not make an old event new. No admission/eviction occurs.
};

// Caller-owned reusable workspace; contents are unspecified between calls. Prepare it
// with Reserve(); Step only uses the reserved capacity and never grows it. Copying a
// StepScratch does not preserve its capacity.
struct StepScratch {
    std::vector<CandidateId> sortedIds;
};

// First failing check wins in the order below, except InvalidEvent (appended to keep
// existing enum values) is checked between InvalidGeometry and Disabled.
enum class Eligibility : std::uint8_t {
    Eligible,
    InvalidId,            // id == 0
    DuplicateId,          // id occurs more than once in this Step's candidates (every occurrence)
    InvalidValue,         // non-finite/out-of-range scalar or unknown SpatialKind
    InvalidGeometry,      // non-finite Point position, or zero/non-finite Bearing
    Disabled,
    NotPerceived,         // perceptionStrength == 0
    MissingRequiredTag,
    ExcludedTag,
    OutOfRange,           // Point farther than its cutoff distance
    InvalidEvent,         // invalid strength/directionality or positive-strength future time
};

// One evaluated candidate. Factors are the values used for selection; those the
// evaluation did not reach are 0.
struct CandidateTrace {
    CandidateId id{};
    Eligibility eligibility{};
    float attraction{};               // input copy
    float salience{};                 // input copy
    float perceptionStrength{};       // input copy
    double distance{};                // Point only, once computed
    float distanceGate{};
    float angleGate{};
    float gate{};                     // distanceGate * angleGate * perceptionStrength
    float affinity{};                 // context affinity
    float preference{};               // weighted mean of attraction, salience, affinity
    float rawScore{};                 // gate * preference, base score
    float effectiveScore{};           // min(1, rawScore + noveltyContribution), as compared
    TimeMs eventTimeMs{};             // input copy, ignored if eventStrength == 0
    float eventStrength{};            // input copy, including rejected values
    // Rounded distanceGate * (1 - eventDirectionality * (1 - angleGate)) * perceptionStrength,
    // computed from unrounded geometric factors. At directionality 1, reuses gate exactly.
    // Placed here to reuse x64 alignment padding without moving pre-existing fields.
    float noveltyGate{};
    double eventNovelty{};            // strength * 2^(-age / halfLife), [0,1]; absent => 0
    double noveltyContribution{};     // beta * noveltyGate * eventNovelty, before saturation
};

// Optional caller-owned explanation of one successful Step; written only when Ok.
// Prepare it with Reserve() like StepScratch.
struct DecisionTrace {
    std::vector<CandidateTrace> candidates;  // one per input candidate, in input order
    CandidateId leading{};            // highest eligible effective score, ties to lower ID; 0 if none
    float leadingScore{};
    // Temporal policy inputs, as compared. The current target is Decision::previous.
    bool currentEligible{};           // the current target is present and eligible
    float currentScore{};             // its effective score; 0 when absent or ineligible
    CandidateId challenger{};         // leading eligible candidate other than the current target; 0 if none
    float challengerScore{};
    // Dwell left for the current target at this step; 0 when expired or there is none.
    // An ineligible current target is not protected even when this is positive.
    TimeMs dwellRemainingMs{};
    float acquireThreshold{};         // profile values the scores were compared against
    float releaseThreshold{};
    float switchMargin{};
    float overrideMargin{};
};

// Evaluates one observer. Validation order: profile, observer, time, capacity. On any
// status other than Ok, memory, output, scratch and trace are left untouched. Candidate-level problems
// never fail the step; they reject only that candidate (see Eligibility).
// Ties between equal effective scores go to the lower candidate ID, and the temporal policy
// compares only scores and IDs, so the result does not depend on candidate order. An
// exact tie never replaces the current target, because switchMargin is positive.
StepStatus Step(const ObserverSnapshot& observer,
                std::span<const CandidateSnapshot> candidates,
                const AttentionProfile& profile,
                TimeMs now,
                ObserverMemory& memory,
                StepScratch& scratch,
                Decision& output,
                DecisionTrace* trace = nullptr) noexcept;

void ResetMemory(ObserverMemory& memory) noexcept;

// Prepare caller-owned buffers for up to `candidateCapacity` candidates per Step. These
// allocate (and, where exceptions are enabled, may throw std::bad_alloc), so call them
// outside the per-step path. Capacity never shrinks.
void Reserve(StepScratch& scratch, std::size_t candidateCapacity);
void Reserve(DecisionTrace& trace, std::size_t candidateCapacity);

// Returns Ok or InvalidProfile; Step applies the same check.
StepStatus ValidateProfile(const AttentionProfile& profile) noexcept;

const char* ToString(StepStatus value) noexcept;
const char* ToString(DecisionKind value) noexcept;
const char* ToString(DecisionReason value) noexcept;
const char* ToString(Eligibility value) noexcept;

} // namespace interest
