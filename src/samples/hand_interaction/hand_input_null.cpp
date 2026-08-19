// Copyright (c) 2025 Snowapril
//
// createPlatformHandInput() for every platform that has no camera-and-hand-tracking
// implementation. The sample falls back to the cursor stand-in on null, so this is a supported
// configuration rather than a failure.

#include "hand_input.h"

namespace vkm
{
    HandInputSource* createPlatformHandInput()
    {
        return nullptr;
    }
} // namespace vkm
