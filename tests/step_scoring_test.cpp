// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Checks geometric gates and contextual weighted-mean scoring against explicit formulas.

#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using interest::AttentionProfile;
using interest::CandidateSnapshot;
using interest::CandidateTrace;
using interest::ContextPreference;
using interest::Eligibility;
using interest::ObserverSnapshot;
using interest::SpatialKind;
using interest::StepStatus;
using interest::TagMask;
using test::Harness;
using test::Near;
using test::NonSpatial;
using test::Observer;
using test::PointAt;
using test::Profile;

constexpr double kTolerance = 1e-6;

constexpr TagMask Tag(unsigned bit)
{
    return TagMask{1} << bit;
}

// Evaluates a single candidate and returns its trace entry.
CandidateTrace Evaluate(const CandidateSnapshot& candidate, const AttentionProfile& profile,
                        const ObserverSnapshot& observer = Observer())
{
    Harness h;
    const CandidateSnapshot candidates[]{candidate};
    if (h.Run(observer, candidates, profile, 0) != StepStatus::Ok ||
        h.trace.candidates.size() != 1) {
        return CandidateTrace{};
    }
    return h.trace.candidates[0];
}

double SmoothStep(double edge0, double edge1, double x)
{
    double t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

void PointScoreMatchesDesignFormula()
{
    ContextPreference context{};
    context.affinityTags = Tag(2);
    context.tagAffinity[2] = 0.8f;
    const ContextPreference contexts[1]{context};
    const AttentionProfile profile = Profile(contexts);

    CandidateSnapshot candidate = PointAt(1, 13, 4, 0.7f, 0.4f);
    candidate.perceptionStrength = 0.9f;
    candidate.tags = Tag(2);
    const CandidateTrace entry = Evaluate(candidate, profile);

    const double distance = std::sqrt(13.0 * 13.0 + 4.0 * 4.0);
    const double distanceGate = 1.0 - SmoothStep(10.0, 20.0, distance);
    const double cosPhi = 13.0 / distance;
    const double angleFloor = static_cast<double>(0.2f);
    const double angleGate = angleFloor + (1.0 - angleFloor) * ((1.0 + cosPhi) / 2.0);
    const double gate = distanceGate * angleGate * static_cast<double>(0.9f);
    const double preference =
        (static_cast<double>(0.7f) + static_cast<double>(0.4f) + static_cast<double>(0.8f)) /
        3.0;

    CHECK(entry.eligibility == Eligibility::Eligible);
    CHECK(Near(entry.distance, distance, 1e-12));
    CHECK(Near(entry.distanceGate, distanceGate, kTolerance));
    CHECK(Near(entry.angleGate, angleGate, kTolerance));
    CHECK(Near(entry.gate, gate, kTolerance));
    CHECK(entry.affinity == 0.8f);
    CHECK(Near(entry.preference, preference, kTolerance));
    CHECK(Near(entry.rawScore, gate * preference, kTolerance));
    CHECK(entry.attraction == 0.7f && entry.salience == 0.4f &&
          entry.perceptionStrength == 0.9f);
}

void DistanceGate()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);  // fullRange 10, maxRange 20

    CHECK(Evaluate(PointAt(1, 5, 0, 1.f, 1.f), profile).distanceGate == 1.f);
    CHECK(Evaluate(PointAt(1, 10, 0, 1.f, 1.f), profile).distanceGate == 1.f);
    CHECK(Near(Evaluate(PointAt(1, 15, 0, 1.f, 1.f), profile).distanceGate, 0.5, kTolerance));

    // At the cutoff the candidate is still eligible but scores 0; beyond it, rejected.
    const CandidateTrace atCutoff = Evaluate(PointAt(1, 20, 0, 1.f, 1.f), profile);
    CHECK(atCutoff.eligibility == Eligibility::Eligible);
    CHECK(atCutoff.distanceGate == 0.f && atCutoff.rawScore == 0.f);
    CHECK(Evaluate(PointAt(1, 20.001, 0, 1.f, 1.f), profile).eligibility ==
          Eligibility::OutOfRange);

    // A coincident point has no direction; it is treated as straight ahead.
    const CandidateTrace coincident = Evaluate(PointAt(1, 0, 0, 1.f, 1.f), profile);
    CHECK(coincident.distance == 0.0 && coincident.angleGate == 1.f);

    // fullRange >= cutoff: no falloff until the hard cutoff.
    AttentionProfile noFalloff = Profile(contexts);
    noFalloff.fullRange = 30.f;
    CHECK(Evaluate(PointAt(1, 19.9, 0, 1.f, 1.f), noFalloff).distanceGate == 1.f);
    CHECK(Evaluate(PointAt(1, 20.1, 0, 1.f, 1.f), noFalloff).eligibility ==
          Eligibility::OutOfRange);

    // A candidate's own maxDistance replaces the profile cutoff.
    CandidateSnapshot farReaching = PointAt(1, 30, 0, 1.f, 1.f);
    farReaching.maxDistance = 40.f;
    const CandidateTrace far = Evaluate(farReaching, profile);
    CHECK(far.eligibility == Eligibility::Eligible);
    CHECK(Near(far.distanceGate, 1.0 - SmoothStep(10.0, 40.0, 30.0), kTolerance));

    // Bearing and NonSpatial candidates have no distance penalty.
    CandidateSnapshot bearing = PointAt(2, 1000, 0, 1.f, 1.f);
    bearing.spatial = SpatialKind::Bearing;
    bearing.bearing = {1.f, 0.f, 0.f};
    CHECK(Evaluate(bearing, profile).eligibility == Eligibility::Eligible);
    CHECK(Evaluate(bearing, profile).distanceGate == 1.f);
    CandidateSnapshot nonSpatial = NonSpatial(3, 1.f, 1.f);
    nonSpatial.position = {1000.0, 0.0, 0.0};
    CHECK(Evaluate(nonSpatial, profile).distanceGate == 1.f);
}

