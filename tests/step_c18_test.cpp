// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Sustained candidate churn, invalid inputs and exact replay of decisions, memory and traces.
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace {
using interest::AttentionProfile;
using interest::CandidateId;
using interest::CandidateSnapshot;
using interest::CandidateTrace;
using interest::ContextId;
using interest::ContextPreference;
using interest::DecisionReason;
using interest::DecisionTrace;
using interest::Eligibility;
using interest::ObserverMemory;
using interest::ObserverSnapshot;
using interest::Reserve;
using interest::ResetMemory;
using interest::SpatialKind;
using interest::Step;
using interest::StepStatus;
using interest::TagMask;
using interest::TimeMs;

constexpr std::size_t kCapacity = 72;
constexpr int kSteps = 12000;

bool SameFloat(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool SameEntry(const CandidateTrace& a, const CandidateTrace& b)
{
    // Compare fields, not padding; copied invalid inputs may contain NaNs.
    bool same = a.id == b.id && a.eligibility == b.eligibility &&
        SameFloat(a.attraction, b.attraction) && SameFloat(a.salience, b.salience) &&
        SameFloat(a.perceptionStrength, b.perceptionStrength) && a.distance == b.distance &&
        a.distanceGate == b.distanceGate && a.angleGate == b.angleGate && a.gate == b.gate &&
        a.affinity == b.affinity && a.preference == b.preference && a.rawScore == b.rawScore;
    same = same && a.effectiveScore == b.effectiveScore && a.eventTimeMs == b.eventTimeMs &&
        SameFloat(a.eventStrength, b.eventStrength) && a.eventNovelty == b.eventNovelty &&
        a.noveltyContribution == b.noveltyContribution;
    return same;
}

bool SameTrace(const DecisionTrace& a, const DecisionTrace& b)
{
    if (!test::SamePolicyTrace(a, b) || a.candidates.size() != b.candidates.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.candidates.size(); ++i) {
        if (!SameEntry(a.candidates[i], b.candidates[i])) { return false; }
    }
    return true;
}

// A diagnostic fingerprint of every base decision, memory and policy-trace
// field, independent of object layout/endian/padding. Full exact transition files
// can additionally be emitted for byte comparison with another build of this test.
void Record(std::FILE* file, std::uint64_t& hash, std::uint64_t value)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        const auto part = static_cast<unsigned char>((value >> (byte * 8)) & 255u);
        hash = (hash ^ part) * 1099511628211ull;
        if (file != nullptr) { CHECK(std::fputc(part, file) != EOF); }
    }
}

void RecordStep(std::FILE* file, std::uint64_t& hash, const test::Harness& h)
{
    const auto& d = h.output;
    const auto& m = h.memory;
    const auto& t = h.trace;
    const std::uint64_t fields[]{
        static_cast<std::uint64_t>(d.kind), static_cast<std::uint64_t>(d.reason),
        d.selected, d.previous, std::bit_cast<std::uint32_t>(d.rawScore),
        m.current, static_cast<std::uint64_t>(m.focusStartTime),
        static_cast<std::uint64_t>(m.lastStepTime), m.hasStepped ? 1u : 0u,
        t.leading, std::bit_cast<std::uint32_t>(t.leadingScore), t.currentEligible ? 1u : 0u,
        std::bit_cast<std::uint32_t>(t.currentScore), t.challenger,
        std::bit_cast<std::uint32_t>(t.challengerScore),
        static_cast<std::uint64_t>(t.dwellRemainingMs),
        std::bit_cast<std::uint32_t>(t.acquireThreshold),
        std::bit_cast<std::uint32_t>(t.releaseThreshold),
        std::bit_cast<std::uint32_t>(t.switchMargin),
        std::bit_cast<std::uint32_t>(t.overrideMargin)};
    for (auto value : fields) { Record(file, hash, value); }
}

