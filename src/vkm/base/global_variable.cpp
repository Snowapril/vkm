// Copyright (c) 2025 Snowapril

#include <vkm/base/global_variable.h>
#include <vkm/base/common.h>

#include <cstdint>
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

    template <>
    bool GlobalVariable<int32_t>::setFromString(const std::string& valueStr)
    {
        try
        {
            size_t consumed = 0;
            const long parsed = std::stol(valueStr, &consumed);
            if (consumed != valueStr.size() || parsed < INT32_MIN || parsed > INT32_MAX)
            {
                return false;
            }
            _value = static_cast<int32_t>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    template <>
    bool GlobalVariable<uint32_t>::setFromString(const std::string& valueStr)
    {
        // std::stoul silently wraps a leading '-'; reject negatives explicitly.
        if (valueStr.empty() || valueStr[0] == '-')
        {
            return false;
        }
        try
        {
            size_t consumed = 0;
            const unsigned long parsed = std::stoul(valueStr, &consumed);
            if (consumed != valueStr.size() || parsed > UINT32_MAX)
            {
                return false;
            }
            _value = static_cast<uint32_t>(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    template <>
    bool GlobalVariable<float>::setFromString(const std::string& valueStr)
    {
        try
        {
            size_t consumed = 0;
            const float parsed = std::stof(valueStr, &consumed);
            if (consumed != valueStr.size())
            {
                return false;
            }
            _value = parsed;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
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