void AngleGate()
{
    const ContextPreference contexts[1]{};
    const AttentionProfile profile = Profile(contexts);  // floor 0.2, sharpness 1

    CHECK(Near(Evaluate(PointAt(1, 5, 0, 1.f, 1.f), profile).angleGate, 1.0, kTolerance));
    CHECK(Near(Evaluate(PointAt(1, 0, 5, 1.f, 1.f), profile).angleGate, 0.6, kTolerance));
    // Behind the observer only the floor remains, so a perceived stimulus stays relevant.
    const CandidateTrace behind = Evaluate(PointAt(1, -5, 0, 1.f, 1.f), profile);
    CHECK(behind.eligibility == Eligibility::Eligible);
    CHECK(Near(behind.angleGate, 0.2, kTolerance));

    AttentionProfile sharp = Profile(contexts);
    sharp.angleSharpness = 2.f;
    CHECK(Near(Evaluate(PointAt(1, 0, 5, 1.f, 1.f), sharp).angleGate, 0.4, kTolerance));

    AttentionProfile flat = Profile(contexts);
    flat.angleSharpness = 0.f;  // no angular preference, even directly behind
    CHECK(Evaluate(PointAt(1, -5, 0, 1.f, 1.f), flat).angleGate == 1.f);

    AttentionProfile noFloor = Profile(contexts);
    noFloor.angleFloor = 0.f;
    const CandidateTrace hidden = Evaluate(PointAt(1, -5, 0, 1.f, 1.f), noFloor);
    CHECK(hidden.eligibility == Eligibility::Eligible && hidden.rawScore == 0.f);

    // Bearing directions and referenceForward may have any nonzero length.
    CandidateSnapshot bearing = NonSpatial(2, 1.f, 1.f);
    bearing.spatial = SpatialKind::Bearing;
    bearing.bearing = {0.f, 3.f, 0.f};
    CHECK(Near(Evaluate(bearing, profile).angleGate, 0.6, kTolerance));

    ObserverSnapshot unit = Observer();
    unit.referenceForward = {0.6f, 0.8f, 0.f};
    ObserverSnapshot scaled = Observer();
    scaled.referenceForward = {6.f, 8.f, 0.f};
    const CandidateSnapshot point = PointAt(3, 4, -7, 0.6f, 0.9f);
    CHECK(Near(Evaluate(point, profile, unit).rawScore,
               static_cast<double>(Evaluate(point, profile, scaled).rawScore), kTolerance));

    // NonSpatial: the gate is perception strength alone.
    CandidateSnapshot faint = NonSpatial(4, 1.f, 1.f);
    faint.perceptionStrength = 0.4f;
    const CandidateTrace faintEntry = Evaluate(faint, profile);
    CHECK(faintEntry.angleGate == 1.f && Near(faintEntry.gate, 0.4, kTolerance));
}

