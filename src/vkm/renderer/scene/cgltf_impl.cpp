// Copyright (c) 2025 Snowapril

// Single translation unit holding cgltf's implementation. It lives apart from
// gltf_importer.cpp so the third-party code can be compiled with the project's
// warnings-as-errors (-Werror / /WX) relaxed without relaxing them for our own sources.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
