// Copyright (c) 2025 Snowapril
//
// Browser camera capture: getUserMedia into an offscreen <video>, drawn into a 2D canvas so its
// pixels can be read back as RGBA.
//
// The whole JS side is non-blocking. getUserMedia returns a promise, and nothing here waits on
// it: start() kicks it off and returns, and the poll below reports no frame until the stream is
// live. That keeps this off ASYNCIFY, which the WebGPU driver already leans on heavily, and it
// keeps the browser's main thread -- the one running the render loop -- free the whole time.
//
// Reading pixels back through a canvas is the only portable route. There is no way to hand a
// MediaStream's texture to WebGPU without importing an external texture, which the engine's
// resource model has no representation for, so the frame takes the same CPU trip every other
// platform's does.

#include <vkm/platform/common/video_capture.h>

#include <vkm/base/common.h>

#include <emscripten/emscripten.h>

#include <cstdint>

namespace vkm
{
    namespace
    {
        /*
        * @brief Starts the camera. Returns at once; the stream arrives later.
        * @return 1 when the request was made, 0 when the browser exposes no camera API.
        */
        EM_JS(int, vkmWasmVideoStart, (), {
            // navigator.mediaDevices is undefined outside a secure context, which is anything
            // other than https or a localhost origin.
            if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
                return 0;
            }
            if (!Module.vkmVideo) {
                Module.vkmVideo = {
                    element: document.createElement("video"),
                    canvas: document.createElement("canvas"),
                    ready: false,
                    error: null,
                    sequence: 0,
                    lastDrawnTime: -1,
                };
                Module.vkmVideo.element.autoplay = true;
                Module.vkmVideo.element.muted = true;
                Module.vkmVideo.element.playsInline = true;
            }
            var state = Module.vkmVideo;
            navigator.mediaDevices.getUserMedia({ video: { width: 1280, height: 720 }, audio: false })
                .then(function(stream) {
                    state.element.srcObject = stream;
                    return state.element.play();
                })
                .then(function() { state.ready = true; })
                .catch(function(err) { state.error = String(err && err.name ? err.name : err); });
            return 1;
        });

        /*
        * @brief Whether a camera frame the poll has not yet returned is available, and how big it
        * is. Writes 0 into both sizes when nothing is ready.
        */
        EM_JS(int, vkmWasmVideoPollSize, (int* outWidth, int* outHeight), {
            var state = Module.vkmVideo;
            if (!state || !state.ready) { HEAP32[outWidth >> 2] = 0; HEAP32[outHeight >> 2] = 0; return 0; }
            var video = state.element;
            var width = video.videoWidth | 0;
            var height = video.videoHeight | 0;
            if (width <= 0 || height <= 0) { HEAP32[outWidth >> 2] = 0; HEAP32[outHeight >> 2] = 0; return 0; }
            // currentTime only advances when the element has decoded a new frame, so it is what
            // distinguishes a fresh frame from the render loop simply running faster than the
            // camera.
            if (video.currentTime === state.lastDrawnTime) { HEAP32[outWidth >> 2] = 0; HEAP32[outHeight >> 2] = 0; return 0; }
            HEAP32[outWidth >> 2] = width;
            HEAP32[outHeight >> 2] = height;
            return 1;
        });

        /*
        * @brief Copies the current frame into the wasm heap as tightly packed RGBA.
        * @return 1 on success, 0 when the frame went away between the size poll and this call.
        */
        EM_JS(int, vkmWasmVideoRead, (uint8_t* destination, int capacityBytes), {
            var state = Module.vkmVideo;
            if (!state || !state.ready) { return 0; }
            var video = state.element;
            var width = video.videoWidth | 0;
            var height = video.videoHeight | 0;
            if (width <= 0 || height <= 0 || width * height * 4 > capacityBytes) { return 0; }

            var canvas = state.canvas;
            if (canvas.width !== width || canvas.height !== height) {
                canvas.width = width;
                canvas.height = height;
                state.context = null;
            }
            if (!state.context) {
                // willReadFrequently keeps the canvas on a software backing, which is what makes
                // the getImageData below cheap rather than a GPU readback stall every frame.
                state.context = canvas.getContext("2d", { willReadFrequently: true });
            }
            if (!state.context) { return 0; }

            state.context.drawImage(video, 0, 0, width, height);
            var image = state.context.getImageData(0, 0, width, height);
            HEAPU8.set(image.data, destination);
            state.lastDrawnTime = video.currentTime;
            state.sequence = (state.sequence + 1) | 0;
            return 1;
        });