void ContextAffinity()
{
    ContextPreference context{};
    context.affinityTags = Tag(2) | Tag(5);
    context.tagAffinity[2] = 0.3f;
    context.tagAffinity[5] = 0.9f;
    context.tagAffinity[7] = 1.f;  // not in affinityTags, so never used
    context.fallbackAffinity = 0.1f;
    const ContextPreference contexts[1]{context};
    const AttentionProfile profile = Profile(contexts);

    const auto affinityFor = [&profile](TagMask tags) {
        CandidateSnapshot candidate = NonSpatial(1, 0.5f, 0.5f);
        candidate.tags = tags;
        return Evaluate(candidate, profile).affinity;
    };
    CHECK(affinityFor(Tag(2)) == 0.3f);
    CHECK(affinityFor(Tag(2) | Tag(5)) == 0.9f);  // maximum matching affinity
    CHECK(affinityFor(Tag(5) | Tag(7)) == 0.9f);
    CHECK(affinityFor(Tag(7)) == 0.1f);           // no tag with a defined affinity
    CHECK(affinityFor(0) == 0.1f);                // untagged
}

void HigherInputsNeverLowerRawScore()
{
    ContextPreference context{};
    context.affinityTags = Tag(0);
    const ContextPreference contexts[1]{context};
    const AttentionProfile profile = Profile(contexts);

    test::Random rng(0x5EED0001u);
    bool monotonic = true;
    for (int sample = 0; sample < 2000; ++sample) {
        // Draw values in a fixed order; argument evaluation order is unspecified.
        CandidateSnapshot base = PointAt(1, 0, 0, 0.f, 0.f);
        base.position.x = static_cast<double>(rng.Below(41)) - 20.0;
        base.position.y = static_cast<double>(rng.Below(41)) - 20.0;
        base.attraction = rng.Unit();
        base.salience = rng.Unit();
        base.perceptionStrength = rng.Unit();
        base.tags = Tag(0);
        ContextPreference lowContext = context;
        lowContext.tagAffinity[0] = rng.Unit();
        const ContextPreference low[1]{lowContext};
        AttentionProfile lowProfile = profile;
        lowProfile.contexts = low;
        const CandidateTrace baseEntry = Evaluate(base, lowProfile);
        if (baseEntry.eligibility != Eligibility::Eligible) {
            continue;
        }

        CandidateSnapshot more = base;
        switch (rng.Below(4)) {
        case 0: more.attraction = rng.Unit() * (1.f - base.attraction) + base.attraction; break;
        case 1: more.salience = rng.Unit() * (1.f - base.salience) + base.salience; break;
        case 2:
            more.perceptionStrength =
                rng.Unit() * (1.f - base.perceptionStrength) + base.perceptionStrength;
            break;
        default: {
            ContextPreference highContext = lowContext;
            highContext.tagAffinity[0] = 1.f;
            const ContextPreference high[1]{highContext};
            AttentionProfile highProfile = profile;
            highProfile.contexts = high;
            if (Evaluate(more, highProfile).rawScore < baseEntry.rawScore) {
                monotonic = false;
            }
            continue;
        }
        }
        if (Evaluate(more, lowProfile).rawScore < baseEntry.rawScore) {
            monotonic = false;
        }
    }
    CHECK(monotonic);
}

