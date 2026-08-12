#!/usr/bin/env bash

# 使用 vivid 创建的虚拟 capture nodes 验证 camera_demo 第一阶段功能。
#
# 本脚本不加载或卸载内核模块。先运行：
#   sudo ./tools/virtual_camera_modules.sh load
#
# 用法：
#   ./tools/test_virtual_v4l2_probe.sh
#   ./tools/test_virtual_v4l2_probe.sh ./path/to/camera_demo

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
camera_demo="${1:-${project_dir}/build/camera_demo}"

if [[ ! -x "${camera_demo}" ]]; then
    echo "error: camera_demo is missing or not executable: ${camera_demo}" >&2
    echo "build it first with: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

shopt -s nullglob
vivid_nodes=()
for sys_node in /sys/class/video4linux/video*; do
    device_name="$(<"${sys_node}/name")"
    if [[ "${device_name}" == vivid* ]]; then
        vivid_nodes+=("/dev/$(basename "${sys_node}")")
    fi
done
shopt -u nullglob

if [[ "${#vivid_nodes[@]}" -eq 0 ]]; then
    echo "error: no vivid video nodes were found." >&2
    echo "load them with: sudo ./tools/virtual_camera_modules.sh load" >&2
    exit 1
fi

seen_single=0
seen_multi=0
passed=0

for node in "${vivid_nodes[@]}"; do
    echo "===== Testing ${node} ====="

    # 保存 stdout 供断言使用，同时原样显示给开发者。命令失败时先输出完整诊断，
    # 然后继续测试其他 node，使一次运行能够发现多个设备上的问题。
    if output="$("${camera_demo}" "${node}" 2>&1)"; then
        echo "${output}"

        if [[ "${output}" != *"Formats ("* ]]; then
            echo "error: ${node} output does not contain a format list." >&2
            continue
        fi

        if [[ "${output}" == *"Capture API: single-planar"* ]]; then
            seen_single=1
        elif [[ "${output}" == *"Capture API: multi-planar"* ]]; then
            seen_multi=1
        else
            echo "error: ${node} did not report a recognized capture API." >&2
            continue
        fi

        passed=$((passed + 1))
    else
        echo "${output}" >&2
        echo "error: camera_demo failed for ${node}." >&2
    fi
done

if [[ "${seen_single}" -ne 1 || "${seen_multi}" -ne 1 ]]; then
    echo "error: test did not cover both single-planar and multi-planar APIs." >&2
    echo "reload vivid using tools/virtual_camera_modules.sh to get both instances." >&2
    exit 1
fi

echo "Virtual V4L2 probe test passed (${passed} nodes)."