        /*
        * @brief What the camera request failed with, as a code rather than a string: writing a
        * string back would pull in a runtime helper that is otherwise dead-code-eliminated.
        * @return 0 no failure yet, 1 refused by the user, 2 no camera, 3 anything else.
        */
        EM_JS(int, vkmWasmVideoError, (), {
            var state = Module.vkmVideo;
            if (!state || !state.error) { return 0; }
            if (state.error === "NotAllowedError" || state.error === "SecurityError") { return 1; }
            if (state.error === "NotFoundError" || state.error === "DevicesNotFoundError") { return 2; }
            return 3;
        });

        EM_JS(void, vkmWasmVideoStop, (), {
            var state = Module.vkmVideo;
            if (!state) { return; }
            var stream = state.element.srcObject;
            if (stream) {
                stream.getTracks().forEach(function(track) { track.stop(); });
                state.element.srcObject = null;
            }
            state.ready = false;
        });

        class WasmVideoCapture final : public VkmVideoCaptureBase
        {
        public:
            ~WasmVideoCapture() override { stop(); }

            bool start(std::string* outError) override
            {
                if (vkmWasmVideoStart() == 0)
                {
                    *outError = "this page has no camera API; getUserMedia needs a secure context "
                                "(https, or a localhost origin)";
                    return false;
                }
                _started = true;
                return true;
            }

            void stop() override
            {
                if (_started)
                {
                    vkmWasmVideoStop();
                    _started = false;
                }
            }

            bool tryAcquireFrame(VkmVideoFrame* outFrame) override
            {
                if (!_started)
                {
                    return false;
                }

                int width = 0;
                int height = 0;
                if (vkmWasmVideoPollSize(&width, &height) == 0)
                {
                    reportPermissionFailureOnce();
                    return false;
                }

                const size_t byteSize = static_cast<size_t>(width) * height * 4;
                _pixels.resize(byteSize);
                if (vkmWasmVideoRead(_pixels.data(), static_cast<int>(byteSize)) == 0)
                {
                    return false;
                }

                outFrame->_pixels = _pixels;
                outFrame->_width = static_cast<uint32_t>(width);
                outFrame->_height = static_cast<uint32_t>(height);
                // A canvas hands back RGBA; every backend samples it as readily as BGRA, so the
                // channel order travels with the frame instead of being converted here.
                outFrame->_format = VkmFormat::R8G8B8A8_UNORM;
                outFrame->_sequence = ++_sequence;
                return true;
            }

            const char* getName() const override { return "Browser camera (getUserMedia)"; }

        private:
            /*
            * @brief Logs a refused or failed camera request, once.
            * @details The refusal arrives on a promise long after start() returned, so this is the
            * only place it can surface.
            */
            void reportPermissionFailureOnce()
            {
                if (_reportedError)
                {
                    return;
                }
                const int code = vkmWasmVideoError();
                if (code == 0)
                {
                    return;
                }
                _reportedError = true;
                switch (code)
                {
                    case 1:  VKM_DEBUG_ERROR("Browser camera access was refused"); break;
                    case 2:  VKM_DEBUG_ERROR("No camera is attached to this machine"); break;
                    default: VKM_DEBUG_ERROR("The browser refused to open a camera"); break;
                }
            }

            std::vector<uint8_t> _pixels;
            uint64_t _sequence = 0;
            bool _started = false;
            bool _reportedError = false;
        };
    } // namespace

    VkmVideoCaptureBase* vkmCreateVideoCapture()
    {
        return new WasmVideoCapture();
    }
} // namespace vkm
