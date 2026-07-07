// Copyright (c) 2025 Snowapril

#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>

namespace vkm
{
    // Looks up `key` in `table`. Returns std::nullopt if `key` is not a recognized
    // value for EnumT. Callers are responsible for turning a nullopt into a
    // descriptive parse error (field name + offending string) at the call site,
    // since only the caller knows which JSON field is being parsed.
    template <typename EnumT>
    std::optional<EnumT> parseEnumFromString(const std::unordered_map<std::string_view, EnumT>& table, std::string_view key)
    {
        auto it = table.find(key);
        if (it == table.end())
        {
            return std::nullopt;
        }
        return it->second;
    }
} // namespace vkm
