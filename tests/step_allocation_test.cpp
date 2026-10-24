// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Verifies the capacity contract by counting every global allocation: once the caller
// has reserved capacity, Step must not allocate, including when it rejects an input.
// Global operator new is replaced program-wide, so this suite has its own executable.

#include "test_support.hpp"

#include <cstdlib>
#include <new>
#include <vector>

namespace {

std::size_t allocationCount = 0;

} // namespace

void* operator new(std::size_t size)
{
    ++allocationCount;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    std::abort();  // keeps the replacement valid when exceptions are disabled
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace {

using interest::AttentionProfile;
using interest::CandidateSnapshot;
using interest::ContextPreference;
using interest::StepStatus;

void CounterDetectsAllocations()
{
    const std::size_t before = allocationCount;
    std::vector<int> values;
    values.push_back(1);
    CHECK(allocationCount > before);
}

void PreparedStepsDoNotAllocate()
{
    constexpr std::size_t kCapacity = 32;
    const ContextPreference contexts[2]{};
    AttentionProfile profile = test::Profile(contexts);
    // A release band and dwell, so the steps also take the temporal-policy paths.
    profile.releaseThreshold = 0.2f;
    profile.switchMargin = 0.05f;
    profile.overrideMargin = 0.2f;
    profile.minDwellMs = 250;
    profile.noveltyBeta = 0.75f;

    // Build every input up front so that only Step runs inside the counted region.
    test::Random rng(0x5EED0003u);
    std::vector<std::vector<CandidateSnapshot>> frames(400);
    for (std::vector<CandidateSnapshot>& frame : frames) {
        frame.resize(static_cast<std::size_t>(rng.Below(kCapacity + 1)));
        for (CandidateSnapshot& c : frame) {
            c = test::PointAt(rng.Below(24), 0, 0, 0.f, 0.f);  // ids 0..23: duplicates common
            c.position.x = static_cast<double>(rng.Below(41)) - 20.0;
            c.position.y = static_cast<double>(rng.Below(41)) - 20.0;
            c.attraction = rng.Unit();
            c.salience = rng.Unit();
            c.spatial = static_cast<interest::SpatialKind>(rng.Below(3));
            c.bearing = {1.f, 0.f, 0.f};
            c.enabled = rng.Below(8) != 0;
            c.eventTimeMs = -100;
            c.eventStrength = 0.75f;
            c.eventDirectionality = static_cast<float>(rng.Below(5)) / 4.f;
        }
    }
    // Repeated population changes with eligible inputs and fresh events, independent
    // of the randomized rejection coverage above. No per-candidate memory remains.
    for (std::size_t i = 200; i < frames.size(); ++i) {
        frames[i].clear();
        for (std::size_t j = 0; j < kCapacity; ++j) {
            frames[i].push_back(test::NonSpatial(1000 + (i % 2) * 100 + j, 0.8f, 0.8f));
            frames[i].back().eventTimeMs = static_cast<interest::TimeMs>((i + 1) * 100 - 50);
            frames[i].back().eventStrength = 0.8f;
            frames[i].back().eventDirectionality = static_cast<float>(j % 5) / 4.f;
        }
    }
    const std::vector<CandidateSnapshot> oversized(kCapacity + 1, test::NonSpatial(1, 1.f, 1.f));

    interest::ObserverMemory memory{};
    interest::StepScratch scratch;
    interest::DecisionTrace trace;
    interest::Decision output{};
    interest::Reserve(scratch, kCapacity);
    interest::Reserve(trace, kCapacity);
    AttentionProfile invalidProfile = profile;
    invalidProfile.noveltyHalfLifeMs = 0;
    auto invalidObserver = test::Observer();
    invalidObserver.referenceForward = {};

    const std::size_t before = allocationCount;
    bool allOk = true;
    interest::TimeMs now = 0;
    for (const std::vector<CandidateSnapshot>& frame : frames) {
        now += 100;
        const bool traced = now % 300 != 0;
        const auto context = static_cast<interest::ContextId>(now % 2);
        if (interest::Step(test::Observer(context), frame, profile, now, memory, scratch,
                           output, traced ? &trace : nullptr) != StepStatus::Ok) {
            allOk = false;
        }
    }
    // Rejections do not allocate either.
    const bool rejected =
        interest::Step(test::Observer(), oversized, profile, now, memory, scratch, output,
                       &trace) == StepStatus::InsufficientCapacity &&
        interest::Step(test::Observer(), frames[0], profile, now - 1, memory, scratch, output,
                       &trace) == StepStatus::TimeWentBackwards &&
        interest::Step(test::Observer(), frames[0], invalidProfile, now, memory, scratch,
                       output, &trace) == StepStatus::InvalidProfile &&
        interest::Step(invalidObserver, frames[0], profile, now, memory, scratch,
                       output, &trace) == StepStatus::InvalidObserver;
    const std::size_t allocations = allocationCount - before;

    CHECK(allOk && rejected);
    CHECK(allocations == 0);
    std::printf("  steps: %zu, allocations during steps: %zu\n", frames.size() + 4,
                allocations);
}

} // namespace

int main()
{
    test::Run("allocation counter detects allocations", CounterDetectsAllocations);
    test::Run("prepared steps do not allocate", PreparedStepsDoNotAllocate);
    return test::Finish("interest_allocation_test");
}
