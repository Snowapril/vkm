// Author : snowapril

#define DOCTEST_CONFIG_IMPLEMENT

#include "UnitTestUtils.hpp"
#include <spdlog/spdlog.h>

int main()
{
    doctest::Context context;

    spdlog::set_level(spdlog::level::debug);

    // Run queries, or run tests unless --no-run is specified
    return context.run();
}