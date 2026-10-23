// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

#include "interest/attention.hpp"

#include <array>
#include <cstdio>

int main()
{
    // The profile borrows this storage. Keep it alive through every Step call.
    std::array<interest::ContextPreference, 1> contexts{};
    contexts[0].affinityTags = 1;
    contexts[0].tagAffinity[0] = 0.8f;

    interest::AttentionProfile profile{};
    profile.contexts = contexts;
    profile.fullRange = 10.f;
    profile.maxRange = 30.f;
    profile.acquireThreshold = 0.5f;
    profile.releaseThreshold = 0.35f;
    profile.switchMargin = 0.1f;
    profile.overrideMargin = 0.3f;
    profile.minDwellMs = 400;
    profile.noveltyBeta = 0.25f;
    profile.noveltyHalfLifeMs = 1000;
    if (interest::ValidateProfile(profile) != interest::StepStatus::Ok) {
        return 1;
    }

    interest::ObserverMemory memory{};  // one per observer; persist between calls
    interest::StepScratch scratch{};
    interest::DecisionTrace trace{};    // optional
    interest::Reserve(scratch, 8);      // setup may allocate; Step will not
    interest::Reserve(trace, 8);

    interest::ObserverSnapshot observer{};
    observer.id = 1;
    observer.referenceForward = {1.f, 0.f, 0.f};  // body/travel baseline
    observer.context = 0;

    std::array<interest::CandidateSnapshot, 1> candidates{};
    auto& candidate = candidates[0];
    candidate.id = 42;
    candidate.position = {5.0, 0.0, 0.0};
    candidate.attraction = 0.8f;
    candidate.salience = 0.7f;
    candidate.tags = 1;
    candidate.eventTimeMs = 1000;
    candidate.eventStrength = 1.f;

    // Explicit monotonic host time; the event timestamp stays unchanged on replay.
    for (const interest::TimeMs now : {1000, 1200}) {
        interest::Decision decision{};
        const auto status = interest::Step(observer, candidates, profile, now,
                                           memory, scratch, decision, &trace);
        if (status != interest::StepStatus::Ok) {
            std::fprintf(stderr, "Step failed: %s\n", interest::ToString(status));
            return 1;
        }
        switch (decision.kind) {
        case interest::DecisionKind::Acquire:
        case interest::DecisionKind::Keep:
        case interest::DecisionKind::Switch:
            // The host resolves the ID and drives gameplay or presentation.
            std::printf("%s %llu (%s)\n", interest::ToString(decision.kind),
                        static_cast<unsigned long long>(decision.selected),
                        interest::ToString(decision.reason));
            break;
        case interest::DecisionKind::Clear:
        case interest::DecisionKind::None:
            std::puts("No attention target");
            break;
        }
        if (decision.selected != 42 ||
            decision.kind != (now == 1000 ? interest::DecisionKind::Acquire
                                         : interest::DecisionKind::Keep)) {
            return 1;
        }
    }
    return 0;
}
