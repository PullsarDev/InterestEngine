// SPDX-FileCopyrightText: 2026 PullsarDev
// SPDX-License-Identifier: Apache-2.0

// Verifies version metadata, C++20 and optional constrained-build compiler flags.

#include "interest/version.hpp"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Expect(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", description);
        ++failures;
    }
}

} // namespace

int main()
{
    Expect(std::strcmp(interest::VersionString(), "0.1.0") == 0,
           "VersionString() returns the library version");

    Expect(__cplusplus >= 202002L, "compiled as C++20 or later");

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
