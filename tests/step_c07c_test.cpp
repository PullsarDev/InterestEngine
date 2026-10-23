// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Compares weighted-mean, max and soft-OR preference formulas through the real kernel.
// Alternative formulas are local test controls, not production options.

#include "test_support.hpp"

#include <cstdio>
#include <vector>

namespace {

using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::CandidateTrace;
using interest::ContextPreference;
using interest::Eligibility;
using interest::StepStatus;
using interest::TagMask;
using test::Harness;
using test::Near;
using test::Observer;
using test::PointAt;

enum class Model { Mean, Max, SoftOR };
constexpr Model kModels[]{Model::Mean, Model::Max, Model::SoftOR};

const char* Name(Model model)
{
    switch (model) {
    case Model::Mean: return "Mean";
    case Model::Max: return "Max";
    case Model::SoftOR: return "SoftOR";
    }
    return "?";
}

double Preference(Model model, const CandidateTrace& e, const AttentionProfile& profile)
{
    const double a = static_cast<double>(e.attraction);
    const double s = static_cast<double>(e.salience);
    const double f = static_cast<double>(e.affinity);
    switch (model) {
    case Model::Mean: {
        const double wa = static_cast<double>(profile.attractionWeight);
        const double ws = static_cast<double>(profile.salienceWeight);
        const double wf = static_cast<double>(profile.affinityWeight);
        return (wa * a + ws * s + wf * f) / (wa + ws + wf);
    }
    case Model::Max: return a > s ? (a > f ? a : f) : (s > f ? s : f);
    case Model::SoftOR: return 1.0 - (1.0 - a) * (1.0 - s) * (1.0 - f);
    }
    return 0.0;
}

struct Outcome {
    CandidateId selected = 0;
    CandidateId leading = 0;
    float leadingScore = 0.f;
    float runnerUpScore = 0.f;  // second-highest eligible score
};

float Score(Model model, const CandidateTrace& e, const AttentionProfile& profile)
{
    return static_cast<float>(static_cast<double>(e.gate) * Preference(model, e, profile));
}

Outcome Select(Model model, const Harness& h, const AttentionProfile& profile)
{
    Outcome out;
    bool any = false;
    for (const CandidateTrace& e : h.trace.candidates) {
        if (e.eligibility != Eligibility::Eligible) {
            continue;
        }
        const float score = Score(model, e, profile);
        if (!any || score > out.leadingScore || (score == out.leadingScore && e.id < out.leading)) {
            if (any) {
                out.runnerUpScore = out.leadingScore;
            }
            out.leading = e.id;
            out.leadingScore = score;
            any = true;
        } else if (score > out.runnerUpScore) {
            out.runnerUpScore = score;
        }
    }
    out.selected = any && out.leadingScore >= profile.acquireThreshold ? out.leading : 0;
    return out;
}

// Affinity is carried by tags so it is explicit per candidate; untagged means 0.
constexpr unsigned kAffinityBits = 11;
constexpr float kAffinityByBit[kAffinityBits]{0.f,  0.1f, 0.2f, 0.3f, 0.4f, 0.5f,
                                              0.6f, 0.7f, 0.8f, 0.9f, 1.f};

ContextPreference ExplicitAffinityContext()
{
    ContextPreference context{};
    context.fallbackAffinity = 0.f;
    for (unsigned bit = 0; bit < kAffinityBits; ++bit) {
        context.affinityTags |= TagMask{1} << bit;
        context.tagAffinity[bit] = kAffinityByBit[bit];
    }
    return context;
}

// affinityTenths: 0..10 selects affinity 0.0..1.0.
CandidateSnapshot Stimulus(CandidateId id, double x, double y, float attraction, float salience,
                           unsigned affinityTenths)
{
    CandidateSnapshot c = PointAt(id, x, y, attraction, salience);
    c.tags = TagMask{1} << affinityTenths;
    return c;
}

struct Case {
    const char* name;
    std::vector<CandidateSnapshot> candidates;
    CandidateId expected[3];  // Mean, Max, SoftOR
};

void FixedScenarios()
{
    const ContextPreference contexts[1]{ExplicitAffinityContext()};
    const AttentionProfile profile = test::Profile(contexts);  // equal weights, threshold 0.3

    // Equal gates: every Point sits at distance 5 straight ahead unless stated.
    const std::vector<Case> cases{
        {"C07c-1 sudden pure-salience event vs steady interest",
         {Stimulus(1, 5, 0, 0.f, 1.f, 0), Stimulus(2, 5, 0, 0.6f, 0.6f, 6)},
         {2, 1, 1}},
        {"C07c-2 weak distractor vs steady interest",
         {Stimulus(1, 5, 0, 0.1f, 0.7f, 1), Stimulus(2, 5, 0, 0.6f, 0.6f, 6)},
         {2, 1, 2}},
        {"C07c-3 one saturated cue vs uniformly strong",
         {Stimulus(1, 5, 0, 0.f, 1.f, 0), Stimulus(2, 5, 0, 1.f, 1.f, 10)},
         {2, 1, 1}},  // Max and SoftOR tie at 1.0; the lower ID wins, not the better source
        {"C07c-4 lone weak cue (no target expected under mean)",
         {Stimulus(1, 5, 0, 0.f, 0.35f, 0)},
         {0, 1, 1}},
        {"C07c-5 several moderate cues vs one strong cue",
         {Stimulus(1, 5, 0, 0.5f, 0.5f, 5), Stimulus(2, 5, 0, 0.8f, 0.f, 1)},
         {1, 2, 1}},
        {"C07c-6 sudden event behind the observer (gate 0.2) vs steady interest ahead",
         {Stimulus(1, -5, 0, 0.f, 1.f, 0), Stimulus(2, 5, 0, 0.6f, 0.6f, 6)},
         {2, 2, 2}},
    };

    std::printf("  %-74s %8s %8s %8s\n", "case (selected id, leading score)", "Mean", "Max",
                "SoftOR");
    for (const Case& c : cases) {
        Harness h;
        CHECK(h.Run(Observer(), c.candidates, profile, 0) == StepStatus::Ok);
        std::printf("  %-74s", c.name);
        for (std::size_t m = 0; m < 3; ++m) {
            const Outcome outcome = Select(kModels[m], h, profile);
            std::printf(" %2llu %5.3f", static_cast<unsigned long long>(outcome.selected),
                        static_cast<double>(outcome.leadingScore));
            if (outcome.selected != c.expected[m]) {
                std::fprintf(stderr, "    %s: %s selected %llu, expected %llu\n", c.name,
                             Name(kModels[m]), static_cast<unsigned long long>(outcome.selected),
                             static_cast<unsigned long long>(c.expected[m]));
                CHECK(outcome.selected == c.expected[m]);
            }
            if (kModels[m] == Model::Mean) {  // the experiment's Mean is the kernel decision
                CHECK(outcome.selected == h.output.selected);
                CHECK(outcome.leading == h.trace.leading);
                CHECK(Near(outcome.leadingScore, static_cast<double>(h.trace.leadingScore),
                           1e-6));
            }
        }
        std::printf("\n");

        if (&c == &cases[2]) {  // saturation: Max and SoftOR cannot tell the two apart
            for (Model model : {Model::Max, Model::SoftOR}) {
                CHECK(Score(model, *h.Find(1), profile) == Score(model, *h.Find(2), profile));
            }
        }
    }

    // Discrimination between two strong sources (0.9 vs 0.8 on every cue). A temporal switch
    // margin is an absolute score difference, so a compressed gap matters.
    const CandidateSnapshot strong[]{Stimulus(1, 5, 0, 0.9f, 0.9f, 9),
                                     Stimulus(2, 5, 0, 0.8f, 0.8f, 8)};
    Harness gap;
    CHECK(gap.Run(Observer(), strong, profile, 0) == StepStatus::Ok);
    std::printf("  C07c-7 score gap, all cues 0.9 vs 0.8:");
    float gaps[3]{};
    for (std::size_t m = 0; m < 3; ++m) {
        const Outcome outcome = Select(kModels[m], gap, profile);
        gaps[m] = outcome.leadingScore - outcome.runnerUpScore;
        std::printf(" %s %.4f", Name(kModels[m]), static_cast<double>(gaps[m]));
        CHECK(outcome.selected == 1);
    }
    std::printf("\n");
    CHECK(Near(gaps[0], 0.1, 1e-5) && Near(gaps[1], 0.1, 1e-5) && Near(gaps[2], 0.007, 1e-5));

    // Salience-weighted mean: the kernel's own weights, no new mechanism.
    AttentionProfile salient = profile;
    salient.salienceWeight = 4.f;
    std::printf("  salience-weighted Mean (1,4,1):");
    for (std::size_t i = 0; i < 2; ++i) {
        Harness h;
        CHECK(h.Run(Observer(), cases[i].candidates, salient, 0) == StepStatus::Ok);
        std::printf(" %s -> %llu", i == 0 ? "C07c-1" : "C07c-2",
                    static_cast<unsigned long long>(h.output.selected));
        CHECK(h.output.selected == (i == 0 ? 1u : 2u));  // takes the event, ignores the distractor
    }
    std::printf("\n");
}

// A generated stream. A steady source of moderate interest stays at a fixed, well-gated
// position ahead (6, 1). Each step adds 0-3 weak distractors (low attraction/affinity,
// salience 0.3-0.8) ahead of the observer, and in about 15% of steps a sudden event
// (salience 0.95-1, low attraction, no affinity) near and ahead. Selecting a distractor
// is a false distraction because the steady source is always available. Gates differ
// between candidates but are identical across models.
void GeneratedStream()
{
    const ContextPreference contexts[1]{ExplicitAffinityContext()};
    const AttentionProfile equal = test::Profile(contexts);
    // Salience-weight sweep for the kernel's own Mean model (sensitivity, not tuning).
    AttentionProfile salient[4]{equal, equal, equal, equal};
    salient[0].salienceWeight = 2.f;
    salient[1].salienceWeight = 3.f;
    salient[2].salienceWeight = 4.f;
    salient[3].salienceWeight = 6.f;

    struct Row {
        const char* label;
        Model model;
        const AttentionProfile* profile;
        int distracted = 0;
        int captured = 0;
        int none = 0;
    };
    Row rows[]{{"Mean (1,1,1)", Model::Mean, &equal, 0, 0, 0},
               {"Mean (1,2,1)", Model::Mean, &salient[0], 0, 0, 0},
               {"Mean (1,3,1)", Model::Mean, &salient[1], 0, 0, 0},
               {"Mean (1,4,1)", Model::Mean, &salient[2], 0, 0, 0},
               {"Mean (1,6,1)", Model::Mean, &salient[3], 0, 0, 0},
               {"Max", Model::Max, &equal, 0, 0, 0},
               {"SoftOR", Model::SoftOR, &equal, 0, 0, 0}};

    test::Random rng(0x5EED0C07u);
    const auto between = [&rng](float lo, float hi) { return lo + (hi - lo) * rng.Unit(); };
    // x in [x0, x0 + xSpan], y in [-ySpan/2, ySpan/2], whole units.
    const auto position = [&rng](double x0, std::uint64_t xSpan, std::uint64_t ySpan, double& x,
                                 double& y) {
        x = x0 + static_cast<double>(rng.Below(xSpan + 1));
        y = static_cast<double>(rng.Below(ySpan + 1)) - static_cast<double>(ySpan) / 2.0;
    };

    constexpr int kSteps = 4000;
    int eventSteps = 0;
    for (int stepIndex = 0; stepIndex < kSteps; ++stepIndex) {
        std::vector<CandidateSnapshot> candidates;
        double x = 0.0;
        double y = 0.0;
        candidates.push_back(Stimulus(1, 6, 1, 0.6f, 0.6f, 6));  // steady interest
        const auto distractors = rng.Below(4);
        for (CandidateId d = 0; d < distractors; ++d) {
            position(2.0, 12, 16, x, y);
            const float attraction = between(0.f, 0.2f);
            const float salience = between(0.3f, 0.8f);
            candidates.push_back(
                Stimulus(10 + d, x, y, attraction, salience, static_cast<unsigned>(rng.Below(2))));
        }
        const bool event = rng.Below(100) < 15;
        if (event) {
            ++eventSteps;
            position(2.0, 7, 10, x, y);
            const float attraction = between(0.f, 0.2f);
            const float salience = between(0.95f, 1.f);
            candidates.push_back(Stimulus(50, x, y, attraction, salience, 0));
        }

        for (Row& row : rows) {
            Harness h;
            if (h.Run(Observer(), candidates, *row.profile, 0) != StepStatus::Ok) {
                CHECK(false);
                continue;
            }
            const CandidateId selected = Select(row.model, h, *row.profile).selected;
            if (row.model == Model::Mean) {
                CHECK(selected == h.output.selected);  // matches the kernel
            }
            row.distracted += selected >= 10 && selected < 50 ? 1 : 0;
            row.captured += selected == 50 ? 1 : 0;
            row.none += selected == 0 ? 1 : 0;
        }
    }

    std::printf("  stream: %d steps, %d with an event\n", kSteps, eventSteps);
    std::printf("  %-14s %22s %22s %10s\n", "model", "distractor selected", "event selected",
                "no target");
    for (const Row& row : rows) {
        std::printf("  %-14s %10d (%5.1f%%) %10d (%5.1f%%) %10d\n", row.label, row.distracted,
                    100.0 * row.distracted / kSteps, row.captured,
                    eventSteps > 0 ? 100.0 * row.captured / eventSteps : 0.0, row.none);
    }

    // Recorded qualitative outcomes (exact counts are printed above).
    const Row& meanRow = rows[0];
    const Row& salientRow = rows[3];
    const Row& maxRow = rows[5];
    const Row& softOrRow = rows[6];
    for (const Row& row : rows) {
        CHECK(row.none == 0);  // the steady source always qualifies
    }
    // Equal-weight mean: never distracted, but never takes a pure-salience event.
    CHECK(meanRow.distracted == 0 && meanRow.captured == 0);
    // Max takes every event and many weak distractors.
    CHECK(maxRow.captured == eventSteps && maxRow.distracted > kSteps / 10);
    // SoftOR is never distracted here and takes about half of the events.
    CHECK(softOrRow.distracted == 0 && softOrRow.captured > eventSteps / 3);
    // Salience-weighted mean (1,4,1): no distraction, more events than SoftOR.
    CHECK(salientRow.distracted == 0 && salientRow.captured > softOrRow.captured);
}

} // namespace

int main()
{
    test::Run("C07c fixed scenarios", FixedScenarios);
    test::Run("C07c generated stream", GeneratedStream);
    return test::Finish("interest_c07c_test");
}
