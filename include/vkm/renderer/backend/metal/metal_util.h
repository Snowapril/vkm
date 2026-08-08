// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#import <Foundation/NSError.h>
#import <Metal/MTLPixelFormat.h>

#include <string>

namespace vkm
{
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