void InjectErrors(test::Harness& h, const ObserverSnapshot& observer,
                  const AttentionProfile& p, TimeMs now)
{
    const test::Harness before = h;
    const auto* scratchData = h.scratch.sortedIds.data();
    const auto* traceData = h.trace.candidates.data();
    const auto scratchCapacity = h.scratch.sortedIds.capacity();
    const auto traceCapacity = h.trace.candidates.capacity();
    const std::array<CandidateSnapshot, 1> frame{test::Scored(10000, 1.f)};
    const std::vector<CandidateSnapshot> oversized(scratchCapacity + 1, frame[0]);
    auto invalidProfile = p; invalidProfile.attractionWeight = -1.f;
    auto invalidObserver = observer; invalidObserver.referenceForward = {};
    const auto check = [&](StepStatus expected, const ObserverSnapshot& o,
                           const AttentionProfile& profile, TimeMs time,
                           std::span<const CandidateSnapshot> candidates) {
        CHECK(Step(o, candidates, profile, time, h.memory, h.scratch, h.output,
                   &h.trace) == expected);
        CHECK(test::SameMemory(before.memory, h.memory));
        CHECK(test::SameDecision(before.output, h.output));
        CHECK(SameTrace(before.trace, h.trace));
        CHECK(before.scratch.sortedIds == h.scratch.sortedIds);
        CHECK(scratchData == h.scratch.sortedIds.data());
        CHECK(traceData == h.trace.candidates.data());
        CHECK(scratchCapacity == h.scratch.sortedIds.capacity());
        CHECK(traceCapacity == h.trace.candidates.capacity());
    };
    // Overlapping failures also verify the documented validation precedence.
    check(StepStatus::InvalidProfile, invalidObserver, invalidProfile,
          h.memory.lastStepTime - 1, oversized);
    check(StepStatus::InvalidObserver, invalidObserver, p,
          h.memory.lastStepTime - 1, oversized);
    check(StepStatus::TimeWentBackwards, observer, p, h.memory.lastStepTime - 1, oversized);
    check(StepStatus::InsufficientCapacity, observer, p, now, oversized);
    DecisionTrace shortTrace;
    CHECK(Step(observer, frame, p, now, h.memory, h.scratch, h.output,
               &shortTrace) == StepStatus::InsufficientCapacity);
    CHECK(shortTrace.candidates.empty() && shortTrace.candidates.capacity() == 0);
    CHECK(test::SameMemory(before.memory, h.memory) && test::SameDecision(before.output, h.output));
    CHECK(before.scratch.sortedIds == h.scratch.sortedIds && SameTrace(before.trace, h.trace));
    CHECK(scratchData == h.scratch.sortedIds.data() && traceData == h.trace.candidates.data());
    CHECK(scratchCapacity == h.scratch.sortedIds.capacity() && traceCapacity == h.trace.candidates.capacity());
}

