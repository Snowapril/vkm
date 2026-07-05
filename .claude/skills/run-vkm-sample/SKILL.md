---
name: run-vkm-sample
description: Build and launch a vkm graphics sample against a specific backend (metal, vulkan, or webgpu) by running bootstrap.py and then the matching scripts/run_sample.* wrapper. Use this whenever the user asks to run/launch/try/build a sample against a backend or renderer — e.g. "run sample triangle with metal backend", "try the vulkan triangle sample", "launch triangle on webgpu in release mode" — even if they phrase it loosely or don't name the script. Sample names and backend names vary per request; always read them from the user's phrasing rather than assuming "triangle"/"metal".
---

## What this does

vkm samples live under `src/samples/<name>/` and each can target three graphics
backends: `metal` (macOS only), `vulkan`, or `webgpu` (built via Emscripten).
`scripts/run_sample.{sh,ps1,py}` configures CMake, builds the sample's target,
and launches it — a native binary for metal/vulkan, or a local web server +
browser tab for webgpu. This skill runs the full pipeline: bootstrap
dependencies, then build+run the requested sample/backend combination.

## Steps

1. **Bootstrap dependencies first, every time.**
   Run `python3 bootstrap.py` (or `python bootstrap.py` on Windows) from the
   project root before invoking run_sample. It is idempotent — safe to
   re-run even when dependencies already look installed — and provides
   things run_sample depends on (e.g. `dependencies/src/emsdk` for the
   webgpu backend). Treat it as mandatory setup, not a step to skip just
   because a build directory already exists.

2. **Extract backend and sample name from the request.** Don't hardcode
   assumptions — the user picks both freely per request.
   - `backend`: one of `metal`, `vulkan`, `webgpu`.
   - `sample`: a directory name under `src/samples/`. If you're unsure it
     exists, check `src/samples/*/CMakeLists.txt` — but don't block on this,
     since run_sample itself validates the sample and prints the available
     list on error, so a stale hardcoded list of sample names is never the
     source of truth.
   - If the backend is unsupported on the current platform (e.g. `metal` on
     Linux/Windows), don't try to swap it silently — let the script's own
     platform check reject it with its error message.

3. **Pick the OS-appropriate wrapper script:**

   | Platform | Script |
   |----------|--------|
   | macOS / Linux | `scripts/run_sample.sh` |
   | Windows | `scripts/run_sample.ps1` |

   Both are thin wrappers around `scripts/run_sample.py` (which they call
   directly for the `webgpu` backend) — reach for `run_sample.py` yourself
   only if there's no shell available at all.

4. **Pass through any extra options the user actually mentioned:**

   | If the user says... | Add this flag |
   |----------------------|----------------|
   | "release" / "release mode" / "optimized" | `--build-type Release` |
   | "debug" / "debug mode" | `--build-type Debug` (already the default) |
   | a specific job/thread count ("use 8 jobs", "-j8") | `--jobs <n>` |
   | a custom build directory | `--build-dir <path>` |

   Leave a flag off entirely when the user didn't mention it — let the
   script's own defaults apply rather than inventing a value.

5. **Run it in the background, not blocking in the foreground.** Both
   launch paths hang the calling process on purpose:
   - `metal`/`vulkan` launch a GUI binary directly and block until the user
     closes its window.
   - `webgpu` starts a local server and blocks on `input()` waiting for
     Enter to shut it down.

   Start the command as a background process so the conversation isn't
   stuck waiting on a GUI window or a server that's meant to stay up, e.g.:
   ```
   ./scripts/run_sample.sh --backend metal --sample triangle
   ```
   run with a background-capable execution mode, then report back what was
   launched (and, for webgpu, the local URL it will serve once the build
   finishes) rather than waiting on the process to exit.

## Notes on backend requirements

- `metal` requires macOS with the macOS 26 SDK (Metal 4 / `MTL4CommandBuffer.h`
  headers); the script already checks this and prints a clear error if it's
  missing — surface that message rather than trying to work around it.
- `vulkan` requires `VULKAN_SDK` set or `vulkaninfo` on PATH.
- `webgpu` requires `dependencies/src/emsdk`, which step 1's bootstrap run
  provides.
