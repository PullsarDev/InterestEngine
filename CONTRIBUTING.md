# Contributing

Use a focused pull request that describes the problem, the resulting behavior and
the checks actually run. Include a minimal reproducible scenario for a bug report:
snapshots, profile and context values, explicit times, previous state, status and
decision/trace. Remove private game data before submitting.

The production library uses C++20 and the standard library only. Preserve explicit
time, caller ownership, allocation-free prepared steps, validation precedence and
deterministic candidate ordering. Changes to numerical behavior or public contracts
need deliberate review and regression evidence. Keep host integration outside the
kernel.

Build with warnings treated as errors and run CTest in Debug and Release. Follow
the README commands; also check `INTEREST_CONSTRAINED_BUILD=ON` where practical.
The CI matrix adds GCC, Clang, MSVC and Clang address/undefined-behavior sanitizers.
Report unavailable configurations honestly.

Tests use dependency-free checks that remain active under `NDEBUG`. The suites cover:

| Area | Files in `tests/` |
|---|---|
| Version and compiler constraints | `smoke_test.cpp` |
| Snapshots, rejection paths, gates and formulas | `step_scenario_test.cpp`, `step_scoring_test.cpp` |
| Allocation and capacity contracts | `step_allocation_test.cpp` |
| Alternative formula controls | `step_c07c_test.cpp` |
| Hysteresis, dwell, switching and time extremes | `step_temporal_test.cpp` |
| Stateless baseline, update rates, observer variation | `step_baseline_test.cpp`, `step_rate_test.cpp`, `step_observers_test.cpp` |
| Novelty, saturation and sensitivity | `step_novelty_test.cpp`, `step_retention_test.cpp` |
| Sustained churn, exact replay, permutations and failed-step atomicity | `step_c18_test.cpp` |
| Geometry transforms, finite extremes and time translation/decay | `step_property_test.cpp` |

In test output, **B1** is stateless selection of the leading candidate at or above
the acquisition threshold. **B2** uses the same contextual scores with the kernel's
temporal policy; novelty is disabled for these baseline comparisons. B1 is read
from the Step trace so the comparison uses identical scores. Ablations change one
temporal mechanism through profile parameters.

Labels such as `C07c`, `C12` and `C18` are stable scenario identifiers: preference
formula comparison, update-rate comparison and sustained churn, respectively.
Letter suffixes distinguish variants. The test functions and nearby comments
describe each scenario; these labels are not public API concepts.

The benchmark also checks paired inputs, replay, trace independence and allocation
counts in smoke mode. See [benchmark methodology](docs/benchmarks.md) before making
performance claims. Do not use CI smoke timings as performance results.

When editing the README example, update `examples/basic.cpp` identically and run
`interest_basic_example`. The parent-project fixture in `examples/consumer` checks
the exported CMake target and that utilities default off when embedded.

By submitting a contribution, you agree to license it under Apache-2.0, unless
explicitly stated otherwise. Preserve third-party attribution and identify the
origin and license of any proposed external material.