bool ValidTransition(const test::Harness& h, const ObserverMemory& before,
                     const AttentionProfile& p, TimeMs now,
                     std::span<const CandidateSnapshot> frame)
{
    bool ok = test::ConsistentStep(h, p) && h.output.previous == before.current &&
        h.memory.current == h.output.selected && h.memory.hasStepped &&
        h.memory.lastStepTime == now && h.trace.candidates.size() == frame.size();
    const TimeMs focus = h.output.selected == 0 ? 0
        : h.output.selected == before.current ? before.focusStartTime : now;
    const auto kind = h.output.selected == 0
        ? (before.current == 0 ? interest::DecisionKind::None : interest::DecisionKind::Clear)
        : (before.current == 0 ? interest::DecisionKind::Acquire
           : before.current == h.output.selected ? interest::DecisionKind::Keep : interest::DecisionKind::Switch);
    ok = ok && h.memory.focusStartTime == focus && h.output.kind == kind;
    TimeMs dwell = 0;
    if (before.current != 0 && now >= before.focusStartTime) {
        const auto elapsed = static_cast<std::uint64_t>(now) -
                             static_cast<std::uint64_t>(before.focusStartTime);
        if (elapsed < static_cast<std::uint64_t>(p.minDwellMs)) {
            dwell = p.minDwellMs - static_cast<TimeMs>(elapsed);
        }
    }
    ok = ok && h.trace.dwellRemainingMs == dwell &&
        h.trace.acquireThreshold == p.acquireThreshold &&
        h.trace.releaseThreshold == p.releaseThreshold &&
        h.trace.switchMargin == p.switchMargin && h.trace.overrideMargin == p.overrideMargin;
    bool foundSelected = h.output.selected == 0 && h.output.rawScore == 0.f;
    foundSelected = foundSelected && h.output.effectiveScore == 0.f;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto& c = frame[i];
        const auto& e = h.trace.candidates[i];
        const auto count = std::count_if(frame.begin(), frame.end(),
            [&](const auto& other) { return other.id == c.id; });
        ok = ok && e.id == c.id && SameFloat(e.attraction, c.attraction) &&
            SameFloat(e.salience, c.salience) && SameFloat(e.perceptionStrength, c.perceptionStrength);
        if (c.id == 0) { ok = ok && e.eligibility == Eligibility::InvalidId; }
        else if (count > 1) { ok = ok && e.eligibility == Eligibility::DuplicateId; }
        const float computed[]{e.distanceGate, e.angleGate, e.gate, e.affinity,
                               e.preference, e.rawScore};
        for (float value : computed) {
            ok = ok && std::isfinite(value) && value >= 0.f && value <= 1.f;
        }
        ok = ok && e.eventTimeMs == c.eventTimeMs && SameFloat(e.eventStrength, c.eventStrength) &&
            std::isfinite(e.effectiveScore) && e.effectiveScore >= 0.f && e.effectiveScore <= 1.f &&
            std::isfinite(e.eventNovelty) && e.eventNovelty >= 0 && e.eventNovelty <= 1 &&
            std::isfinite(e.noveltyContribution) && e.noveltyContribution >= 0 && e.noveltyContribution <= 1;
        if (e.eligibility == Eligibility::Eligible) {
            ok = ok && e.effectiveScore == static_cast<float>((std::min)(
                1.0, static_cast<double>(e.rawScore) + e.noveltyContribution));
        } else {
            ok = ok && e.effectiveScore == 0 && e.eventNovelty == 0 && e.noveltyContribution == 0;
        }
        if (e.eligibility != Eligibility::Eligible) {
            ok = ok && e.rawScore == 0 && (h.output.selected == 0 || e.id != h.output.selected);
        } else if (e.id == h.output.selected) {
            foundSelected = h.output.rawScore == e.rawScore;
            foundSelected = foundSelected && h.output.effectiveScore == e.effectiveScore;
        }
    }
    return ok && foundSelected;
}

