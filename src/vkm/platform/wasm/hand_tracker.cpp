// Copyright (c) 2025 Snowapril
//
// Browser hand tracking through MediaPipe's HandLandmarker, which reports the same 21-joint
// skeleton in the same order VkmHandJoint declares -- wrist first, then thumb to little finger,
// each base to tip -- so the two zip together with no remapping.
//
// Nothing here ships with the browser, unlike Vision on Apple platforms. The model and the
// tasks-vision runtime are fetched by scripts/download_hand_model.py into resources/HandTracking
// and copied next to the page at configure time. When they are absent the detector never becomes
// ready and its caller falls back to whatever input it has, which is why the sample still builds
// and runs on a checkout that never ran the script.
//
// Detection runs in a Web Worker, for two independent reasons. MediaPipe's runtime is itself an
// Emscripten module, and on the main thread it adopts this page's `Module` global as its own --
// then aborts the whole page over a missing `printErr` export. A worker has its own global scope,
// so the two runtimes cannot see each other at all. It also keeps a detection that takes tens of
// milliseconds off the thread driving the render loop.

#include <vkm/platform/common/hand_tracker.h>

#include <vkm/base/common.h>

#include <emscripten/emscripten.h>

#include <cstdint>
#include <vector>

namespace vkm
{
    namespace
    {
        // MediaPipe reports one score per hand rather than per joint, so this gates the whole
        // pose. It is a different quantity from Vision's per-joint confidence: a detection the
        // model is only half sure of is not worth pushing anything with.
        constexpr float kHandScoreFloor = 0.50f;

        /*
        * @brief Starts the worker and tells it to load the runtime and the model.
        * @details Returns as soon as the worker is spawned; the import, the wasm instantiation
        * and the model download all resolve inside it later.
        * @return 1 when the worker was started, 0 when the page cannot host one.
        */
        EM_JS(int, vkmWasmHandStart, (), {
            if (typeof Worker === "undefined" || typeof document === "undefined") { return 0; }
            if (Module.vkmHand && Module.vkmHand.worker) { return 1; }

            // Every URL handed to the worker is absolute. A blob worker's own base URL is the
            // blob, so a relative path would resolve against nothing useful.
            var base = new URL("./hand_tracking/", location.href).href;

            var source = [
                "let landmarker = null;",
                "self.onmessage = async (event) => {",
                "  const message = event.data;",
                "  if (message.cmd === 'init') {",
                "    try {",
                "      const vision = await import(message.bundleUrl);",
                "      const fileset = await vision.FilesetResolver.forVisionTasks(message.wasmUrl);",
                "      landmarker = await vision.HandLandmarker.createFromOptions(fileset, {",
                "        baseOptions: { modelAssetPath: message.modelUrl, delegate: 'CPU' },",
                "        runningMode: 'VIDEO',",
                "        numHands: 1,",
                "      });",
                "      self.postMessage({ type: 'ready' });",
                "    } catch (err) {",
                "      self.postMessage({ type: 'failed', reason: String(err) });",
                "    }",
                "    return;",
                "  }",
                "  if (message.cmd === 'frame') {",
                "    if (!landmarker) { self.postMessage({ type: 'none' }); return; }",
                "    const image = new ImageData(new Uint8ClampedArray(message.pixels), message.width, message.height);",
                "    if (!self.canvas || self.canvas.width !== message.width || self.canvas.height !== message.height) {",
                "      self.canvas = new OffscreenCanvas(message.width, message.height);",
                "      self.context = self.canvas.getContext('2d', { willReadFrequently: true });",
                "    }",
                "    self.context.putImageData(image, 0, 0);",
                "    let result = null;",
                "    try { result = landmarker.detectForVideo(self.canvas, message.timestamp); }",
                "    catch (err) { self.postMessage({ type: 'none' }); return; }",
                "    if (!result || !result.landmarks || result.landmarks.length === 0) {",
                "      self.postMessage({ type: 'none' }); return;",
                "    }",
                "    const points = result.landmarks[0];",
                "    if (points.length < 21) { self.postMessage({ type: 'none' }); return; }",
                "    const flat = new Float32Array(42);",
                "    for (let i = 0; i < 21; ++i) { flat[i * 2] = points[i].x; flat[i * 2 + 1] = points[i].y; }",
                "    let score = 1.0;",
                "    if (result.handedness && result.handedness.length > 0 && result.handedness[0].length > 0) {",
                "      score = result.handedness[0][0].score;",
                "    }",
                "    self.postMessage({ type: 'pose', joints: flat, score: score }, [flat.buffer]);",
                "  }",
                "};",
            ].join("\n");

            var state = {
                worker: null,
                ready: false,
                failed: false,
                busy: false,
                joints: null,
                score: 0,
                hasPose: false,
            };
            Module.vkmHand = state;

            // Deliberately a *classic* worker, not a module one. MediaPipe's wasm loader calls
            // importScripts(), which a module worker forbids outright; a classic worker allows it
            // and still supports the dynamic import() below, so this is the only kind that can
            // load both halves. The package ships no UMD bundle, so importScripts() cannot stand
            // in for that import.
            var blob = new Blob([source], { type: "text/javascript" });
            state.worker = new Worker(URL.createObjectURL(blob));

            state.worker.onmessage = function(event) {
                var message = event.data;
                if (message.type === "ready") { state.ready = true; return; }
                if (message.type === "failed") {
                    state.failed = true;
                    console.error("vkm hand tracker: " + message.reason);
                    return;
                }
                state.busy = false;
                if (message.type === "pose") {
                    state.joints = message.joints;
                    state.score = message.score;
                    state.hasPose = true;
                }
            };
            state.worker.onerror = function(err) {
                state.failed = true;
                console.error("vkm hand tracker worker: " + err.message);
            };

            state.worker.postMessage({
                cmd: "init",
                bundleUrl: base + "vision_bundle.mjs",
                wasmUrl: base + "wasm",
                modelUrl: base + "hand_landmarker.task",
            });
            return 1;
        });

