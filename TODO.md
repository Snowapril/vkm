# TODO.md

## Known Limitations

- **Vulkan sample builds fail to link on macOS.** `src/vkm/platform/apple/application.mm`
  (the Apple windowing/render-coordinator code) references the Metal-specific
  `VkmDriverMetal`/`VkmSwapChainMetal` types directly, regardless of which backend is
  selected. A `VKM_USE_VULKAN_API=ON -DVKM_USE_METAL_API=OFF` build never compiles those
  Metal types in, so linking a windowed target (e.g. the `triangle` sample) fails with
  undefined symbols. Does not affect Vulkan on Linux/Windows, or the `UnitTests` target on
  macOS (it doesn't create a window). Fix requires updating `application.mm` to dispatch to
  the active backend's driver/swapchain type instead of hard-coding Metal. See the
  "Native Backends" section of README.md for the current workaround (use Metal for the
  sample on macOS).
