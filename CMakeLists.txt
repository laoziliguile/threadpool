cmake_minimum_required(VERSION 3.20)

# ============================================================================
#  ThreadForge —— 进阶多线程池 + 桌面可视化实验台
#  纯 C++17，仅依赖 Windows 系统库（D3D11 / DWM / GDI）。
# ============================================================================
project(threadforge
        VERSION 0.1.0
        DESCRIPTION "Advanced thread pool with dynamic scaling, priority scheduling and live dashboard"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# 所有可执行文件统一输出到 <build>/bin，方便 VSCode 调试路径固定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Threads REQUIRED)

# 我们自己写的代码开启严格警告；第三方代码不插手
add_library(threadforge_warnings INTERFACE)
target_compile_options(threadforge_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>)

# 【踩坑记录】本项目放在中文目录下时，MinGW 的 __FILE__ 会带上非 ASCII 路径，
# 而 assert()/IM_ASSERT() 会把 __FILE__ 变成字符串常量，编译器尝试把它从
# “执行字符集”里转换时会报：converting to execution character set: Illegal byte sequence
# 解决：把源码根目录前缀映射成 "."，这样 __FILE__ 又变回相对路径（纯 ASCII），
# 顺带让 gdb 里的断点路径更短。
if(MINGW)
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

# ---------------------------------------------------------------------------
#  tp_core：线程池内核（纯标准库，可单独测试、可在无界面环境运行）
# ---------------------------------------------------------------------------
add_library(tp_core STATIC
  src/config.cpp
  src/priority_queue.cpp
  src/worker.cpp
  src/pool.cpp
  src/event_log.cpp
  src/simulator.cpp)

target_include_directories(tp_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(tp_core PUBLIC Threads::Threads PRIVATE threadforge_warnings)
# Windows 下 socket/线程库；其余平台 CMake 自动处理
target_compile_definitions(tp_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
#  imgui：本地内置的 Dear ImGui（固定版本，不依赖网络）
# ---------------------------------------------------------------------------
add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp)

target_include_directories(imgui PUBLIC
  "${CMAKE_SOURCE_DIR}/third_party/imgui"
  "${CMAKE_SOURCE_DIR}/third_party/imgui/backends")

target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi gdi32 imm32 ole32)

# ---------------------------------------------------------------------------
#  threadforge_app：桌面仪表盘（Win32 窗口 + D3D11 + ImGui）
# ---------------------------------------------------------------------------
add_executable(threadforge_app WIN32
  app/main.cpp
  app/theme.cpp
  app/dashboard.cpp)

# app/ 目录本身要进头文件搜索路径（main.cpp → "app/dashboard.hpp"），
# 所以源码根目录也要加进去（theme.cpp → "app/theme.hpp"）
target_include_directories(threadforge_app PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(threadforge_app PRIVATE tp_core imgui threadforge_warnings)
target_compile_definitions(threadforge_app PRIVATE UNICODE _UNICODE)
if(MINGW)
  # wWinMain 入口 + 不弹控制台窗口
  target_link_options(threadforge_app PRIVATE -municode)
endif()

# ---------------------------------------------------------------------------
#  threadforge_cli：无界面演示 / 压测基准（学习时先跑这个，再看界面）
# ---------------------------------------------------------------------------
add_executable(threadforge_cli apps/cli_main.cpp)
target_link_libraries(threadforge_cli PRIVATE tp_core threadforge_warnings)

# ---------------------------------------------------------------------------
#  单元测试（CTest 驱动）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(tp_tests
  tests/test_main.cpp
  tests/test_priority_queue.cpp
  tests/test_pool.cpp)
target_include_directories(tp_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(tp_tests PRIVATE tp_core threadforge_warnings)
add_test(NAME tp_tests COMMAND tp_tests)
# 并发用例一旦踩到死锁（比如析构没唤醒睡着的线程），进程会一直卡住。
# 给个超时，让"卡死"变成一个明确的失败，而不是让 CI 干等。
set_tests_properties(tp_tests PROPERTIES TIMEOUT 60)

# ---------------------------------------------------------------------------
#  便捷目标：cmake --build build --target run_app
# ---------------------------------------------------------------------------
add_custom_target(run_app COMMAND threadforge_app DEPENDS threadforge_app USES_TERMINAL)
add_custom_target(run_cli COMMAND threadforge_cli DEPENDS threadforge_cli USES_TERMINAL)
