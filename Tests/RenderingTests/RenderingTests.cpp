// Author : snowapril

#define DOCTEST_CONFIG_IMPLEMENT

#include "RenderingTestsUtils.hpp"

int main()
{
    doctest::Context context;

    // Run queries, or run tests unless --no-run is specified
    return context.run();
}