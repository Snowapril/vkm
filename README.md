# vkm

[![CodeFactor](https://www.codefactor.io/repository/github/snowapril/vkm/badge)](https://www.codefactor.io/repository/github/snowapril/vkm)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/c2815435af9745a78831c15659dd996a)](https://www.codacy.com/gh/snowapril/vkm/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=snowapril/vkm&amp;utm_campaign=Badge_Grade)
![Ubuntu github action](https://github.com/snowapril/vkm/actions/workflows/ubuntu.yml/badge.svg?branch=main)
![Window github action](https://github.com/snowapril/vkm/actions/workflows/window.yml/badge.svg?branch=main)
![MacOS github action](https://github.com/snowapril/vkm/actions/workflows/macos.yml/badge.svg?branch=main)

## QuickStart
```bash
git clone https://github.com/snowapril/vkm.git
cd vkm
python bootstrap.py
mkdir build
cd build
cmake .. && cmake --build .
```

## Dependencies
*   [vulkan-hpp](https://github.com/KhronosGroup/Vulkan-Hpp)
*   [glfw](https://github.com/glfw/glfw)
*   [glm](https://github.com/g-truc/glm)
*   [imgui](https://github.com/ocornut/imgui)
*   [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)
*   [stb](https://github.com/nothings/stb)
*   [taskflow](https://github.com/taskflow/taskflow)
*   [meshoptimizer](https://github.com/zeux/meshoptimizer)
*   [glslang](https://github.com/KhronosGroup/glslang)
*   [doctest](https://github.com/doctest/doctest)

## How To Contribute

Contributions are always welcome, either reporting issues/bugs or forking the repository and then issuing pull requests when you have completed some additional coding that you feel will be beneficial to the main project. If you are interested in contributing in a more dedicated capacity, then please contact me.

## Contact

You can contact me via e-mail (sinjihng at gmail.com). I am always happy to answer questions or help with any issues you might have, and please be sure to share any additional work or your creations with me, I love seeing what other people are making.

## License

<img align="right" src="http://opensource.org/trademarks/opensource/OSI-Approved-License-100x137.png">

The class is licensed under the [MIT License](http://opensource.org/licenses/MIT):

Copyright (c) 2024 Snowapril

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
