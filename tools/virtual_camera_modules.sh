#!/usr/bin/env bash

# 管理用于本项目本机测试的 vivid/vimc 虚拟摄像头内核模块。
#
# 用法：
#   sudo ./tools/virtual_camera_modules.sh load
#   ./tools/virtual_camera_modules.sh status
#   sudo ./tools/virtual_camera_modules.sh unload
#
# load 会创建两个 vivid capture 实例：
#   1. 使用 V4L2 single-planar API；
#   2. 使用 V4L2 multi-planar API。
# 同时加载 vimc，供后续 media-controller pipeline 测试使用。

set -euo pipefail

action="${1:-status}"

print_status() {
    echo "Loaded virtual media modules:"
    if [[ -d /sys/module/vivid ]]; then
        echo "  vivid: loaded"
    else
        echo "  vivid: not loaded"
    fi

    if [[ -d /sys/module/vimc ]]; then
        echo "  vimc:  loaded"
    else
        echo "  vimc:  not loaded"
    fi

    echo "Video nodes:"
    shopt -s nullglob
    local found=0
    local sys_node
    for sys_node in /sys/class/video4linux/video*; do
        found=1
        local node_name
        local device_name
        node_name="$(basename "${sys_node}")"
        device_name="$(<"${sys_node}/name")"
        echo "  /dev/${node_name}: ${device_name}"
    done
    shopt -u nullglob

    if [[ "${found}" -eq 0 ]]; then
        echo "  none"
    fi
}

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "error: '${action}' changes kernel modules and must run as root." >&2
        echo "retry with: sudo $0 ${action}" >&2
        exit 1
    fi
}

case "${action}" in
    load)
        require_root

        if [[ -d /sys/module/vivid ]]; then
            echo "error: vivid is already loaded." >&2
            echo "module parameters cannot be changed after loading." >&2
            echo "inspect with '$0 status', or unload it explicitly first." >&2
            exit 1
        fi

        # n_devs=2 创建两个互相独立的实例。
        # multiplanar=1,2 分别选择 single-planar 和 multi-planar capture API。
        # node_types=1,1 只创建 Video Capture node，避免 VBI/radio/output node 干扰。
        # num_inputs=1,1 和 input_types=0,0 为每个实例创建一个 webcam 输入，便于
        # VIDIOC_ENUM_FRAMESIZES 返回 320x180、640x360、1280x720 等离散尺寸。
        # no_error_inj=1 禁止测试控件主动模拟设备断开，确保基础测试结果可重复。
        modprobe vivid \
            n_devs=2 \
            multiplanar=1,2 \
            node_types=1,1 \
            num_inputs=1,1 \
            input_types=0,0 \
            no_error_inj=1

        # vimc 模拟 sensor -> debayer -> scaler -> capture 的 media-controller 管线。
        # 它不是当前 probe 测试的必要条件；加载失败时撤销刚创建的 vivid，避免留下
        # 一个只完成了一半的测试环境。
        if ! modprobe vimc; then
            modprobe -r vivid
            echo "error: failed to load vimc; vivid was unloaded again." >&2
            exit 1
        fi

        print_status
        ;;

    status)
        print_status
        ;;

    unload)
        require_root

        # modprobe -r 只卸载指定测试模块；若设备正被进程打开，内核会返回 busy，
        # 脚本不会强制结束其他进程。
        if [[ -d /sys/module/vimc ]]; then
            modprobe -r vimc
        fi
        if [[ -d /sys/module/vivid ]]; then
            modprobe -r vivid
        fi

        print_status
        ;;

    *)
        echo "Usage: $0 {load|status|unload}" >&2
        exit 2
        ;;
esac
