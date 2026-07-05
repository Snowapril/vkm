# CLAUDE.md

```
vkm/
├── include/vkm/
│   ├── base       
│   ├── platform    # Platform specific headers such as windowing
│   │   ├── common  # Common platform headers that are shared across macOS/iOS/windows, etc.
│   │   ├── apple   # macOS/iOS specific platform headers
│   │   ├── windows # Windows specific platform headers
│   │   ├── wasm    # Emscripten/WASM specific platform headers (GLFW via Emscripten port)
│   ├── renderer
│   │   ├── backend
│   │   │   ├── common  # Common renderer backend headers that are shared across metal/vulkan, etc.
│   │   │   ├── metal   # Metal specific renderer headers
│   │   │   ├── vulkan  # Vulkan specific renderer headers
│   │   │   └── webgpu  # WebGPU specific renderer headers (Emscripten/WASM only)
```