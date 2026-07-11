// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/shader_cache_loader.h>

#include <cstring>
#include <fstream>

namespace vkm
{
    namespace
    {
        void setError(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
        }
    } // namespace

    std::optional<VkmLoadedShaderCache> loadShaderCacheFile(const std::string& filepath,
        VkmShaderCacheBackend expectedBackend, std::string* outError)
    {
        std::ifstream file(filepath, std::ios::binary);
        if (!file)
        {
            setError(outError, "cannot open shader cache file: " + filepath);
            return std::nullopt;
        }

        VkmShaderCacheHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(header)))
        {
            setError(outError, "truncated shader cache header: " + filepath);
            return std::nullopt;
        }

        if (header.magic != kVkmShaderCacheMagic)
        {
            setError(outError, "bad magic in shader cache file: " + filepath);
            return std::nullopt;
        }
        if (header.version != kVkmShaderCacheVersion)
        {
            setError(outError, "shader cache version mismatch in " + filepath);
            return std::nullopt;
        }
        if (header.backend != expectedBackend)
        {
            setError(outError, "shader cache backend mismatch in " + filepath);
            return std::nullopt;
        }

        VkmLoadedShaderCache result;
        result.stage = header.stage;
        result.contentFormat = header.contentFormat;

        // entryPoint is a fixed-size NUL-padded field; strnlen bounds the copy.
        result.entryPoint.assign(header.entryPoint,
            ::strnlen(header.entryPoint, sizeof(header.entryPoint)));

        result.content.resize(static_cast<size_t>(header.contentSize));
        if (header.contentSize > 0)
        {
            file.read(reinterpret_cast<char*>(result.content.data()),
                static_cast<std::streamsize>(header.contentSize));
            if (file.gcount() != static_cast<std::streamsize>(header.contentSize))
            {
                setError(outError, "truncated shader cache content in " + filepath);
                return std::nullopt;
            }
        }

        return result;
    }
} // namespace vkm
