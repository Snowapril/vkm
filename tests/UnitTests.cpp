// Author : snowapril

#define DOCTEST_CONFIG_IMPLEMENT

#include "UnitTestUtils.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace
{
    /*
    * Per-test time budget.
    *
    * Every test case is failed if it runs longer than this, so a test that silently becomes
    * far slower is a build failure rather than something nobody notices. A test that is
    * legitimately slower declares its own budget at the test itself:
    *
    *     TEST_CASE("imports a large scene" * doctest::timeout(20.0)) { ... }
    *
    * A declared budget always wins -- the default below is only applied to test cases that
    * carry no timeout decorator (see applyDefaultTestTimeout).
    *
    * Sized well above the slowest undecorated test on a healthy machine so it catches
    * regressions without going off on a loaded CI runner.
    */
    constexpr double kDefaultTestTimeoutSeconds = 10.0;

    /*
    * Gives every test case that declares no timeout decorator the default budget. Mutating the
    * registry in place is what makes the default apply to every existing test case without
    * decorating each one; doctest's own check (and its "exceeded time limit" failure) then
    * applies uniformly. Safe with respect to the registry's std::set ordering, which is by
    * line/name/file/template-id only -- see TestCase::operator<.
    */
    void applyDefaultTestTimeout(double seconds)
    {
        for (const doctest::detail::TestCase& testCase : doctest::detail::getRegisteredTests())
        {
            if (testCase.m_timeout == 0.0)
            {
                const_cast<doctest::detail::TestCase&>(testCase).m_timeout = seconds;
            }
        }
    }

#if !defined(__EMSCRIPTEN__)
    /*
    * doctest measures a test's duration and fails it *afterwards*, which cannot help when a
    * test never returns at all -- that hangs the whole run with no output and no attribution.
    * So a watchdog thread kills the process once a test overruns its budget by this factor,
    * turning an indefinite hang into a failure that names the test responsible.
    *
    * The factor is what keeps the two mechanisms from racing: a test that merely overruns is
    * reported by doctest as an ordinary failure and the run continues through the remaining
    * tests; only a test that is genuinely stuck reaches the watchdog.
    *
    * Native-only: the wasm build links no pthreads (see VKM_EMSCRIPTEN_PORT_FLAGS), so
    * std::thread would throw at startup there. The budget itself still applies on wasm through
    * doctest's own check above -- only the hang-killer is missing, and a browser-hosted run is
    * bounded by scripts/run_tests.py's own harness rather than by this thread.
    */
    constexpr double kHangTimeoutFactor = 3.0;
    constexpr double kMinHangGraceSeconds = 5.0;

    // Tracks the running test case's deadline and kills the process once it passes, naming the
    // test so a hang is attributable from a CI log alone.
    class TestHangWatchdog final : public doctest::IReporter
    {
    public:
        explicit TestHangWatchdog(const doctest::ContextOptions&) {}

        void test_run_start() override
        {
            _running = true;
            _thread = std::thread([this] { watch(); });
        }

        void test_run_end(const doctest::TestRunStats&) override
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _running = false;
                _testActive = false;
                ++_generation;
            }
            _wakeUp.notify_all();
            if (_thread.joinable())
            {
                _thread.join();
            }
        }

        void test_case_start(const doctest::TestCaseData& testCaseData) override
        {
            const double budget = (testCaseData.m_timeout > 0.0) ? testCaseData.m_timeout
                                                                 : kDefaultTestTimeoutSeconds;
            const double grace = std::max(budget * kHangTimeoutFactor, budget + kMinHangGraceSeconds);
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _name = (testCaseData.m_name != nullptr) ? testCaseData.m_name : "<unnamed>";
                _file = testCaseData.m_file.c_str();
                _line = testCaseData.m_line;
                _budgetSeconds = budget;
                _deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(static_cast<int64_t>(grace * 1000.0));
                _testActive = true;
                ++_generation;
            }
            _wakeUp.notify_all();
        }

        // Re-entry for unfinished subcases restarts the clock, matching doctest's own duration
        // measurement: each pass through the test case body is timed as its own run.
        void test_case_reenter(const doctest::TestCaseData& testCaseData) override
        {
            test_case_start(testCaseData);
        }

        void test_case_end(const doctest::CurrentTestCaseStats&) override
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _testActive = false;
                ++_generation;
            }
            _wakeUp.notify_all();
        }

        void report_query(const doctest::QueryData&) override {}
        void test_case_exception(const doctest::TestCaseException&) override {}
        void subcase_start(const doctest::SubcaseSignature&) override {}
        void subcase_end() override {}
        void log_assert(const doctest::AssertData&) override {}
        void log_message(const doctest::MessageData&) override {}
        void test_case_skipped(const doctest::TestCaseData&) override {}

    private:
        void watch()
        {
            std::unique_lock<std::mutex> lock(_mutex);
            while (_running)
            {
                if (!_testActive)
                {
                    _wakeUp.wait(lock, [this] { return _testActive || !_running; });
                    continue;
                }

                const uint64_t generation = _generation;
                const std::chrono::steady_clock::time_point deadline = _deadline;
                // True when this test ended or another one started; false means the deadline
                // passed with the same test still running.
                if (_wakeUp.wait_until(lock, deadline,
                                       [this, generation] { return _generation != generation || !_running; }))
                {
                    continue;
                }

                std::fprintf(stderr,
                             "\n[doctest] TEST HUNG: \"%s\" (%s:%u) blew its %gs budget and never "
                             "returned; killing the run.\nIf it is legitimately this slow, give it "
                             "its own budget: TEST_CASE(\"...\" * doctest::timeout(seconds)).\n",
                             _name.c_str(), _file.c_str(), _line, _budgetSeconds);
                std::fflush(stderr);
                std::fflush(stdout);
                // _Exit, not abort(): abort() runs backward-cpp's SIGABRT handler, which tries
                // to symbolize a stack trace from this watchdog thread while the hung thread may
                // hold the malloc lock -- observed deadlocking there, leaving the run wedged
                // exactly as if there were no watchdog at all. Everything worth reporting is
                // already flushed above, so skip handlers and atexit entirely.
                std::_Exit(EXIT_FAILURE);
            }
        }

        std::thread _thread;
        std::mutex _mutex;
        std::condition_variable _wakeUp;

        bool _running = false;
        bool _testActive = false;
        uint64_t _generation = 0;

        std::string _name;
        std::string _file;
        unsigned _line = 0;
        double _budgetSeconds = 0.0;
        std::chrono::steady_clock::time_point _deadline;
    };
#endif // !defined(__EMSCRIPTEN__)
} // namespace

#if !defined(__EMSCRIPTEN__)
REGISTER_LISTENER("vkm_hang_watchdog", 1, TestHangWatchdog);
#endif

namespace vkmtest
{
    double getDefaultTestTimeoutSeconds()
    {
        return kDefaultTestTimeoutSeconds;
    }

    int countTestsWithoutTimeBudget()
    {
        int count = 0;
        for (const doctest::detail::TestCase& testCase : doctest::detail::getRegisteredTests())
        {
            if (testCase.m_timeout == 0.0)
            {
                ++count;
            }
        }
        return count;
    }
} // namespace vkmtest

int main(int argc, char** argv)
{
    doctest::Context context;

    // Lets a developer narrow a run down (--test-case="*Camera*", --duration=true), which is
    // also how you re-run just the test the watchdog named.
    context.applyCommandLine(argc, argv);

    spdlog::set_level(spdlog::level::debug);

    applyDefaultTestTimeout(kDefaultTestTimeoutSeconds);

    // Run queries, or run tests unless --no-run is specified
    return context.run();
}
