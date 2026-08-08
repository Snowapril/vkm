// Copyright (c) 2025 Snowapril

#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>

namespace vkm
{
    /*
    * @brief Looks up an enum value by its string spelling.
    * @details The caller turns a nullopt into a descriptive parse error naming the field and the
    * offending string, only it knowing which JSON field is being parsed.
    * @param table Recognized spellings for EnumT.
    * @param key Spelling to look up.
    * @return The value, or nullopt when `key` is not a recognized value for EnumT.
    */
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
