#!/usr/bin/env bash

# 为 64 位 RK3568 Linux 用户态配置、编译并安装 camera_demo。
#
# 可选环境变量：
#   RK3568_CROSS_COMPILE  交叉工具前缀；未设置时优先使用 ATK Buildroot 工具链
#   RK3568_SYSROOT        目标 rootfs/sysroot；未设置时从选中的编译器自动查询
#   RK3568_BUILD_DIR      构建目录，默认 <project>/build-rk3568
#   RK3568_STAGE_DIR      安装暂存目录，默认 <build-dir>/stage
#   RK3568_CPU_FLAGS      可选，例如 -mcpu=cortex-a55

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"

atk_cross_prefix="/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-"
if [[ -n "${RK3568_CROSS_COMPILE:-}" ]]; then
    cross_prefix="${RK3568_CROSS_COMPILE}"
elif command -v aarch64-buildroot-linux-gnu-g++ >/dev/null 2>&1; then
    cross_prefix="aarch64-buildroot-linux-gnu-"
elif [[ -x "${atk_cross_prefix}g++" ]]; then
    cross_prefix="${atk_cross_prefix}"
else
    # 保留通用 Debian/Ubuntu AArch64 工具链作为最后的兼容回退。
    cross_prefix="aarch64-linux-gnu-"
fi
build_dir="${RK3568_BUILD_DIR:-${project_dir}/build-rk3568}"
stage_dir="${RK3568_STAGE_DIR:-${build_dir}/stage}"
sysroot="${RK3568_SYSROOT:-}"
cpu_flags="${RK3568_CPU_FLAGS:-}"
cxx_compiler="${cross_prefix}g++"
readelf_tool="${cross_prefix}readelf"

if ! command -v "${cxx_compiler}" >/dev/null 2>&1; then
    echo "error: RK3568 cross compiler was not found: ${cxx_compiler}" >&2
    echo "set RK3568_CROSS_COMPILE to the full SDK tool prefix." >&2
    echo "example: export RK3568_CROSS_COMPILE=/opt/rk-sdk/bin/aarch64-linux-gnu-" >&2
    exit 1
fi

if ! command -v "${readelf_tool}" >/dev/null 2>&1; then
    echo "error: RK3568 ELF inspection tool was not found: ${readelf_tool}" >&2
    echo "the compiler prefix must identify a complete AArch64 toolchain." >&2
    exit 1
fi

# Buildroot 的 toolchain wrapper 会根据自己的安装位置返回已经重定位后的 sysroot。
# 显式环境变量仍具有最高优先级，便于临时使用目标板 rootfs 的另一份快照。
if [[ -z "${sysroot}" ]]; then
    detected_sysroot="$("${cxx_compiler}" -print-sysroot 2>/dev/null || true)"
    if [[ -n "${detected_sysroot}" && -d "${detected_sysroot}" ]]; then
        sysroot="${detected_sysroot}"
    fi
fi

if [[ -n "${sysroot}" && ! -d "${sysroot}" ]]; then
    echo "error: RK3568_SYSROOT is not a directory: ${sysroot}" >&2
    exit 1
fi

cmake_args=(
    -S "${project_dir}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="${project_dir}/cmake/toolchains/rk3568-aarch64.cmake"
    -DRK3568_CROSS_COMPILE="${cross_prefix}"
    -DRK3568_CPU_FLAGS="${cpu_flags}"
    -DCMAKE_INSTALL_PREFIX=/usr/local
    # 板端正式产物不允许带着编译告警进入部署阶段。
    -DCAMERA_DEMO_WARNINGS_AS_ERRORS=ON
    -DCAMERA_DEMO_REQUIRE_DRM_PROBE=ON
    -DCAMERA_DEMO_REQUIRE_RGA_DRM_TEST=ON
)

if [[ -n "${sysroot}" ]]; then
    cmake_args+=("-DRK3568_SYSROOT=${sysroot}")
else
    echo "warning: RK3568_SYSROOT is empty; the compiler's built-in sysroot will be used." >&2
    echo "warning: provide the board SDK/rootfs sysroot before adding target libraries such as libdrm." >&2
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel
cmake --install "${build_dir}" --prefix "${stage_dir}"

binary="${stage_dir}/bin/camera_demo"
if [[ ! -f "${binary}" ]]; then
    echo "error: expected output was not installed: ${binary}" >&2
    exit 1
fi

drm_probe_binary="${stage_dir}/bin/drm_probe"
if [[ ! -f "${drm_probe_binary}" ]]; then
    echo "error: expected DRM probe was not installed: ${drm_probe_binary}" >&2
    exit 1
fi

rga_drm_test_binary="${stage_dir}/bin/rga_drm_test"
if [[ ! -f "${rga_drm_test_binary}" ]]; then
    echo "error: expected RGA/DRM test was not installed: ${rga_drm_test_binary}" >&2
    exit 1
fi

# readelf 来自交叉工具链，不依赖宿主机 file 命令对架构名称的输出格式。
machine="$("${readelf_tool}" -h "${binary}" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
if [[ "${machine}" != *AArch64* ]]; then
    echo "error: output is not an AArch64 ELF: ${machine}" >&2
    exit 1
fi

drm_probe_machine="$("${readelf_tool}" -h "${drm_probe_binary}" |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
if [[ "${drm_probe_machine}" != *AArch64* ]]; then
    echo "error: drm_probe is not an AArch64 ELF: ${drm_probe_machine}" >&2
    exit 1
fi

rga_drm_test_machine="$("${readelf_tool}" -h "${rga_drm_test_binary}" |
    sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
if [[ "${rga_drm_test_machine}" != *AArch64* ]]; then
    echo "error: rga_drm_test is not an AArch64 ELF: ${rga_drm_test_machine}" >&2
    exit 1
fi

echo "RK3568 build completed."
echo "  ELF machine: ${machine}"
echo "  Binary:      ${binary}"
echo "  DRM probe:   ${drm_probe_binary}"
echo "  RGA/DRM:     ${rga_drm_test_binary}"
echo "  Deploy with: ./tools/deploy_rk3568.sh"
