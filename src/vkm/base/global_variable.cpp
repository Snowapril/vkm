// Copyright (c) 2025 Snowapril

#include <vkm/base/global_variable.h>
#include <vkm/base/common.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace vkm
{
    GlobalVariableBase::GlobalVariableBase(const char* name)
        : _name(name)
    {
    }

    // Out-of-line definition anchors GlobalVariableBase's vtable in this translation unit.
    GlobalVariableBase::~GlobalVariableBase()
    {
    }

    GlobalVariableManager& GlobalVariableManager::singleton()
    {
        // Immortal singleton: placement-new into static storage, never destroyed -- same idiom
        // as LoggerManager/MemoryTracker. Must be valid during the static initialization that
        // registers global variables, so it cannot depend on static destruction order.
        alignas(GlobalVariableManager) static unsigned char storage[sizeof(GlobalVariableManager)];
        static GlobalVariableManager* instance = ::new (storage) GlobalVariableManager();
        return *instance;
    }

    void GlobalVariableManager::registerVariable(GlobalVariableBase* variable)
    {
        _variables.push_back(variable);
    }

    void GlobalVariableManager::setCommandLineOverrides(const std::vector<std::string>& args)
    {
        _pendingOverrides = args;
    }

    size_t GlobalVariableManager::applyCommandLineOverrides()
    {
        size_t applied = 0;
        for (const std::string& arg : _pendingOverrides)
        {
            // Only "--<name>=<value>" tokens are ours; anything else belongs to another subsystem.
            if (arg.rfind("--", 0) != 0)
            {
                continue;
            }
            const size_t equals = arg.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            const std::string name = arg.substr(2, equals - 2);
            const std::string value = arg.substr(equals + 1);
            if (name.empty())
            {
                continue;
            }

            for (GlobalVariableBase* variable : _variables)
            {
                if (name != variable->getName())
                {
                    continue;
                }
                if (variable->setFromString(value))
                {
                    ++applied;
                    VKM_DEBUG_INFO((std::string("Global variable override: ") + name + " = " + value).c_str());
                }
                else
                {
                    VKM_DEBUG_ERROR((std::string("Global variable '") + name + "' rejected value '" + value + "'").c_str());
                }
            }
        }
        _pendingOverrides.clear();
        return applied;
    }

    // ---- Per-type string parsing. Full string must be consumed, else the value is rejected. ----

    template <>
    bool GlobalVariable<bool>::setFromString(const std::string& valueStr)
    {
        if (valueStr == "1" || valueStr == "true" || valueStr == "True" || valueStr == "TRUE")
        {
            _value = true;
            return true;
        }
        if (valueStr == "0" || valueStr == "false" || valueStr == "False" || valueStr == "FALSE")
        {
            _value = false;
            return true;
        }
        return false;
    }

    // std::from_chars is used instead of std::sto* because it never throws: the WASM build is
    // compiled with -fno-exceptions, where a std::sto* parse failure (e.g. an empty or
    // non-numeric value) aborts the process rather than being caught. from_chars reports the
    // same conditions via its error code and enforces full-string consumption (ptr == last).
    template <>
    bool GlobalVariable<int32_t>::setFromString(const std::string& valueStr)
    {
        const char* first = valueStr.data();
        const char* last = first + valueStr.size();
        int32_t parsed = 0;
        const std::from_chars_result result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc() || result.ptr != last)
        {
            return false;
        }
        _value = parsed;
        return true;
    }

    template <>
    bool GlobalVariable<uint32_t>::setFromString(const std::string& valueStr)
    {
        // from_chars rejects a leading '-' for unsigned types (unlike std::stoul, which wraps it).
        const char* first = valueStr.data();
        const char* last = first + valueStr.size();
        uint32_t parsed = 0;
        const std::from_chars_result result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc() || result.ptr != last)
        {
            return false;
        }
        _value = parsed;
        return true;
    }

    template <>
    bool GlobalVariable<float>::setFromString(const std::string& valueStr)
    {
        // std::from_chars for floating-point is unavailable in some standard libraries our CI
        // uses (e.g. GCC 10's libstdc++), so use std::strtof, which likewise never throws --
        // it reports errors via errno and the end pointer, keeping the -fno-exceptions safety.
        // An explicit empty check is required because strtof consumes nothing for "" and would
        // otherwise leave end == first == first + size() and be mistaken for a full parse of 0.
        if (valueStr.empty())
        {
            return false;
        }
        const char* first = valueStr.c_str();
        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(first, &end);
        if (errno != 0 || end != first + valueStr.size())
        {
            return false;
        }
        _value = parsed;
        return true;
    }

    template <>
    bool GlobalVariable<std::string>::setFromString(const std::string& valueStr)
    {
        _value = valueStr;
        return true;
    }

    template class GlobalVariable<bool>;
    template class GlobalVariable<int32_t>;
    template class GlobalVariable<uint32_t>;
    template class GlobalVariable<float>;
    template class GlobalVariable<std::string>;
} // namespace vkm