// Generates small scenes with deliberate id collisions, invalid values, disabled and
// unperceived candidates, all three spatial kinds and tag rules. Each scene must give the
// same decision and per-id trace for several orders, and satisfy the score invariants.
void GeneratedScenesArePermutationInvariant()
{
    ContextPreference picky{};
    picky.requiredTags = Tag(0);
    picky.excludedTags = Tag(3);
    picky.affinityTags = Tag(0) | Tag(1);
    picky.tagAffinity[0] = 0.4f;
    picky.tagAffinity[1] = 0.9f;
    ContextPreference unrestricted{};
    const ContextPreference contexts[2]{unrestricted, picky};

    test::Random rng(0x5EED0002u);
    int scenes = 0;
    int selections = 0;
    int duplicates = 0;
    bool invariant = true;
    for (; scenes < 3000; ++scenes) {
        AttentionProfile profile = Profile(contexts);
        profile.acquireThreshold = 0.05f + 0.5f * rng.Unit();
        profile.releaseThreshold = profile.acquireThreshold;  // may not exceed acquire
        profile.angleSharpness = 3.f * rng.Unit();
        ObserverSnapshot observer = Observer(static_cast<interest::ContextId>(rng.Below(2)));
        observer.referenceForward = {rng.Unit() - 0.5f, rng.Unit() - 0.5f, 0.1f};

        std::vector<CandidateSnapshot> candidates(static_cast<std::size_t>(rng.Below(10)));
        for (CandidateSnapshot& c : candidates) {
            c.id = rng.Below(13);  // 0..12: id 0 and collisions are common
            c.spatial = static_cast<SpatialKind>(rng.Below(3));
            c.position = {static_cast<double>(rng.Below(51)) - 25.0,
                          static_cast<double>(rng.Below(51)) - 25.0, 0.0};
            c.bearing = {rng.Unit() - 0.5f, rng.Unit() - 0.5f, 0.f};
            c.attraction = rng.Unit();
            c.salience = rng.Unit();
            c.perceptionStrength = rng.Below(8) == 0 ? 0.f : rng.Unit();
            c.maxDistance = rng.Below(4) == 0 ? 30.f : 0.f;
            c.tags = rng.Below(16);
            c.enabled = rng.Below(10) != 0;
            if (rng.Below(20) == 0) {
                c.salience = 2.f;  // invalid value
            }
        }

        Harness reference;
        if (reference.Run(observer, candidates, profile, 0) != StepStatus::Ok) {
            invariant = false;
            continue;
        }
        const interest::Decision& decision = reference.output;

        // Score invariants on the reference order.
        float bestScore = 0.f;
        interest::CandidateId best = 0;
        for (const CandidateTrace& entry : reference.trace.candidates) {
            if (entry.eligibility == Eligibility::DuplicateId) {
                ++duplicates;
            }
            if (entry.eligibility != Eligibility::Eligible) {
                if (entry.rawScore != 0.f ||
                    (decision.selected != 0 && entry.id == decision.selected)) {
                    invariant = false;
                }
                continue;
            }
            if (!std::isfinite(entry.rawScore) || entry.rawScore < 0.f || entry.rawScore > 1.f) {
                invariant = false;
            }
            if (best == 0 || entry.rawScore > bestScore ||
                (entry.rawScore == bestScore && entry.id < best)) {
                best = entry.id;
                bestScore = entry.rawScore;
            }
        }
        if (reference.trace.leading != best || reference.trace.leadingScore != bestScore) {
            invariant = false;
        }
        if (decision.selected != 0) {
            ++selections;
            if (decision.selected != best || decision.rawScore != bestScore ||
                decision.rawScore < profile.acquireThreshold) {
                invariant = false;
            }
        } else if (best != 0 && bestScore >= profile.acquireThreshold) {
            invariant = false;
        }

        // Same result for reversed, rotated and shuffled orders.
        for (int orderKind = 0; orderKind < 3; ++orderKind) {
            std::vector<CandidateSnapshot> permuted = candidates;
            if (orderKind == 0) {
                std::reverse(permuted.begin(), permuted.end());
            } else if (orderKind == 1 && !permuted.empty()) {
                std::rotate(permuted.begin(), permuted.begin() + 1, permuted.end());
            } else {
                rng.Shuffle(permuted);
            }
            Harness h;
            if (h.Run(observer, permuted, profile, 0) != StepStatus::Ok ||
                !test::SameDecision(h.output, decision) ||
                !test::SameMemory(h.memory, reference.memory) ||
                h.trace.leading != reference.trace.leading) {
                invariant = false;
                continue;
            }
            for (const CandidateTrace& entry : h.trace.candidates) {
                const CandidateTrace* original = reference.Find(entry.id);
                if (original == nullptr || !test::SameEntry(entry, *original)) {
                    invariant = false;
                }
            }
        }
    }
    CHECK(invariant);
    // Guard against a generator that never exercises the interesting paths.
    CHECK(selections > scenes / 10 && selections < scenes * 9 / 10);
    CHECK(duplicates > scenes / 2);
    std::printf("  generated scenes: %d, with selection: %d, duplicate entries: %d\n", scenes,
                selections, duplicates);
}