void Stream(std::uint64_t seed, bool novelty, std::FILE* recording)
{
    ContextPreference contexts[2]{};
    contexts[1].requiredTags = TagMask{1};
    contexts[1].excludedTags = TagMask{2};
    contexts[1].affinityTags = TagMask{1} << 63;
    contexts[1].tagAffinity[63] = 0.875f;
    AttentionProfile p = test::TemporalProfile(contexts);
    p.salienceWeight = 1.f; p.affinityWeight = 1.f;
    p.noveltyBeta = novelty ? 0.75f : 0.f;
    test::Random rng(seed), shuffler(seed ^ 0xABCDEFu);
    std::array<CandidateSnapshot, 64> pool{};
    for (std::size_t i = 0; i < pool.size(); ++i) { pool[i] = test::Scored(i + 1, 0.625f); }
    test::Harness h, replay, shuffled, untraced;
    for (auto* state : {&h, &replay, &shuffled, &untraced}) {
        Reserve(state->scratch, kCapacity); Reserve(state->trace, kCapacity);
    }
    const auto* scratchData = h.scratch.sortedIds.data();
    const auto* traceData = h.trace.candidates.data();
    const auto scratchCapacity = h.scratch.sortedIds.capacity();
    const auto traceCapacity = h.trace.candidates.capacity();
    std::uint64_t hash = 14695981039346656037ull;
    std::array<int, 10> reasons{};
    std::array<int, 11> rejections{};
    int resets = 0, failures = 0, duplicates = 0, repeated = 0;
    int events = 0;
    CandidateId nextId = 65;
    TimeMs now = -100000;
    for (int step = 0; step < kSteps; ++step) {
        const TimeMs delta = static_cast<TimeMs>(rng.Below(401));
        if (delta == 0) { ++repeated; }
        now += delta;
        if (step % 997 == 0) {
            for (auto* state : {&h, &replay, &shuffled, &untraced}) { ResetMemory(state->memory); }
            ++resets;
        }
        // Replace one source's lifetime and update another source's facts. Events
        // remain unchanged on other pool members across arbitrarily many steps.
        auto& c = pool[static_cast<std::size_t>(rng.Below(pool.size()))];
        c = test::Scored(nextId++, rng.Unit());
        c.salience = rng.Unit(); c.perceptionStrength = rng.Unit();
        c.spatial = static_cast<SpatialKind>(rng.Below(3));
        c.position = {static_cast<double>(rng.Below(49)) - 24, static_cast<double>(rng.Below(17)) - 8, 0};
        c.bearing = {rng.Unit() - 0.5f, rng.Unit() - 0.5f, 0.25f};
        c.tags = rng.Below(4) | (rng.Below(2) << 63);
        c.enabled = rng.Below(8) != 0;
        const auto age = static_cast<TimeMs>(rng.Below(2001));
        const float strength = rng.Unit();
        const auto edit = rng.Below(40);
        if (edit == 0) { c.attraction = std::numeric_limits<float>::quiet_NaN(); }
        if (edit == 1) { c.position.x = std::numeric_limits<double>::infinity(); c.spatial = SpatialKind::Point; }
        if (edit == 2) { c.perceptionStrength = 0; }
        if (edit == 3) { c.spatial = static_cast<SpatialKind>(99); }
        if (edit == 4) { c.id = 0; }
        c.eventTimeMs = now - age; c.eventStrength = strength;
        auto& changed = pool[static_cast<std::size_t>(rng.Below(pool.size()))];
        changed.eventTimeMs = now;
        changed.eventStrength = 0.5f;
        // Invalid novelty event inputs are outside the temporal compatibility domain.
        if (novelty && edit == 5) { c.eventTimeMs = now + 1; c.eventStrength = 1.f; }
        if (novelty && edit == 6) { c.eventStrength = std::numeric_limits<float>::quiet_NaN(); }
        std::vector<CandidateSnapshot> frame;
        const auto count = static_cast<std::size_t>(rng.Below(pool.size() + 1));
        frame.insert(frame.end(), pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(count));
        if (!frame.empty() && step % 11 == 0) {
            const auto duplicate = frame.front();
            frame.push_back(duplicate); frame.push_back(duplicate); ++duplicates;
        }
        std::vector<std::size_t> order(frame.size());
        for (std::size_t i = 0; i < order.size(); ++i) { order[i] = i; }
        shuffler.Shuffle(order);
        std::vector<CandidateSnapshot> permuted;
        for (auto i : order) { permuted.push_back(frame[i]); }
        const auto observer = test::Observer(static_cast<ContextId>(rng.Below(2)));
        p.minDwellMs = step % 3 == 0 ? 0 : 500;
        const int checksBefore = test::State().failures;
        if (step % 127 == 1) { InjectErrors(h, observer, p, now); ++failures; }
        const auto before = h.memory;
        CHECK(Step(observer, frame, p, now, h.memory, h.scratch, h.output, &h.trace) == StepStatus::Ok);
        CHECK(Step(observer, frame, p, now, replay.memory, replay.scratch, replay.output, &replay.trace) == StepStatus::Ok);
        CHECK(Step(observer, permuted, p, now, shuffled.memory, shuffled.scratch, shuffled.output, &shuffled.trace) == StepStatus::Ok);
        CHECK(Step(observer, frame, p, now, untraced.memory, untraced.scratch, untraced.output) == StepStatus::Ok);
        CHECK(ValidTransition(h, before, p, now, frame));
        CHECK(test::SameMemory(h.memory, replay.memory) && test::SameDecision(h.output, replay.output));
        CHECK(SameTrace(h.trace, replay.trace));
        CHECK(test::SameMemory(h.memory, shuffled.memory) && test::SameDecision(h.output, shuffled.output));
        CHECK(test::SamePolicyTrace(h.trace, shuffled.trace));
        CHECK(test::SameMemory(h.memory, untraced.memory) && test::SameDecision(h.output, untraced.output));
        for (std::size_t i = 0; i < order.size(); ++i) {
            CHECK(SameEntry(h.trace.candidates[order[i]], shuffled.trace.candidates[i]));
        }
        CHECK(scratchData == h.scratch.sortedIds.data() && traceData == h.trace.candidates.data());
        CHECK(scratchCapacity == h.scratch.sortedIds.capacity() && traceCapacity == h.trace.candidates.capacity());
        ++reasons[static_cast<std::size_t>(h.output.reason)];
        for (const auto& e : h.trace.candidates) {
            ++rejections[static_cast<std::size_t>(e.eligibility)];
            if (e.eventNovelty > 0) { ++events; }
            if (!novelty && e.eligibility == Eligibility::Eligible) { CHECK(e.effectiveScore == e.rawScore); }
        }
        RecordStep(recording, hash, h);
        if (checksBefore != test::State().failures) {
            std::fprintf(stderr, "C18 seed=%llu novelty=%d step=%d time=%lld\n",
                static_cast<unsigned long long>(seed), novelty ? 1 : 0, step, static_cast<long long>(now));
            return; // a bounded, directly reproducible first failure
        }
    }
    CHECK(resets > 10 && failures > 90 && duplicates > 1000 && repeated > 10);
    for (auto reason : {DecisionReason::HighestScore, DecisionReason::CurrentInvalid,
                       DecisionReason::BetterCandidate, DecisionReason::MinimumDwell}) {
        CHECK(reasons[static_cast<std::size_t>(reason)] > 0);
    }
    for (auto rejection : {Eligibility::Eligible, Eligibility::InvalidId, Eligibility::DuplicateId,
                          Eligibility::InvalidValue, Eligibility::InvalidGeometry, Eligibility::Disabled,
                          Eligibility::NotPerceived, Eligibility::MissingRequiredTag,
                          Eligibility::ExcludedTag, Eligibility::OutOfRange}) {
        CHECK(rejections[static_cast<std::size_t>(rejection)] > 0);
    }
    CHECK(events > kSteps);
    if (novelty) { CHECK(rejections[static_cast<std::size_t>(Eligibility::InvalidEvent)] > 0); }
    std::printf("  C18 seed=%llu beta=%s steps=%d simulated_ms=%lld resets=%d error_batches=%d "
                "triple_frames=%d repeated_times=%d transition_hash=%016llx\n",
        static_cast<unsigned long long>(seed), novelty ? "0.75" : "0", kSteps,
        static_cast<long long>(now + 100000), resets, failures, duplicates, repeated,
        static_cast<unsigned long long>(hash));
}

std::FILE* recordingFile = nullptr;
void C18()
{
    for (std::uint64_t seed : {0xC180001ull, 0xC180002ull, 0xC180003ull, 0xC180004ull}) {
        Stream(seed, false, recordingFile);
        Stream(seed, true, nullptr);
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 2) { std::fprintf(stderr, "usage: interest_c18_test [legacy-transition-output]\n"); return 1; }
    if (argc == 2) {
#ifdef _MSC_VER
        if (fopen_s(&recordingFile, argv[1], "wb") != 0) { recordingFile = nullptr; }
#else
        recordingFile = std::fopen(argv[1], "wb");
#endif
        if (recordingFile == nullptr) { std::perror("C18 recording"); return 1; }
    }
    test::Run("C18 sustained candidate/event churn", C18);
    if (recordingFile != nullptr) { CHECK(std::fclose(recordingFile) == 0); }
    return test::Finish("interest_c18_test");
}
