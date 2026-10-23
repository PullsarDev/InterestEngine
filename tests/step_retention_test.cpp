// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Regression evidence for event novelty, geometric limits and configuration sensitivity.
#include "test_support.hpp"

#include <algorithm>
#include <array>

namespace {
using interest::AttentionProfile;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::DecisionKind;
using interest::TimeMs;
using test::Harness;

enum class Control { B2, Fatigue, Novelty, Combined, Salience, Salience8 };
constexpr Control controls[]{Control::B2, Control::Novelty, Control::Salience, Control::Salience8};
constexpr Control crowdControls[]{Control::B2, Control::Novelty};

const char* Name(Control c)
{
    switch (c) {
    case Control::B2: return "B2";
    case Control::Fatigue: return "fatigue";
    case Control::Novelty: return "novelty";
    case Control::Combined: return "combined";
    case Control::Salience: return "salience";
    case Control::Salience8: return "salience8";
    }
    return "unknown";
}

AttentionProfile Profile(std::span<const ContextPreference> contexts, Control control)
{
    auto p = test::TemporalProfile(contexts);
    p.minDwellMs = 0;
    p.noveltyBeta = control == Control::Novelty || control == Control::Combined ? 0.75f : 0.f;
    return p;
}

double Novelty(const CandidateSnapshot& c, TimeMs now, TimeMs halfLife)
{
    if (c.eventStrength == 0.f) { return 0; }
    const auto age = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(c.eventTimeMs);
    return static_cast<double>(c.eventStrength) *
           std::exp2(-static_cast<double>(age) / static_cast<double>(halfLife));
}

// The strongest unit-strength host pulse reaches salience=1 at onset. It does not
// modify attraction, affinity, geometry or the temporal policy. Other controls see the
// exact base snapshots, including the same event pair.
void HostPulse(std::vector<CandidateSnapshot>& frame, Control c, TimeMs now, TimeMs halfLife)
{
    if (c != Control::Salience && c != Control::Salience8) { return; }
    for (auto& candidate : frame) {
        candidate.salience = static_cast<float>(static_cast<double>(candidate.salience) +
            (1.0 - static_cast<double>(candidate.salience)) * Novelty(candidate, now, halfLife));
        candidate.eventStrength = 0.f;
    }
}

struct CrowdMetrics {
    int switches = 0;
    int clears = 0;
    std::size_t peak = 0;
    std::size_t evictions = 0;
    std::size_t freshSamples = 0;
    int freshSelections = 0;
    int positiveFreshSelections = 0;
    double lostH = 0;
    std::size_t decayUpdates = 0;
};

CrowdMetrics Crowd(std::size_t population, std::uint64_t seed, bool varied,
                   const AttentionProfile& p)
{
    test::Random rng(seed);
    std::vector<CandidateSnapshot> frame;
    for (std::size_t i = 0; i < population; ++i) {
        const float score = varied ? 0.7f + 0.15f * rng.Unit() : 0.8f;
        frame.push_back(test::Scored(1 + i, score));
    }
    Harness h;
    CrowdMetrics m;
    for (TimeMs now = 0; now <= 60000; now += 50) {
        CHECK(h.Run(test::Observer(), frame, p, now) == interest::StepStatus::Ok);
        CHECK(test::ConsistentStep(h, p));
        m.switches += h.output.kind == DecisionKind::Switch ? 1 : 0;
        m.clears += h.output.kind == DecisionKind::Clear ? 1 : 0;
        rng.Shuffle(frame);
    }
    return m;
}

void CapacitySweep()
{
    const ContextPreference contexts[1]{};
    for (bool varied : {false, true}) {
        for (std::size_t population : {8u, 16u, 17u, 24u, 40u}) {
            for (Control control : crowdControls) {
                const auto p = Profile(contexts, control);
                const auto m = Crowd(population, 0xC1700, varied, p);
                if (control == Control::B2 || control == Control::Novelty) {
                    CHECK(m.switches == 0 && m.clears == 0);
                }
                CHECK(m.peak == 0 && m.evictions == 0 && m.decayUpdates == 0);
                std::printf("  crowd n=%zu varied=%d %-8s switch=%d clear=%d peak=%zu "
                            "evict=%zu freshSamples=%zu freshSelections=%d positiveFresh=%d "
                            "lostH=%.3f exp2Updates=%zu\n",
                            population, varied, Name(control), m.switches, m.clears, m.peak,
                            m.evictions, m.freshSamples, m.freshSelections, m.positiveFreshSelections,
                            m.lostH, m.decayUpdates);
            }
        }
    }
}

void C13C15AndRecovery()
{
    const ContextPreference contexts[1]{};
    for (Control control : crowdControls) {
        const auto p = Profile(contexts, control);
        Harness h;
        int renewals = 0;
        int alreadyAttending = 0;
        TimeMs firstClear = -1;
        for (TimeMs now = 0; now <= 90000; now += 50) {
            std::vector<CandidateSnapshot> frame;
            if (now < 60000 || now >= 80000) {
                auto c = test::Scored(1, 0.8f);
                if (now >= 10150 && now < 60000) {
                    c.eventTimeMs = now < 20150 ? 10150 : 20150;
                    c.eventStrength = 1;
                }
                frame.push_back(c);
            }
            CHECK(h.Run(test::Observer(), frame, p, now) == interest::StepStatus::Ok);
            if (now < 60000 && firstClear < 0 && h.output.kind == DecisionKind::Clear) {
                firstClear = now;
            }
            if (now == 10150 || now == 20150) {
                renewals += h.output.kind == DecisionKind::Acquire ? 1 : 0;
                alreadyAttending += h.output.kind == DecisionKind::Keep ? 1 : 0;
            }
            if (now == 80000) { CHECK(h.output.kind == DecisionKind::Acquire); }
        }
        if (control == Control::B2 || control == Control::Novelty) {
            CHECK(firstClear == -1 && alreadyAttending == 2 && renewals == 0);
        }
        std::printf("  C13/C15 %-8s firstClear=%lldms renewals=%d alreadyAttending=%d "
                    "return=80000ms\n", Name(control), static_cast<long long>(firstClear),
                    renewals, alreadyAttending);
    }
}

struct EventMetrics {
    int frontCaptures = 0;
    int behindCaptures = 0;
    int returns = 0;
    int falseSwitches = 0;
    int changes = 0;
    TimeMs maxLatency = -1;
    TimeMs duration = 0;
};

EventMetrics EventStream(Control control, TimeMs interval, bool weak)
{
    const ContextPreference contexts[1]{};
    auto p = Profile(contexts, control);
    p.salienceWeight = control == Control::Salience8 ? 8.f : 1.f;
    // Attraction/salience tradeoff is held fixed for all other controls. Affinity
    // weight stays zero, so the pulse cannot borrow a third consideration's budget.
    Harness h, shuffled, untraced;
    test::Random rng(0xC130017);
    EventMetrics m;
    int capturedCycle = -1;
    int returnedCycle = -1;
    for (TimeMs now = 0; now < 600000; now += interval) { // ten minutes, 120 independent events
        const int cycle = static_cast<int>(now / 5000);
        const int label = cycle % 3; // front important, far irrelevant, behind important
        const TimeMs onset = static_cast<TimeMs>(cycle) * 5000 + 150;
        std::vector<CandidateSnapshot> frame;
        for (interest::CandidateId id = 1; id <= 24; ++id) {
            const float score = id == 1 ? 0.625f : 0.45f + rng.Unit() * 0.1f;
            frame.push_back(test::NonSpatial(id, score, score));
        }
        const float cue = weak ? 0.1f : 0.6f;
        auto event = test::NonSpatial(99, cue, cue);
        if (label == 1) { event.spatial = interest::SpatialKind::Point; event.position = {19, 0, 0}; }
        if (label == 2) { event.spatial = interest::SpatialKind::Bearing; event.bearing = {-1.f, 0, 0}; }
        if (now >= onset) { event.eventTimeMs = onset; event.eventStrength = 1; }
        frame.push_back(event);
        // A non-event salient cue, labelled irrelevant. Increasing the salience
        // weight must account for this cost on exactly the same underlying stream.
        const bool spike = now % 5000 >= 3000 && now % 5000 < 3250;
        frame.push_back(test::NonSpatial(100, 0.f, spike ? 1.f : 0.f));
        HostPulse(frame, control, now, p.noveltyHalfLifeMs);
        CHECK(h.Run(test::Observer(), frame, p, now) == interest::StepStatus::Ok);
        CHECK(test::ConsistentStep(h, p));
        m.changes += h.output.selected != h.output.previous ? 1 : 0;
        if (h.output.kind == DecisionKind::Switch &&
            (h.output.selected != 1 && (h.output.selected != 99 || label == 1))) {
            ++m.falseSwitches;
        }
        if (h.output.selected == 99) {
            m.duration += interval;
            if (capturedCycle != cycle && now >= onset && now - onset <= 1000) {
                capturedCycle = cycle;
                if (label == 0) { ++m.frontCaptures; }
                if (label == 2) { ++m.behindCaptures; }
                m.maxLatency = (std::max)(m.maxLatency, now - onset);
            }
        }
        if (capturedCycle == cycle && returnedCycle != cycle && h.output.selected == 1) {
            returnedCycle = cycle;
            ++m.returns;
        }
        interest::Reserve(untraced.scratch, frame.size());
        CHECK(interest::Step(test::Observer(), frame, p, now, untraced.memory, untraced.scratch,
                             untraced.output) == interest::StepStatus::Ok);
        rng.Shuffle(frame);
        CHECK(shuffled.Run(test::Observer(), frame, p, now) == interest::StepStatus::Ok);
        CHECK(test::SameDecision(h.output, shuffled.output) && test::SameMemory(h.memory, shuffled.memory));
        CHECK(test::SameDecision(h.output, untraced.output) && test::SameMemory(h.memory, untraced.memory));
        CHECK(test::SamePolicyTrace(h.trace, shuffled.trace));
        for (const auto& e : shuffled.trace.candidates) {
            CHECK(test::SameEntry(e, *h.Find(e.id)));
        }
    }
    std::printf("  events %-8s %lldHz weak=%d front=%d/40 behind=%d/40 return=%d "
                "falseSwitch=%d changes=%d maxLatency=%lldms attention=%lldms\n", Name(control),
                static_cast<long long>(1000 / interval), weak, m.frontCaptures, m.behindCaptures,
                m.returns, m.falseSwitches, m.changes, static_cast<long long>(m.maxLatency),
                static_cast<long long>(m.duration));
    return m;
}

void NoveltyVersusHostSalience()
{
    for (Control control : controls) {
        const auto m = EventStream(control, 50, true);
        if (control == Control::Novelty) {
            CHECK(m.frontCaptures == 40 && m.behindCaptures == 0 && m.falseSwitches == 0);
            CHECK(m.returns == 40 && m.maxLatency == 0);
        }
        if (control == Control::B2 || control == Control::Salience) { CHECK(m.frontCaptures == 0); }
        if (control == Control::Salience8) { CHECK(m.frontCaptures == 40 && m.falseSwitches >= 120); }
    }
    const auto low = EventStream(Control::Novelty, 200, true);
    CHECK(low.frontCaptures == 40 && low.maxLatency == 50 && low.falseSwitches == 0);
    // Easy event cues can be served by host salience alone: acknowledge this simpler domain.
    const auto easy = EventStream(Control::Salience, 50, false);
    const auto easyNovelty = EventStream(Control::Novelty, 50, false);
    CHECK(easy.frontCaptures == 40 && easy.falseSwitches == 0);
    CHECK(easy.frontCaptures == easyNovelty.frontCaptures && easy.returns == easyNovelty.returns &&
          easy.duration == easyNovelty.duration && easy.changes == easyNovelty.changes);
}

void C14C16AndSaturation()
{
    ContextPreference contexts[2]{};
    contexts[0].affinityTags = contexts[1].affinityTags = 3;
    contexts[0].tagAffinity[0] = contexts[1].tagAffinity[1] = 0.9f;
    for (Control control : crowdControls) {
        auto p = Profile(contexts, control);
        p.salienceWeight = p.affinityWeight = 1;
        CandidateSnapshot frame[]{test::NonSpatial(1, 0.4f, 0.4f), test::NonSpatial(2, 0.4f, 0.4f)};
        frame[0].tags = 1; frame[1].tags = 2;
        Harness a, b;
        CHECK(a.Run(test::Observer(0), frame, p, 0) == interest::StepStatus::Ok);
        CHECK(b.Run(test::Observer(1), frame, p, 0) == interest::StepStatus::Ok);
        CHECK(a.output.selected == 1 && b.output.selected == 2);
    }
    auto p = Profile(contexts, Control::Novelty);
    const CandidateSnapshot event[]{[] { auto c = test::Scored(1, 0.25f);
                                       c.eventStrength = 1; c.eventTimeMs = -1000; return c; }()};
    Harness h;
    for (TimeMs now = -1000; now <= 4000; now += 50) {
        CHECK(h.Run(test::Observer(), event, p, now) == interest::StepStatus::Ok);
        CHECK(h.Find(1)->eventNovelty == Novelty(event[0], now, p.noveltyHalfLifeMs));
    }
    CandidateSnapshot pair[]{test::Scored(1, 0.8f), test::Scored(2, 0.9f)};
    pair[0].eventStrength = 0.5f; pair[1].eventStrength = 1;
    Harness tie;
    CHECK(tie.Run(test::Observer(), std::span<const CandidateSnapshot>(pair, 1), p, 0) == interest::StepStatus::Ok);
    CHECK(tie.Run(test::Observer(), pair, p, 0) == interest::StepStatus::Ok);
    CHECK(tie.Find(1)->effectiveScore == 1 && tie.Find(2)->effectiveScore == 1);
    CHECK_DECISION(tie, Keep, HighestScore, 1); // saturation hides a >margin unsaturated lead
    CHECK(static_cast<double>(pair[1].attraction) + 0.75 -
          (static_cast<double>(pair[0].attraction) + 0.375) > static_cast<double>(p.switchMargin));
    std::printf("  C16 contexts choose different IDs; C14 exact age; saturation masks "
                "a passing unsaturated lead as an M1 tie\n");
}

void NoveltySensitivity()
{
    const ContextPreference contexts[1]{};
    for (float beta : {0.25f, 0.5f, 0.75f, 1.f}) {
        for (TimeMs halfLife : {TimeMs{250}, TimeMs{1000}, TimeMs{4000}}) {
            auto p = Profile(contexts, Control::Novelty);
            p.noveltyBeta = beta;
            p.noveltyHalfLifeMs = halfLife;
            Harness h;
            TimeMs captured = -1;
            TimeMs returned = -1;
            for (TimeMs now = 0; now <= 15000; now += 50) {
                CandidateSnapshot frame[]{test::Scored(1, 0.625f), test::Scored(2, 0.1f)};
                if (now >= 150) { frame[1].eventTimeMs = 150; frame[1].eventStrength = 1; }
                CHECK(h.Run(test::Observer(), frame, p, now) == interest::StepStatus::Ok);
                if (captured < 0 && h.output.selected == 2) { captured = now; }
                if (captured >= 0 && returned < 0 && h.output.selected == 1) { returned = now; }
            }
            CHECK((captured >= 0) == (beta >= 0.75f));
            CHECK(captured < 0 || (captured == 150 && returned > captured));
            std::printf("  novelty sensitivity beta=%.2f halfLife=%lldms capture=%lldms "
                        "duration=%lldms\n", static_cast<double>(beta), static_cast<long long>(halfLife),
                        static_cast<long long>(captured), static_cast<long long>(captured < 0 ? 0 : returned - captured));
        }
    }
}
} // namespace

int main()
{
    std::printf("  footprints observer=%zu candidate=%zu trace=%zu bytes\n",
                sizeof(interest::ObserverMemory), sizeof(CandidateSnapshot), sizeof(interest::CandidateTrace));
    test::Run("C17 capacity cliff and seeded parameter sensitivity", CapacitySweep);
    test::Run("C13/C15 benefit labels and recovery", C13C15AndRecovery);
    test::Run("ten-minute event streams versus host salience", NoveltyVersusHostSalience);
    test::Run("C14/C16 and saturation cost", C14C16AndSaturation);
    test::Run("novelty beta and half-life sensitivity", NoveltySensitivity);
    return test::Finish("interest_retention_test");
}
