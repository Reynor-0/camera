#!/usr/bin/env bash

# Usage (run in WSL from the project root or any directory):
#   ADB=/home/reynor/tools/platform-tools/adb ./tools/deploy_rk3568.sh
#
# 将 RK3568 项目产物部署到 /home/reynor/camera-project，不混放板级管理脚本。
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
board_project_directory="/home/reynor/camera-project"
board_binary_directory="${board_project_directory}/bin"
board_script_directory="${board_project_directory}/scripts"
board_log_directory="${board_project_directory}/logs"
binaries=(
    camera_demo
    drm_probe
    rga_drm_test
    camera_display_once
    camera_display_stream
)

print_usage()
{
    echo "Usage: ADB=/path/to/adb $0 [--help]"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    print_usage
    exit 0
fi
if [[ "$#" -ne 0 ]]; then
    print_usage >&2
    exit 2
fi

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

"${adb_command}" shell "mkdir -p ${board_binary_directory} ${board_script_directory} ${board_log_directory}"

# 迁移旧版部署遗留的已知项目文件。范围使用固定白名单，不处理用户放在 home 中的
# 其他文件。随后 push 当前版本，因此移动旧二进制不会造成版本倒退。
for binary_name in "${binaries[@]}"; do
    "${adb_command}" shell "if [ -e /home/reynor/${binary_name} ]; then mv /home/reynor/${binary_name} ${board_binary_directory}/${binary_name}; fi"
done

for binary_name in "${binaries[@]}"; do
    local_binary="${stage_dir}/bin/${binary_name}"
    board_binary="${board_binary_directory}/${binary_name}"

    if [[ ! -f "${local_binary}" ]]; then
        echo "error: cross-built executable is missing: ${local_binary}" >&2
        echo "run ./tools/cross_build_rk3568.sh first." >&2
        exit 1
    fi

    "${adb_command}" push "${local_binary}" "${board_binary}"
    "${adb_command}" shell "chmod 0755 ${board_binary}"
    echo "Deployed: ${board_binary}"
done

# 板端独占显示脚本与 bin 分目录保存；脚本内部使用绝对项目路径。
test_scripts=(
    run_drm_color_bars_rk3568.sh
    run_drm_page_flip_rk3568.sh
    run_rga_drm_test_rk3568.sh
    run_camera_display_once_rk3568.sh
    run_camera_display_stream_rk3568.sh
)
for script_name in "${test_scripts[@]}"; do
    local_test_script="${project_dir}/tools/${script_name}"
    "${adb_command}" shell "if [ -e /home/reynor/${script_name} ]; then mv /home/reynor/${script_name} ${board_script_directory}/${script_name}; fi"
    board_test_script="${board_script_directory}/${script_name}"
    "${adb_command}" push "${local_test_script}" "${board_test_script}"
    "${adb_command}" shell "chmod 0755 ${board_test_script}"
    echo "Deployed: ${board_test_script}"
done

legacy_logs=(capture-10min.log drm_color_bars.log record.log)
for log_name in "${legacy_logs[@]}"; do
    "${adb_command}" shell "if [ -e /home/reynor/${log_name} ]; then mv /home/reynor/${log_name} ${board_log_directory}/${log_name}; fi"
done

echo "RK3568 project deployment completed: ${board_project_directory}"
