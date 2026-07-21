// Copyright (c) 2025 Snowapril

#include <doctest/doctest.h>

#include <vkm/base/global_variable.h>

#include <string>

// File-scope global variables, declared exactly as production code would. Their names are
// unique so command-line-style overrides in the tests below match only these instances.
VKM_GLOBAL_VARIABLE(bool, gv_test_bool, false);
VKM_GLOBAL_VARIABLE(int32_t, gv_test_int, 7);
VKM_GLOBAL_VARIABLE(uint32_t, gv_test_uint, 3u);
VKM_GLOBAL_VARIABLE(float, gv_test_float, 1.5f);
VKM_GLOBAL_VARIABLE(std::string, gv_test_str, std::string("default"));

TEST_CASE("global variable holds its declared default")
{
    CHECK(gv_test_bool.get() == false);
    CHECK(gv_test_int.get() == 7);
    CHECK(gv_test_uint.get() == 3u);
    CHECK(gv_test_float.get() == doctest::Approx(1.5f));
    CHECK(gv_test_str.get() == "default");

    // Implicit conversion to the underlying type also works.
    const int32_t asInt = gv_test_int;
    CHECK(asInt == 7);
}

TEST_CASE("command-line overrides apply by name across all supported types")
{
    vkm::GlobalVariableManager& manager = vkm::GlobalVariableManager::singleton();
    manager.setCommandLineOverrides({
        "--gv_test_bool=true",
        "--gv_test_int=-42",
        "--gv_test_uint=9",
        "--gv_test_float=2.5",
        "--gv_test_str=hello world",
        "--some-engine-flag=1", // not a global variable -> ignored
        "positional",           // not a --name=value token -> ignored
    });

    const size_t applied = manager.applyCommandLineOverrides();

    CHECK(applied == 5);
    CHECK(gv_test_bool.get() == true);
    CHECK(gv_test_int.get() == -42);
    CHECK(gv_test_uint.get() == 9u);
    CHECK(gv_test_float.get() == doctest::Approx(2.5f));
    CHECK(gv_test_str.get() == "hello world");
}

TEST_CASE("malformed values are rejected and leave the variable unchanged")
{
    gv_test_int.set(100);
    gv_test_uint.set(50u);

    vkm::GlobalVariableManager& manager = vkm::GlobalVariableManager::singleton();
    manager.setCommandLineOverrides({
        "--gv_test_int=notanumber",
        "--gv_test_int=99abc", // trailing garbage -> full string not consumed
        "--gv_test_uint=-1",   // negative into unsigned -> rejected
    });

    const size_t applied = manager.applyCommandLineOverrides();

    CHECK(applied == 0);
    CHECK(gv_test_int.get() == 100);
    CHECK(gv_test_uint.get() == 50u);
}

TEST_CASE("bool accepts 0/1 and true/false spellings")
{
    vkm::GlobalVariableManager& manager = vkm::GlobalVariableManager::singleton();

    manager.setCommandLineOverrides({ "--gv_test_bool=0" });
    CHECK(manager.applyCommandLineOverrides() == 1);
    CHECK(gv_test_bool.get() == false);

    manager.setCommandLineOverrides({ "--gv_test_bool=1" });
    CHECK(manager.applyCommandLineOverrides() == 1);
    CHECK(gv_test_bool.get() == true);

    manager.setCommandLineOverrides({ "--gv_test_bool=maybe" });
    CHECK(manager.applyCommandLineOverrides() == 0);
    CHECK(gv_test_bool.get() == true); // unchanged
}
