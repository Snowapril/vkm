#include <iostream>

int main(int argc, char* argv[])
{
    cxxopts::Options options("vkm", "vkm main");
    options.add_options()("d,debug", "Enable vulkan validation layer", cxxopts::value<bool>()->default_value("false"));

    return 0;
}