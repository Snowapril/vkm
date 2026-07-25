#include "UnitTestUtils.hpp"

#include <chrono>
#include <thread>

// Guards the test harness's own time-budget machinery (see UnitTests.cpp): every test case has
// a budget, and blowing one is a failure rather than something that goes unnoticed.

TEST_CASE("Test time budget - every registered test case has one")
{
    // main() gives the default budget to every test that declares no timeout decorator, so
    // after that pass nothing may be left unbudgeted. A non-zero count here means the default
    // stopped being applied -- at which point a newly hung test would hang the whole run again.
    CHECK(vkmtest::countTestsWithoutTimeBudget() == 0);
    CHECK(vkmtest::getDefaultTestTimeoutSeconds() > 0.0);
}

// A test that overruns its declared budget must be *failed*. `should_fail` is what lets that be
// asserted from inside the suite: doctest expects this test to fail, so the deliberate overrun
// keeps the run green while still proving the timeout is enforced. Remove the decorators and
// this test starts reporting "exceeded time limit of 0.05s".
//
// The sleep is far longer than the budget but far shorter than the watchdog's grace period (see
// kMinHangGraceSeconds), so this exercises doctest's post-hoc check without tripping the
// process-killing watchdog.
TEST_CASE("Test time budget - overrunning a declared budget fails the test"
          * doctest::timeout(0.05) * doctest::should_fail())
{
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}
