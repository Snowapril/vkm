---
description: Run scripts/run_tests.py and fix any errors it reports
argument-hint: "[run_tests.py args, e.g. --build-type Release]"
---

# Run Unit Tests

Run the project's unit test script and fix whatever errors it reports.

1. Execute `python3 scripts/run_tests.py $ARGUMENTS` from the project root.
2. If it fails — non-zero exit code, any `FAIL` entry in the "Test Summary"
   table, a CMake configure/build failure, or a crashing test binary —
   diagnose the root cause from the command output.
3. Fix the actual bug in the source, test, or CMake files. Do not band-aid
   the failure by disabling the failing backend or skipping/removing tests.
4. Re-run the script and repeat steps 2-3 until every entry in the summary
   is `PASS` or `SKIP` (`SKIP` is expected for `webgpu`, and for `vulkan` on
   macOS when no Vulkan SDK is installed).
5. Report a concise summary of what was broken and what was changed.