        /*
        * @brief Hands one frame of tightly packed RGBA to the worker, unless one is still in
        * flight.
        * @return 1 when the frame was sent, 0 when it was dropped.
        */
        EM_JS(int, vkmWasmHandSubmit, (const uint8_t* pixels, int width, int height, double timestampMs), {
            var state = Module.vkmHand;
            if (!state || !state.ready || state.busy) { return 0; }
            state.busy = true;
            // Copied out of the heap and transferred, so the worker owns the bytes outright and
            // nothing is shared across the boundary.
            var bytes = new Uint8Array(HEAPU8.subarray(pixels, pixels + width * height * 4));
            state.worker.postMessage(
                { cmd: "frame", pixels: bytes.buffer, width: width, height: height, timestamp: timestampMs },
                [bytes.buffer]);
            return 1;
        });

        /*
        * @brief Takes the newest completed detection into the heap.
        * @return 1 when a pose was written, 0 when none has completed since the last call.
        */
        EM_JS(int, vkmWasmHandPoll, (float* outJoints, float* outScore), {
            var state = Module.vkmHand;
            if (!state || !state.hasPose) { return 0; }
            state.hasPose = false;
            for (var i = 0; i < 42; ++i) { HEAPF32[(outJoints >> 2) + i] = state.joints[i]; }
            HEAPF32[outScore >> 2] = state.score;
            return 1;
        });

        // Whether loading the runtime or the model failed outright.
        EM_JS(int, vkmWasmHandFailed, (), {
            var state = Module.vkmHand;
            return state && state.failed ? 1 : 0;
        });

        EM_JS(void, vkmWasmHandStop, (), {
            var state = Module.vkmHand;
            if (!state || !state.worker) { return; }
            state.worker.terminate();
            state.worker = null;
            state.ready = false;
        });

        class WasmHandTracker final : public VkmHandTrackerBase
        {
        public:
            ~WasmHandTracker() override { stop(); }

            bool start(std::string* outError) override
            {
                if (vkmWasmHandStart() == 0)
                {
                    *outError = "this page cannot host a hand tracking worker";
                    return false;
                }
                _started = true;
                return true;
            }

            void stop() override
            {
                if (_started)
                {
                    vkmWasmHandStop();
                    _started = false;
                }
            }

            void submitFrame(const VkmVideoFrame& frame) override
            {
                if (!_started || frame._width == 0 || frame._height == 0)
                {
                    return;
                }
                if (frame._format != VkmFormat::R8G8B8A8_UNORM)
                {
                    VKM_DEBUG_ERROR("The browser hand tracker needs RGBA frames");
                    return;
                }

                reportFailureOnce();
                // Dropping while the worker is busy is the backpressure: queueing would only
                // build a backlog of poses already stale by the time they came back.
                vkmWasmHandSubmit(frame._pixels.data(), static_cast<int>(frame._width),
                                  static_cast<int>(frame._height), emscripten_get_now());
            }

            bool tryAcquirePose(VkmHandPose* outPose) override
            {
                float joints[kVkmHandJointCount * 2] = {};
                float score = 0.0f;
                if (vkmWasmHandPoll(joints, &score) == 0)
                {
                    return false;
                }

                VkmHandPose pose;
                for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
                {
                    pose._joints[i] = glm::vec2(joints[i * 2 + 0], joints[i * 2 + 1]);
                    // MediaPipe scores a whole hand rather than each joint, so every joint
                    // carries the hand's score rather than one of its own.
                    pose._confidence[i] = score;
                }
                pose._valid = score >= kHandScoreFloor;

                *outPose = pose;
                return true;
            }

            const char* getName() const override { return "MediaPipe hand landmarker"; }

        private:
            /*
            * @brief Logs a failed runtime or model load, once.
            * @details The worker reports it long after start() returned, so this is the only
            * place a missing resources/HandTracking can surface.
            */
            void reportFailureOnce()
            {
                if (_reportedFailure || vkmWasmHandFailed() == 0)
                {
                    return;
                }
                _reportedFailure = true;
                VKM_DEBUG_ERROR("Hand tracking assets missing or unreadable; run "
                                "scripts/download_hand_model.py and reconfigure");
            }

            bool _started = false;
            bool _reportedFailure = false;
        };
    } // namespace

    VkmHandTrackerBase* vkmCreateHandTracker()
    {
        return new WasmHandTracker();
    }
} // namespace vkm
