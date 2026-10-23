// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Property coverage complements the detailed scoring, temporal and novelty scenarios.
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace {
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::Eligibility;
using interest::SpatialKind;
using interest::StepStatus;
using interest::TimeMs;
using test::Harness;
using test::Observer;
using test::Scored;

template <typename V>
V QuarterTurn(V v) { return V{-v.y, v.x, v.z}; }

void GeometryTransformsPreserveScores()
{
    const ContextPreference contexts[1]{};
    auto p = test::Profile(contexts);
    p.noveltyBeta = 0.75f;
    test::Random rng(0xC180101);
    for (int scene = 0; scene < 256; ++scene) {
        auto observer = Observer();
        observer.position = {2, -4, 8};
        observer.referenceForward = {0.5f, 0.25f, 0.125f};
        std::array<CandidateSnapshot, 3> frame{};
        for (std::size_t i = 0; i < frame.size(); ++i) {
            auto& c = frame[i];
            c = Scored(i + 1, rng.Unit());
            c.salience = rng.Unit(); c.perceptionStrength = rng.Unit();
            c.spatial = static_cast<SpatialKind>(i);
            c.position = {static_cast<double>(rng.Below(41)) - 20,
                          static_cast<double>(rng.Below(17)) - 8, 4};
            c.bearing = {rng.Unit(), -0.5f, 0.25f};
            c.eventTimeMs = -1000; c.eventStrength = rng.Unit();
            c.maxDistance = scene % 2 == 0 ? 16.f : 0.f;
        }
        Harness original;
        CHECK(original.Run(observer, frame, p, 0) == StepStatus::Ok);
        for (int transform = 0; transform < 4; ++transform) {
            auto moved = observer;
            auto changed = frame;
            auto profile = p;
            if (transform == 0) {
                moved.position.x += 256; moved.position.y -= 128; moved.position.z += 64;
                for (auto& c : changed) { c.position.x += 256; c.position.y -= 128; c.position.z += 64; }
            } else if (transform == 1) {
                moved.position = QuarterTurn(moved.position);
                moved.referenceForward = QuarterTurn(moved.referenceForward);
                for (auto& c : changed) { c.position = QuarterTurn(c.position); c.bearing = QuarterTurn(c.bearing); }
            } else if (transform == 3) {
                // Observer identity is bookkeeping only: variation must come from the
                // profile, never from the ID.
                moved.id = 0xFFFF'FFFF'0000'0000u | static_cast<interest::ObserverId>(scene + 1);
            } else {
                // A change of host distance units; angular directions stay fixed.
                moved.position.x *= 4; moved.position.y *= 4; moved.position.z *= 4;
                profile.fullRange *= 4; profile.maxRange *= 4;
                for (auto& c : changed) {
                    c.position.x *= 4; c.position.y *= 4; c.position.z *= 4; c.maxDistance *= 4;
                }
            }
            Harness transformed;
            CHECK(transformed.Run(moved, changed, profile, 0) == StepStatus::Ok);
            CHECK(test::SameDecision(original.output, transformed.output));
            CHECK(test::SameMemory(original.memory, transformed.memory));
            CHECK(test::SamePolicyTrace(original.trace, transformed.trace));
            for (std::size_t i = 0; i < frame.size(); ++i) {
                const auto& a = original.trace.candidates[i];
                const auto& b = transformed.trace.candidates[i];
                CHECK(a.eligibility == b.eligibility && a.distanceGate == b.distanceGate &&
                      a.angleGate == b.angleGate && a.gate == b.gate && a.rawScore == b.rawScore &&
                      a.noveltyContribution == b.noveltyContribution && a.effectiveScore == b.effectiveScore);
            }
        }
    }
}

void FiniteExtremesAndAdjacentCutoffs()
{
    const ContextPreference contexts[1]{};
    auto p = test::TemporalProfile(contexts);
    p.minDwellMs = 0; p.noveltyBeta = 1;
    p.angleFloor = 0; p.fullRange = 10; p.maxRange = 20;
    auto c = test::PointAt(1, 20, 0, 1.f, 1.f);
    c.eventStrength = 1;
    for (bool hardCutoff : {false, true}) {
        p.fullRange = hardCutoff ? 20.f : 10.f;
        for (double x : {std::nextafter(20.0, 0.0), 20.0, std::nextafter(20.0, 21.0)}) {
            c.position.x = x;
            Harness h;
            CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
            const auto& e = h.trace.candidates[0];
            CHECK(e.eligibility == (x > 20 ? Eligibility::OutOfRange : Eligibility::Eligible));
            if (x <= 20 && hardCutoff) { CHECK(e.gate == 1 && e.effectiveScore == 1); }
            if (x == 20 && !hardCutoff) { CHECK(e.gate == 0 && e.effectiveScore == 0); }
            CHECK(std::isfinite(e.rawScore) && std::isfinite(e.effectiveScore));
        }
    }
    // Finite vectors with the smallest/largest float magnitudes remain valid;
    // normalization happens in double. Extreme finite weights must not overflow.
    p.attractionWeight = p.salienceWeight = p.affinityWeight = (std::numeric_limits<float>::max)();
    auto bearing = Scored(1, 1.f);
    bearing.salience = 1; bearing.spatial = SpatialKind::Bearing;
    for (float magnitude : {std::numeric_limits<float>::denorm_min(), (std::numeric_limits<float>::max)()}) {
        auto observer = Observer(); observer.referenceForward = {magnitude, 0, 0};
        bearing.bearing = {magnitude, 0, 0};
        Harness h;
        CHECK(h.Run(observer, std::span(&bearing, 1), p, 0) == StepStatus::Ok);
        CHECK(h.trace.candidates[0].eligibility == Eligibility::Eligible);
        CHECK(h.trace.candidates[0].angleGate == 1 && test::Near(h.output.rawScore, 5.0 / 6.0, 1e-7));
    }
    auto observer = Observer();
    observer.position.x = -(std::numeric_limits<double>::max)();
    c.position.x = (std::numeric_limits<double>::max)();
    Harness overflow;
    CHECK(overflow.Run(observer, std::span(&c, 1), p, 0) == StepStatus::Ok);
    CHECK(overflow.trace.candidates[0].eligibility == Eligibility::OutOfRange);
    CHECK(overflow.output.selected == 0 && overflow.output.rawScore == 0 && overflow.output.effectiveScore == 0);
    // The diagnostic distance may be +inf for finite coordinates whose difference
    // overflows. Scores remain finite; this does not promise finite input copies.
    CHECK(std::isinf(overflow.trace.candidates[0].distance));
}

void AdjacentSaturationAndThresholds()
{
    const ContextPreference contexts[1]{};
    auto p = test::TemporalProfile(contexts);
    p.minDwellMs = 0; p.noveltyBeta = 1;
    // One float ulp below/at/above raw + event == 1, before and after rounding.
    for (float strength : {std::nextafter(0.5f, 0.f), 0.5f, std::nextafter(0.5f, 1.f)}) {
        auto c = Scored(1, 0.5f); c.eventStrength = strength;
        Harness h;
        CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
        const double sum = 0.5 + static_cast<double>(strength);
        CHECK(h.trace.candidates[0].noveltyContribution == static_cast<double>(strength));
        CHECK(h.output.effectiveScore == static_cast<float>((std::min)(1.0, sum)));
        CHECK(h.output.effectiveScore == 1); // lower neighbor rounds to 1 too (ties to even)
    }
    // Effective-score thresholds, with exactly representable dyadic contributions.
    for (float strength : {std::nextafter(0.25f, 0.f), 0.25f, std::nextafter(0.25f, 1.f)}) {
        auto c = Scored(1, 0.f); c.eventStrength = strength;
        auto profile = p; profile.acquireThreshold = 0.25f; profile.releaseThreshold = 0.125f;
        Harness h;
        CHECK(h.Run(Observer(), std::span(&c, 1), profile, 0) == StepStatus::Ok);
        CHECK((h.output.selected == 1) == (strength >= 0.25f));
        CHECK(test::ConsistentStep(h, profile));
    }
    for (float strength : {std::nextafter(0.375f, 0.f), 0.375f, std::nextafter(0.375f, 1.f)}) {
        auto c = Scored(1, 0.f); c.eventStrength = strength;
        Harness h;
        const CandidateSnapshot first[]{Scored(1, 0.625f)};
        CHECK(h.Run(Observer(), first, p, 0) == StepStatus::Ok);
        CHECK(h.Run(Observer(), std::span(&c, 1), p, 0) == StepStatus::Ok);
        CHECK((h.output.selected == 1) == (strength >= 0.375f));
        CHECK(test::ConsistentStep(h, p));
    }
}

void TimeTranslationAndDecayProperties()
{
    const ContextPreference contexts[1]{};
    auto p = test::TemporalProfile(contexts);
    p.noveltyBeta = 0.75f;
    constexpr TimeMs lo = (std::numeric_limits<TimeMs>::min)();
    constexpr TimeMs hi = (std::numeric_limits<TimeMs>::max)();
    const TimeMs starts[]{lo, -10000, 0, hi - 10000};
    const TimeMs offsets[]{0, 1, 499, 500, 999, 1000, 2000, 8000};
    test::Random rng(0xC180102);
    for (int trial = 0; trial < 128; ++trial) {
        const float strength = rng.Unit();
        const float raw = rng.Unit();
        std::array<Harness, 4> translated;
        double previousNovelty = 1;
        float previousScore = 1;
        for (auto offset : offsets) {
            for (std::size_t i = 0; i < translated.size(); ++i) {
                const CandidateSnapshot incumbent[]{Scored(1, 0.625f)};
                if (offset == 0) { CHECK(translated[i].Run(Observer(), incumbent, p, starts[i]) == StepStatus::Ok); }
                auto event = Scored(2, raw);
                event.eventTimeMs = starts[i]; event.eventStrength = strength;
                const CandidateSnapshot frame[]{incumbent[0], event};
                CHECK(translated[i].Run(Observer(), frame, p, starts[i] + offset) == StepStatus::Ok);
                CHECK(test::ConsistentStep(translated[i], p));
                const auto& reference = translated[0];
                CHECK(test::SameDecision(reference.output, translated[i].output));
                CHECK(test::SamePolicyTrace(reference.trace, translated[i].trace));
                CHECK(reference.memory.current == translated[i].memory.current);
                CHECK(static_cast<std::uint64_t>(reference.memory.focusStartTime) - static_cast<std::uint64_t>(starts[0]) ==
                      static_cast<std::uint64_t>(translated[i].memory.focusStartTime) - static_cast<std::uint64_t>(starts[i]));
                CHECK(reference.Find(2)->eventNovelty == translated[i].Find(2)->eventNovelty);
                CHECK(reference.Find(2)->effectiveScore == translated[i].Find(2)->effectiveScore);
            }
            const auto& e = *translated[0].Find(2);
            CHECK(e.eventNovelty <= previousNovelty && e.effectiveScore <= previousScore);
            if (offset == 1000) { CHECK(e.eventNovelty == static_cast<double>(strength) / 2); }
            if (offset == 2000) { CHECK(e.eventNovelty == static_cast<double>(strength) / 4); }
            previousNovelty = e.eventNovelty; previousScore = e.effectiveScore;
        }
    }
}
} // namespace

int main()
{
    test::Run("rigid geometry transforms, host distance units and observer ID",
              GeometryTransformsPreserveScores);
    test::Run("finite extremes and adjacent distance cutoffs", FiniteExtremesAndAdjacentCutoffs);
    test::Run("adjacent saturation and effective policy thresholds", AdjacentSaturationAndThresholds);
    test::Run("extreme time translations and monotonic novelty decay", TimeTranslationAndDecayProperties);
    return test::Finish("interest_property_test");
}
