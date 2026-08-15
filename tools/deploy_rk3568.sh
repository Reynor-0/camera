#!/usr/bin/env bash

# 将 RK3568 交叉编译产物统一部署到开发板 /home/reynor。
#
# 可选环境变量：
#   ADB                 adb 可执行文件或绝对路径，默认从 PATH 查找 adb
#   RK3568_STAGE_DIR    安装暂存目录，默认 <project>/build-rk3568/stage
#
# 本脚本只部署本项目明确列出的可执行文件，不修改板端系统目录或动态库。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
adb_command="${ADB:-adb}"
stage_dir="${RK3568_STAGE_DIR:-${project_dir}/build-rk3568/stage}"
board_directory="/home/reynor"
binaries=(camera_demo drm_probe rga_drm_test)

if ! command -v "${adb_command}" >/dev/null 2>&1; then
    echo "error: adb was not found: ${adb_command}" >&2
    echo "set ADB to the platform-tools adb absolute path." >&2
    exit 1
fi

if [[ "$("${adb_command}" get-state 2>/dev/null)" != "device" ]]; then
    echo "error: no usable ADB device is connected." >&2
    "${adb_command}" devices -l >&2
    exit 1
fi

"${adb_command}" shell "mkdir -p ${board_directory}"

for binary_name in "${binaries[@]}"; do
    local_binary="${stage_dir}/bin/${binary_name}"
    board_binary="${board_directory}/${binary_name}"

    if [[ ! -f "${local_binary}" ]]; then
        echo "error: cross-built executable is missing: ${local_binary}" >&2
        echo "run ./tools/cross_build_rk3568.sh first." >&2
        exit 1
    fi

    "${adb_command}" push "${local_binary}" "${board_binary}"
    "${adb_command}" shell "chmod 0755 ${board_binary}"
    echo "Deployed: ${board_binary}"
done

# 板端独占显示涉及停止和恢复桌面服务。把受控运行脚本部署到同一目录，避免用户
# 从 /tmp 或其他位置调用不同版本的脚本和可执行文件。
test_scripts=(
    run_drm_color_bars_rk3568.sh
    run_drm_page_flip_rk3568.sh
    run_rga_drm_test_rk3568.sh
)
for script_name in "${test_scripts[@]}"; do
    local_test_script="${project_dir}/tools/${script_name}"
    board_test_script="${board_directory}/${script_name}"
    "${adb_command}" push "${local_test_script}" "${board_test_script}"
    "${adb_command}" shell "chmod 0755 ${board_test_script}"
    echo "Deployed: ${board_test_script}"
done

echo "RK3568 deployment completed."
