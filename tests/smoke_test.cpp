// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Verifies version metadata, x64 trace layout, C++20 and optional compiler constraints.

#include "interest/attention.hpp"
#include "interest/version.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {

int failures = 0;

void Expect(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", description);
        ++failures;
    }
}

void CheckTraceLayout()
{
    using interest::CandidateTrace;
    static_assert(std::is_standard_layout_v<CandidateTrace>);
    std::printf("CandidateTrace: size=%zu alignment=%zu eventStrength=%zu noveltyGate=%zu "
                "eventNovelty=%zu noveltyContribution=%zu\n", sizeof(CandidateTrace),
                alignof(CandidateTrace), offsetof(CandidateTrace, eventStrength),
                offsetof(CandidateTrace, noveltyGate), offsetof(CandidateTrace, eventNovelty),
                offsetof(CandidateTrace, noveltyContribution));
#if defined(_M_X64) || defined(__x86_64__)
    // Recorded v0.1.0 offsets for the tested x64 ABIs. Keep the full legacy field
    // list here so using alignment padding cannot silently move an earlier field.
    // These checks document the tested layouts, not an ABI promise for other targets.
    struct FieldOffset { const char* name; std::size_t actual; std::size_t legacy; };
    const FieldOffset fields[]{
        {"id", offsetof(CandidateTrace, id), 0},
        {"eligibility", offsetof(CandidateTrace, eligibility), 8},
        {"attraction", offsetof(CandidateTrace, attraction), 12},
        {"salience", offsetof(CandidateTrace, salience), 16},
        {"perceptionStrength", offsetof(CandidateTrace, perceptionStrength), 20},
        {"distance", offsetof(CandidateTrace, distance), 24},
        {"distanceGate", offsetof(CandidateTrace, distanceGate), 32},
        {"angleGate", offsetof(CandidateTrace, angleGate), 36},
        {"gate", offsetof(CandidateTrace, gate), 40},
        {"affinity", offsetof(CandidateTrace, affinity), 44},
        {"preference", offsetof(CandidateTrace, preference), 48},
        {"rawScore", offsetof(CandidateTrace, rawScore), 52},
        {"effectiveScore", offsetof(CandidateTrace, effectiveScore), 56},
        {"eventTimeMs", offsetof(CandidateTrace, eventTimeMs), 64},
        {"eventStrength", offsetof(CandidateTrace, eventStrength), 72},
        {"eventNovelty", offsetof(CandidateTrace, eventNovelty), 80},
        {"noveltyContribution", offsetof(CandidateTrace, noveltyContribution), 88},
    };
    Expect(sizeof(CandidateTrace) == 96, "x64 trace retains the v0.1.0 96-byte size");
    Expect(alignof(CandidateTrace) == 8, "x64 trace retains 8-byte alignment");
    Expect(offsetof(CandidateTrace, noveltyGate) == 76, "noveltyGate occupies x64 alignment padding");
    for (const auto& field : fields) {
        Expect(field.actual == field.legacy, field.name);
    }
#endif
}

} // namespace

int main()
{
    Expect(std::strcmp(interest::VersionString(), "0.2.0") == 0,
           "VersionString() returns the library version");

    Expect(__cplusplus >= 202002L, "compiled as C++20 or later");
    CheckTraceLayout();

#if defined(INTEREST_EXPECT_CONSTRAINED_BUILD)
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    Expect(false, "exceptions are disabled under INTEREST_CONSTRAINED_BUILD");
#endif
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
    Expect(false, "RTTI is disabled under INTEREST_CONSTRAINED_BUILD");
#endif
#endif

    if (failures != 0) {
        std::fprintf(stderr, "%d smoke check(s) failed\n", failures);
        return 1;
    }
    std::printf("interest_smoke_test: all checks passed (version %s)\n",
                interest::VersionString());
    return 0;
}
