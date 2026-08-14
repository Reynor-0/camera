cmake_minimum_required(VERSION 3.16)

project(camera_demo VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁止编译器退回到 gnu++11 扩展模式，确保目标程序只依赖标准 ISO C++11。
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAMERA_DEMO_WARNINGS_AS_ERRORS
       "Treat compiler warnings as errors" OFF)

option(CAMERA_DEMO_BUILD_DRM_PROBE
       "Build the DRM/KMS resource and dumb-buffer probe when libdrm is available" ON)
option(CAMERA_DEMO_REQUIRE_DRM_PROBE
       "Fail configuration when drm_probe dependencies are unavailable" OFF)

add_executable(camera_demo
    src/main.cpp
    src/v4l2_buffer.cpp
    src/v4l2_device.cpp
)

target_include_directories(camera_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

# 将 C++11 要求附着到具体 target。其他 target 后续加入项目时，也必须显式声明
# 自己需要的语言标准，避免开发机默认编译器版本掩盖目标板兼容性问题。
target_compile_features(camera_demo PRIVATE cxx_std_11)

target_compile_options(camera_demo PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 将版本作为普通字符串宏传给程序。命令行 `--version` 不依赖 Git 或构建主机上
# 额外生成的文件，因此也适用于离线的 RK3568 交叉编译环境。
target_compile_definitions(camera_demo PRIVATE
    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
)

if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(camera_demo PRIVATE -Werror)
endif()

# rk3568-aarch64.cmake 会设置该变量。这里执行第二层保护，防止 toolchain 文件被
# 错误修改为 32 位编译器后仍然生成无法用于预期 64 位系统的程序。
if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
    target_compile_definitions(camera_demo PRIVATE
        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
    )
endif()

if(CAMERA_DEMO_BUILD_DRM_PROBE)
    # xf86drm.h 位于普通 include 目录，而它引用的 drm.h 通常位于 include/libdrm。
    # 分开查找两者，避免宿主机和交叉 sysroot 的头文件被意外混用。
    find_path(LIBDRM_XF86_INCLUDE_DIR NAMES xf86drm.h)
    find_path(LIBDRM_DRM_INCLUDE_DIR NAMES drm.h PATH_SUFFIXES libdrm)
    find_library(LIBDRM_LIBRARY NAMES drm)

    if(LIBDRM_XF86_INCLUDE_DIR AND LIBDRM_DRM_INCLUDE_DIR AND LIBDRM_LIBRARY)
        add_executable(drm_probe
            src/drm_device.cpp
            src/drm_display.cpp
            src/drm_test_main.cpp
        )

        target_include_directories(drm_probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inc
            ${LIBDRM_XF86_INCLUDE_DIR}
            ${LIBDRM_DRM_INCLUDE_DIR}
        )
        target_link_libraries(drm_probe PRIVATE ${LIBDRM_LIBRARY})
        target_compile_features(drm_probe PRIVATE cxx_std_11)
        target_compile_options(drm_probe PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        target_compile_definitions(drm_probe PRIVATE
            CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
        )

        if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
            target_compile_options(drm_probe PRIVATE -Werror)
        endif()
        if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
            target_compile_definitions(drm_probe PRIVATE
                CAMERA_DEMO_TARGET_RK3568_AARCH64=1
            )
        endif()

        install(TARGETS drm_probe RUNTIME DESTINATION bin)
    elseif(CAMERA_DEMO_REQUIRE_DRM_PROBE)
        message(FATAL_ERROR
            "drm_probe requires xf86drm.h, libdrm/drm.h and libdrm")
    else()
        message(WARNING
            "libdrm development files were not found; drm_probe is disabled")
    endif()
endif()

# `cmake --install` 会把程序放入 staging_dir/bin，便于之后统一复制到开发板。
install(TARGETS camera_demo RUNTIME DESTINATION bin)
