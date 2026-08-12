cmake_minimum_required(VERSION 3.16)

project(camera_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁止编译器退回到 gnu++11 扩展模式，确保目标程序只依赖标准 ISO C++11。
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAMERA_DEMO_WARNINGS_AS_ERRORS
       "Treat compiler warnings as errors" OFF)

add_executable(camera_demo
    src/main.cpp
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

# `cmake --install` 会把程序放入 staging_dir/bin，便于之后统一复制到开发板。
install(TARGETS camera_demo RUNTIME DESTINATION bin)
