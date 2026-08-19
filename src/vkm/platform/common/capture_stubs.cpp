// Copyright (c) 2025 Snowapril
//
// The factories for platforms that have no camera or no hand tracker. Returning null is the
// documented contract, not a failure: a caller is expected to check and supply its own input.

#include <vkm/platform/common/hand_tracker.h>
#include <vkm/platform/common/video_capture.h>

namespace vkm
{
#if !defined(VKM_PLATFORM_APPLE) && !defined(VKM_PLATFORM_WASM)
    // Capture exists on Apple (AVFoundation) and in the browser (getUserMedia). Windows would
    // want Media Foundation and Linux V4L2; neither is written.
    VkmVideoCaptureBase* vkmCreateVideoCapture()
    {
        return nullptr;
    }
#endif

#if !defined(VKM_PLATFORM_APPLE) && !defined(VKM_PLATFORM_WASM)
    // Apple tracks hands through Vision, which ships with the OS, and the browser through a
    // MediaPipe model fetched by scripts/download_hand_model.py. A native Windows or Linux
    // tracker would mean bundling a model and an inference runtime into the engine itself, which
    // is a far larger decision than this interface and is deliberately not made here.
    VkmHandTrackerBase* vkmCreateHandTracker()
    {
        return nullptr;
    }
#endif
} // namespace vkm
