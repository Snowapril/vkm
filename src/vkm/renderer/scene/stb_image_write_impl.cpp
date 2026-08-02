// Copyright (c) 2025 Snowapril

// Single translation unit holding stb_image_write's implementation, for the same reason
// stb_image_impl.cpp exists: the third-party code compiles with the project's
// warnings-as-errors relaxed without relaxing them for our own sources.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
// stb_image_write's .hdr writer uses sprintf, which this SDK deprecates and the project treats
// as an error. Only the PNG path is used here, but the whole header still has to compile.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