// The weighted mean is normalized, so only weight ratios matter. Scaling every weight by a
// power of two is exact in floating point, so scores and decisions must be bitwise equal.
void WeightScaleInvariance()
{
    const ContextPreference contexts[1]{};
    test::Random rng(0x5EED0004u);
    bool invariant = true;
    for (int scene = 0; scene < 500; ++scene) {
        AttentionProfile profile = Profile(contexts);
        profile.attractionWeight = rng.Unit();
        profile.salienceWeight = rng.Unit() + 0.25f;
        profile.affinityWeight = rng.Unit();
        AttentionProfile scaled = profile;
        scaled.attractionWeight *= 8.f;
        scaled.salienceWeight *= 8.f;
        scaled.affinityWeight *= 8.f;

        std::vector<CandidateSnapshot> candidates(static_cast<std::size_t>(rng.Below(6)));
        interest::CandidateId id = 1;
        for (CandidateSnapshot& c : candidates) {
            c = PointAt(id++, 0, 0, 0.f, 0.f);
            c.position.x = static_cast<double>(rng.Below(31)) - 10.0;
            c.position.y = static_cast<double>(rng.Below(31)) - 15.0;
            c.attraction = rng.Unit();
            c.salience = rng.Unit();
        }
        Harness a;
        Harness b;
        if (a.Run(Observer(), candidates, profile, 0) != StepStatus::Ok ||
            b.Run(Observer(), candidates, scaled, 0) != StepStatus::Ok ||
            !test::SameDecision(a.output, b.output)) {
            invariant = false;
            continue;
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (!test::SameEntry(a.trace.candidates[i], b.trace.candidates[i])) {
                invariant = false;
            }
        }
    }
    CHECK(invariant);
}

} // namespace

int main()
{
    test::Run("point score matches design formula", PointScoreMatchesDesignFormula);
    test::Run("distance gate", DistanceGate);
    test::Run("angle gate", AngleGate);
    test::Run("context affinity", ContextAffinity);
    test::Run("higher inputs never lower raw score", HigherInputsNeverLowerRawScore);
    test::Run("generated scenes are permutation invariant",
              GeneratedScenesArePermutationInvariant);
    test::Run("weight scale invariance", WeightScaleInvariance);
    return test::Finish("interest_scoring_test");
}
