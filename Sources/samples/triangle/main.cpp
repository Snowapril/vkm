#include <iostream>
#include <cxxopts.hpp>

int main(int argc, char* argv[])
{
    cxxopts::Options options("triangle", "simple triangle rendering");
    options.add_options()("d,debug", "Enable vulkan validation layer", cxxopts::value<bool>()->default_value("false"));

    return 0;
}