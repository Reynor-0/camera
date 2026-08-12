# RK3568 64-bit Linux 交叉编译工具链。
#
# 使用示例：
#   cmake -S . -B build-rk3568 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3568-aarch64.cmake \
#     -DRK3568_CROSS_COMPILE=/opt/rk-sdk/toolchain/bin/aarch64-linux-gnu- \
#     -DRK3568_SYSROOT=/opt/rk-sdk/sysroot
#
# 也可以通过同名环境变量传入参数。RK3568_CROSS_COMPILE 是工具名称的完整前缀，
# 末尾必须包含连字符，例如 `aarch64-linux-gnu-`。如果编译器已经在 PATH 中，默认
# 值即可使用。

# 告诉 CMake：编译产物运行在 Linux/AArch64，而不是当前构建主机。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 将 RK3568 标记传递给项目 CMakeLists.txt，用于 ABI 检查和构建信息输出。
set(CAMERA_DEMO_TARGET_RK3568_AARCH64 ON CACHE BOOL
    "Build camera_demo for a 64-bit RK3568 Linux target" FORCE)

# SDK 的编译器有时不能在 CMake 配置阶段链接一个可执行文件，因为链接还依赖目标
# rootfs。让 try_compile 只生成静态库，可以避免 CMake 在检测编译器时尝试运行或
# 链接目标程序；真正的 camera_demo 仍会在正常 build 阶段完成链接。
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 参数优先级：CMake -D 参数 > 同名环境变量 > PATH 中的 GNU AArch64 工具链。
if(NOT DEFINED RK3568_CROSS_COMPILE OR RK3568_CROSS_COMPILE STREQUAL "")
    if(DEFINED ENV{RK3568_CROSS_COMPILE} AND
       NOT "$ENV{RK3568_CROSS_COMPILE}" STREQUAL "")
        set(RK3568_CROSS_COMPILE "$ENV{RK3568_CROSS_COMPILE}")
    else()
        set(RK3568_CROSS_COMPILE "aarch64-linux-gnu-")
    endif()
endif()
set(RK3568_CROSS_COMPILE "${RK3568_CROSS_COMPILE}" CACHE STRING
    "Full prefix of the RK3568 cross tools, including the trailing dash")

set(CMAKE_C_COMPILER "${RK3568_CROSS_COMPILE}gcc")
set(CMAKE_CXX_COMPILER "${RK3568_CROSS_COMPILE}g++")
set(CMAKE_AR "${RK3568_CROSS_COMPILE}ar")
set(CMAKE_RANLIB "${RK3568_CROSS_COMPILE}ranlib")
set(CMAKE_STRIP "${RK3568_CROSS_COMPILE}strip")

# Sysroot 应来自目标板 rootfs 或与 BSP 完全匹配的 SDK。它决定编译时使用哪些目标
# 头文件和动态库。当前 V4L2 probe 只依赖 libc 和内核 UAPI，但后续加入 libdrm
# 后必须提供带有目标板 libdrm 的 sysroot，绝不能误链接宿主机 x86_64 库。
if(NOT DEFINED RK3568_SYSROOT OR RK3568_SYSROOT STREQUAL "")
    if(DEFINED ENV{RK3568_SYSROOT} AND
       NOT "$ENV{RK3568_SYSROOT}" STREQUAL "")
        set(RK3568_SYSROOT "$ENV{RK3568_SYSROOT}")
    endif()
endif()

if(DEFINED RK3568_SYSROOT AND NOT RK3568_SYSROOT STREQUAL "")
    if(NOT IS_DIRECTORY "${RK3568_SYSROOT}")
        message(FATAL_ERROR
            "RK3568_SYSROOT is not a directory: ${RK3568_SYSROOT}")
    endif()

    # CMAKE_SYSROOT 会向编译器传递 --sysroot，并作为 find_library/find_path 的根。
    set(CMAKE_SYSROOT "${RK3568_SYSROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${RK3568_SYSROOT}")

    # pkg-config 默认读取宿主机 .pc 文件。为未来的 libdrm 查找显式限制其搜索范围。
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${RK3568_SYSROOT}")
    set(ENV{PKG_CONFIG_LIBDIR}
        "${RK3568_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${RK3568_SYSROOT}/usr/lib/pkgconfig:${RK3568_SYSROOT}/usr/share/pkgconfig")
endif()

# 构建期间需要运行的 cmake/pkg-config 等程序来自主机；头文件、库和 CMake package
# 必须来自目标 sysroot。这可避免不小心把 x86_64 libdrm 链入 ARM 可执行文件。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# AArch64 编译器本身已经保证生成 ARMv8 指令，因此默认不额外指定 -mcpu，兼容较旧
# 的 BSP 工具链。如果需要针对 Cortex-A55 优化，可以传入：
#   -DRK3568_CPU_FLAGS=-mcpu=cortex-a55
set(RK3568_CPU_FLAGS "" CACHE STRING
    "Optional target CPU flags, for example -mcpu=cortex-a55")
if(NOT RK3568_CPU_FLAGS STREQUAL "")
    set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} ${RK3568_CPU_FLAGS}")
    set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} ${RK3568_CPU_FLAGS}")
endif()
