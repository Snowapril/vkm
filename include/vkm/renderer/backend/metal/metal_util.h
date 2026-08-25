// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#import <Foundation/NSError.h>
#import <Metal/MTLPixelFormat.h>
// MTLSparsePageSize, for kVkmMetalSparsePageSize below.
#import <Metal/MTLResource.h>

#include <string>

namespace vkm
{
    /*
    * @brief Page size every placement-sparse texture and the tile heap backing it are created with.
    * @details The smallest Metal offers, chosen for the mip tail rather than for the tile. A
    * texture's tail -- every level too small to fill one tile -- is indivisible and stays backed
    * for the texture's life, so the page size sets the floor on what streaming can ever give back.
    * Measured on an M3 Pro against a 2048x2048 RGBA8 chain: 16 KiB pages put the tail at level 6
    * and 16 KiB, 64 KiB pages at level 5 and 64 KiB, 256 KiB at level 4 and 256 KiB. The smallest
    * page therefore streams two more levels and leaves a sixteenth of the residue.
    * One value engine-wide: a texture and the heap it is mapped from must agree on it.
    */
    inline constexpr MTLSparsePageSize kVkmMetalSparsePageSize = MTLSparsePageSize16;

    MTLPixelFormat getMTLPixelFormat(VkmFormat format);

    /*
    * @brief Describes an NSError from a Metal call. Metal is allowed to fail without populating
    * the out-param, so error - and its localizedDescription - may be nil; both yield "unknown error"
    * rather than a null const char* being handed to a formatter.
    */
    std::string mtlErrorToString(NSError* error);

    /*
    * @brief Logs "msg: <reason>" and returns false when a Metal factory call returned nil.
    * error is the NSError out-param of that call, or nil for the calls that do not take one.
    */
    bool mtlCheckObject(id object, NSError* error, const char* msg);

#define VKM_MTL_CHECK(object, error, msg) mtlCheckObject(object, error, msg)
#define VKM_MTL_ASSERT(object, error, msg) VKM_ASSERT(mtlCheckObject(object, error, msg), msg)
}
