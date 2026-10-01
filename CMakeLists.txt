cmake_minimum_required(VERSION 3.16)

project(camera_demo VERSION 0.15.0 LANGUAGES CXX)

include(CTest)

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
option(CAMERA_DEMO_BUILD_RGA_DRM_TEST
       "Build the offline RGA-to-DRM test when librga is available" ON)
option(CAMERA_DEMO_REQUIRE_RGA_DRM_TEST
       "Fail configuration when rga_drm_test dependencies are unavailable" OFF)

add_library(parking_domain STATIC
    src/parking/domain/card.cpp
    src/parking/domain/coordinator.cpp
    src/parking/domain/fee.cpp
    src/parking/domain/model.cpp
)
target_include_directories(parking_domain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_compile_features(parking_domain PUBLIC cxx_std_11)
target_compile_options(parking_domain PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(parking_domain PRIVATE -Werror)
endif()

add_library(parking_recognition STATIC
    src/parking/recognition/fake.cpp
)
target_include_directories(parking_recognition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_link_libraries(parking_recognition PUBLIC parking_domain)
target_compile_features(parking_recognition PUBLIC cxx_std_11)
target_compile_options(parking_recognition PRIVATE -Wall -Wextra -Wpedantic)
if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(parking_recognition PRIVATE -Werror)
endif()

# SQLite 是 parkingd 持久化状态的必要依赖。RK3568 交叉构建只允许从 toolchain
# sysroot 查找；当前 WSL 缺少 libsqlite3-dev 头文件时，允许复用 Buildroot 的稳定
# SQLite C API 头文件，但仍链接宿主机自己的 x86_64 runtime library。
find_path(PARKING_SQLITE3_INCLUDE_DIR NAMES sqlite3.h
    HINTS
        /opt/atk-dlrk356x-toolchain/aarch64-buildroot-linux-gnu/sysroot/usr/include
)
find_library(PARKING_SQLITE3_LIBRARY NAMES sqlite3)
if(NOT PARKING_SQLITE3_LIBRARY AND NOT CMAKE_CROSSCOMPILING)
    find_file(PARKING_SQLITE3_LIBRARY NAMES libsqlite3.so.0
        PATHS
            /usr/lib/x86_64-linux-gnu
            /lib/x86_64-linux-gnu
        NO_DEFAULT_PATH
    )
endif()
if(NOT PARKING_SQLITE3_INCLUDE_DIR OR NOT PARKING_SQLITE3_LIBRARY)
    message(FATAL_ERROR
        "parkingd requires sqlite3.h and a SQLite library; install libsqlite3-dev "
        "for host builds or provide them in RK3568_SYSROOT")
endif()

# 不把整个 RK3568 sysroot include 目录注入宿主机构建；否则其中的 sys/stat.h 等
# 目标机 ABI 头会覆盖宿主机系统头。SQLite 的公开头只依赖标准 stdarg.h，因此将它
# 单独复制到构建目录，主机构建和交叉构建都只暴露这一份第三方 C API 声明。
set(PARKING_SQLITE3_ISOLATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party")
file(MAKE_DIRECTORY "${PARKING_SQLITE3_ISOLATED_INCLUDE_DIR}")
configure_file(
    "${PARKING_SQLITE3_INCLUDE_DIR}/sqlite3.h"
    "${PARKING_SQLITE3_ISOLATED_INCLUDE_DIR}/sqlite3.h"
    COPYONLY
)

add_library(parking_storage STATIC
    src/parking/storage/memory.cpp
    src/parking/storage/sqlite.cpp
)
target_include_directories(parking_storage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc
    PRIVATE ${PARKING_SQLITE3_ISOLATED_INCLUDE_DIR}
)
target_link_libraries(parking_storage PUBLIC
    parking_domain
    ${PARKING_SQLITE3_LIBRARY}
)
target_compile_features(parking_storage PUBLIC cxx_std_11)
target_compile_options(parking_storage PRIVATE -Wall -Wextra -Wpedantic)
if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(parking_storage PRIVATE -Werror)
endif()

add_library(parking_control STATIC
    src/parking/control/protocol.cpp
    src/parking/control/server.cpp
)
target_include_directories(parking_control PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_link_libraries(parking_control PUBLIC parking_domain)
target_compile_features(parking_control PUBLIC cxx_std_11)
target_compile_options(parking_control PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(parking_control PRIVATE -Werror)
endif()

add_executable(parkingd src/parking/apps/parkingd.cpp)
target_link_libraries(parkingd PRIVATE
    parking_control
    parking_recognition
    parking_storage
    parking_domain
)
target_compile_features(parkingd PRIVATE cxx_std_11)
target_compile_options(parkingd PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(parkingd PRIVATE
    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
)

add_executable(parkingctl src/parking/apps/parkingctl.cpp)
target_link_libraries(parkingctl PRIVATE parking_control parking_domain)
target_compile_features(parkingctl PRIVATE cxx_std_11)
target_compile_options(parkingctl PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(parkingctl PRIVATE
    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
)

if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(parkingd PRIVATE -Werror)
    target_compile_options(parkingctl PRIVATE -Werror)
endif()
if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
    target_compile_definitions(parkingd PRIVATE
        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
    )
    target_compile_definitions(parkingctl PRIVATE
        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
    )
endif()

if(BUILD_TESTING)
    add_executable(parking_domain_test
        tests/parking/domain_test.cpp
    )
    target_link_libraries(parking_domain_test PRIVATE
        parking_domain
        parking_recognition
        parking_storage
    )
    target_compile_features(parking_domain_test PRIVATE cxx_std_11)
    target_compile_options(parking_domain_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
        target_compile_options(parking_domain_test PRIVATE -Werror)
    endif()
    add_test(NAME parking_domain COMMAND parking_domain_test)

    add_executable(parking_control_test
        tests/parking/control_test.cpp
    )
    target_link_libraries(parking_control_test PRIVATE
        parking_control
        parking_domain
    )
    target_compile_features(parking_control_test PRIVATE cxx_std_11)
    target_compile_options(parking_control_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
        target_compile_options(parking_control_test PRIVATE -Werror)
    endif()
    add_test(NAME parking_control COMMAND parking_control_test)

    add_executable(parking_sqlite_test
        tests/parking/sqlite_test.cpp
    )
    target_link_libraries(parking_sqlite_test PRIVATE
        parking_storage
        parking_recognition
        parking_domain
    )
    target_compile_features(parking_sqlite_test PRIVATE cxx_std_11)
    target_compile_options(parking_sqlite_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
        target_compile_options(parking_sqlite_test PRIVATE -Werror)
    endif()
    add_test(NAME parking_sqlite COMMAND parking_sqlite_test)
endif()

add_library(camera_capture STATIC
    src/camera/capture/buffer.cpp
    src/camera/capture/device.cpp
    src/camera/capture/session.cpp
)
target_include_directories(camera_capture PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_compile_features(camera_capture PUBLIC cxx_std_11)
target_compile_options(camera_capture PRIVATE -Wall -Wextra -Wpedantic)
if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
    target_compile_options(camera_capture PRIVATE -Werror)
endif()

add_executable(camera_demo src/camera/apps/camera_demo.cpp)
target_link_libraries(camera_demo PRIVATE camera_capture)

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
        add_library(camera_drm STATIC
            src/camera/display/device.cpp
            src/camera/display/display.cpp
        )
        target_include_directories(camera_drm
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc
            PRIVATE
            ${LIBDRM_XF86_INCLUDE_DIR}
            ${LIBDRM_DRM_INCLUDE_DIR}
        )
        target_link_libraries(camera_drm PUBLIC ${LIBDRM_LIBRARY})
        target_compile_features(camera_drm PUBLIC cxx_std_11)
        target_compile_options(camera_drm PRIVATE -Wall -Wextra -Wpedantic)
        if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
            target_compile_options(camera_drm PRIVATE -Werror)
        endif()

        add_executable(drm_probe src/camera/apps/drm_probe.cpp)
        target_link_libraries(drm_probe PRIVATE camera_drm)
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

        if(CAMERA_DEMO_BUILD_RGA_DRM_TEST)
            find_path(LIBRGA_INCLUDE_DIR NAMES im2d.h PATH_SUFFIXES rga)
            find_library(LIBRGA_LIBRARY NAMES rga)

            if(LIBRGA_INCLUDE_DIR AND LIBRGA_LIBRARY)
                add_library(camera_transform STATIC
                    src/camera/display/transform.cpp
                )
                target_include_directories(camera_transform
                    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc
                    PRIVATE
                    ${LIBRGA_INCLUDE_DIR}
                )
                target_link_libraries(camera_transform PUBLIC ${LIBRGA_LIBRARY})
                target_compile_features(camera_transform PUBLIC cxx_std_11)
                target_compile_options(camera_transform PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
                )
                if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
                    target_compile_options(camera_transform PRIVATE -Werror)
                endif()

                add_executable(rga_drm_test
                    src/camera/apps/rga_drm_test.cpp
                )
                target_link_libraries(rga_drm_test PRIVATE
                    camera_drm
                    camera_transform
                )
                target_compile_features(rga_drm_test PRIVATE cxx_std_11)
                target_compile_options(rga_drm_test PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
                )
                target_compile_definitions(rga_drm_test PRIVATE
                    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
                )
                if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
                    target_compile_options(rga_drm_test PRIVATE -Werror)
                endif()
                if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
                    target_compile_definitions(rga_drm_test PRIVATE
                        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
                    )
                endif()
                install(TARGETS rga_drm_test RUNTIME DESTINATION bin)

                add_executable(camera_display_once
                    src/camera/apps/display_once.cpp
                )
                target_link_libraries(camera_display_once PRIVATE
                    camera_capture
                    camera_drm
                    camera_transform
                )
                target_compile_features(camera_display_once PRIVATE cxx_std_11)
                target_compile_options(camera_display_once PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
                )
                target_compile_definitions(camera_display_once PRIVATE
                    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
                )
                if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
                    target_compile_options(camera_display_once PRIVATE -Werror)
                endif()
                if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
                    target_compile_definitions(camera_display_once PRIVATE
                        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
                    )
                endif()
                install(TARGETS camera_display_once RUNTIME DESTINATION bin)

                add_executable(camera_display_stream
                    src/camera/apps/display_stream.cpp
                )
                target_link_libraries(camera_display_stream PRIVATE
                    camera_capture
                    camera_drm
                    camera_transform
                )
                target_compile_features(camera_display_stream PRIVATE cxx_std_11)
                target_compile_options(camera_display_stream PRIVATE
                    -Wall
                    -Wextra
                    -Wpedantic
                )
                target_compile_definitions(camera_display_stream PRIVATE
                    CAMERA_DEMO_VERSION="${PROJECT_VERSION}"
                )
                if(CAMERA_DEMO_WARNINGS_AS_ERRORS)
                    target_compile_options(camera_display_stream PRIVATE -Werror)
                endif()
                if(CAMERA_DEMO_TARGET_RK3568_AARCH64)
                    target_compile_definitions(camera_display_stream PRIVATE
                        CAMERA_DEMO_TARGET_RK3568_AARCH64=1
                    )
                endif()
                install(TARGETS camera_display_stream RUNTIME DESTINATION bin)
            elseif(CAMERA_DEMO_REQUIRE_RGA_DRM_TEST)
                message(FATAL_ERROR
                    "rga_drm_test requires im2d.h and librga")
            else()
                message(WARNING
                    "librga development files were not found; rga_drm_test is disabled")
            endif()
        endif()
    elseif(CAMERA_DEMO_REQUIRE_DRM_PROBE)
        message(FATAL_ERROR
            "drm_probe requires xf86drm.h, libdrm/drm.h and libdrm")
    else()
        message(WARNING
            "libdrm development files were not found; drm_probe is disabled")
    endif()
endif()

# `cmake --install` 会把程序放入 staging_dir/bin，便于之后统一复制到开发板。
install(TARGETS camera_demo parkingd parkingctl RUNTIME DESTINATION bin)
