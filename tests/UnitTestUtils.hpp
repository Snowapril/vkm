// Author : snowapril

#ifndef UNIT_TEST_UTILS_HPP
#define UNIT_TEST_UTILS_HPP

#include <doctest/doctest.h>

// Skips the rest of the current TEST_CASE (no failing assertion, so CI treats it as a pass)
// when a driver failed to initialize solely due to unsupported/absent hardware. Any other
// failure still hard-fails via REQUIRE_MESSAGE, carrying the engine-provided reason string.
#define VKM_REQUIRE_DEVICE(result) \
    do { \
        if ((result).code == vkm::VkmInitResultCode::HardwareUnsupported) { \
            MESSAGE("Skipping: " << (result).reason); \
            return; \
        } \
        REQUIRE_MESSAGE((result).code == vkm::VkmInitResultCode::Success, (result).reason); \
    } while (0)

#endif  // ! UNIT_TEST_UTILS_HPP