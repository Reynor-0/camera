#!/usr/bin/env bash

# Usage (run in WSL from the project root or any directory):
#   ADB=/home/reynor/tools/platform-tools/adb ./tools/test_camera_display_lifecycle_rk3568.sh [1-1000 iterations] [/dev/videoN] [color-mode]
#
# 通过 ADB 启动板端 forever worker、发送 SIGTERM，并验证 V4L2/DRM 清理日志和进程
# 无残留。日志写入 /home/reynor/camera-project/logs；本脚本不修改开机启动配置。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
adb_command="${ADB:-adb}"
adb_serial="${ADB_SERIAL:-}"
iterations="${1:-1}"
video_device="${2:-/dev/video0}"
color_mode="${3:-bt709-limited}"
board_script="/home/reynor/camera-project/scripts/run_camera_display_stream_rk3568.sh"
board_log_directory="/home/reynor/camera-project/logs"
active_adb_pid=""
active_worker_pid=""

print_usage()
{
    echo "Usage: ADB=/path/to/adb ${project_dir}/tools/test_camera_display_lifecycle_rk3568.sh [1-1000 iterations] [/dev/videoN] [auto|bt601-limited|bt601-full|bt709-limited]"
}

cleanup_active_session()
{
    if [[ -n "${active_worker_pid}" ]]; then
        "${adb_args[@]}" shell \
            "kill -TERM ${active_worker_pid} >/dev/null 2>&1 || true" \
            >/dev/null 2>&1 || true
    fi
    if [[ -n "${active_adb_pid}" ]] && kill -0 "${active_adb_pid}" 2>/dev/null; then
        kill -TERM "${active_adb_pid}" 2>/dev/null || true
        wait "${active_adb_pid}" 2>/dev/null || true
    fi
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    print_usage
    exit 0
fi

trap cleanup_active_session EXIT
if [[ "$#" -gt 3 ]]; then
    print_usage >&2
    exit 2
fi
if [[ ! "${iterations}" =~ ^[0-9]+$ ]] ||
   [[ "${iterations}" -lt 1 || "${iterations}" -gt 1000 ]]; then
    print_usage >&2
    exit 2
fi
if [[ ! "${video_device}" =~ ^/dev/video[0-9]+$ ]]; then
    print_usage >&2
    exit 2
fi
case "${color_mode}" in
    auto|bt601-limited|bt601-full|bt709-limited) ;;
    *)
        print_usage >&2
        exit 2
        ;;
esac

if ! command -v "${adb_command}" >/dev/null 2>&1; then
    echo "error: adb was not found: ${adb_command}" >&2
    exit 1
fi

adb_args=("${adb_command}")
if [[ -n "${adb_serial}" ]]; then
    adb_args+=(-s "${adb_serial}")
fi
if [[ "$("${adb_args[@]}" get-state 2>/dev/null)" != "device" ]]; then
    echo "error: no usable ADB device is connected" >&2
    "${adb_command}" devices -l >&2
    exit 1
fi

"${adb_args[@]}" shell "mkdir -p ${board_log_directory}"
availability="$(
    "${adb_args[@]}" shell \
        "if [ -x ${board_script} ] && [ -c ${video_device} ]; then echo ok; else echo missing; fi" |
        tr -d '\r'
)"
if [[ "${availability}" != "ok" ]]; then
    echo "error: board script or video device is unavailable; deploy the project first" >&2
    exit 1
fi
existing_worker="$(
    "${adb_args[@]}" shell "pidof camera_display_stream 2>/dev/null || true" |
        tr -d '\r\n'
)"
if [[ -n "${existing_worker}" ]]; then
    echo "error: camera_display_stream is already running; refusing to stop an unrelated worker" >&2
    exit 1
fi

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    board_log="${board_log_directory}/lifecycle-sigterm-${iteration}.log"
    echo "Lifecycle iteration ${iteration}/${iterations}: starting forever worker..."
    # 旧版 adbd 会在创建它的远端 shell 退出时终止后台任务，即使使用 nohup。
    # 因此让一个本地 adb 客户端始终持有前台远端会话，另一个客户端负责查询和发信号。
    "${adb_args[@]}" shell \
        "${board_script} forever ${video_device} ${color_mode} --keep-desktop-stopped >${board_log} 2>&1" &
    active_adb_pid=$!

    worker_pid="$(
        "${adb_args[@]}" shell \
            'checks=100; while [ "$checks" -gt 0 ]; do worker=$(pidof camera_display_stream 2>/dev/null || true); if [ -n "$worker" ]; then echo "$worker"; exit 0; fi; checks=$((checks - 1)); sleep .1; done' |
            tr -d '\r\n'
    )"
    if [[ ! "${worker_pid}" =~ ^[0-9]+$ ]]; then
        echo "error: worker did not enter RUNNING; board log follows" >&2
        "${adb_args[@]}" shell "cat ${board_log}" >&2 || true
        exit 1
    fi
    active_worker_pid="${worker_pid}"

    # 允许数十帧通过后发送正式服务停止信号，覆盖运行态而非仅初始化态退出。
    sleep 2
    "${adb_args[@]}" shell "kill -TERM ${worker_pid}"

    stop_state="$(
        "${adb_args[@]}" shell \
            "checks=100; while [ \"\${checks}\" -gt 0 ]; do if ! kill -0 ${worker_pid} >/dev/null 2>&1; then echo stopped; exit 0; fi; checks=\$((checks - 1)); sleep .1; done; echo running" |
            tr -d '\r\n'
    )"
    if [[ "${stop_state}" != "stopped" ]]; then
        echo "error: worker did not stop within 10 seconds" >&2
        "${adb_args[@]}" shell "cat ${board_log}" >&2 || true
        exit 1
    fi
    active_worker_pid=""

    adb_session_stopped=0
    for ((check = 0; check < 50; ++check)); do
        if ! kill -0 "${active_adb_pid}" 2>/dev/null; then
            adb_session_stopped=1
            break
        fi
        sleep 0.1
    done
    if [[ "${adb_session_stopped}" -ne 1 ]]; then
        echo "error: board wrapper did not return after worker cleanup" >&2
        "${adb_args[@]}" shell "cat ${board_log}" >&2 || true
        exit 1
    fi
    wait "${active_adb_pid}" || true
    active_adb_pid=""

    log_validation="$(
        "${adb_args[@]}" shell \
            "if grep -F 'Stop reason: SIGTERM' ${board_log} >/dev/null && \
                grep -F 'Cleanup V4L2: STREAMOFF complete' ${board_log} >/dev/null && \
                grep -F 'Cleanup buffers: framebuffer release complete' ${board_log} >/dev/null && \
                grep -F 'Lifecycle: STOPPED' ${board_log} >/dev/null; then echo ok; else echo invalid; fi" |
            tr -d '\r\n'
    )"
    if [[ "${log_validation}" != "ok" ]]; then
        echo "error: lifecycle cleanup markers are incomplete" >&2
        "${adb_args[@]}" shell "cat ${board_log}" >&2 || true
        exit 1
    fi

    drm_clients="$(
        "${adb_args[@]}" shell "cat /sys/kernel/debug/dri/0/clients 2>/dev/null" |
            tr -d '\r'
    )"
    if grep -q camera_display <<<"${drm_clients}"; then
        echo "error: DRM client remained after iteration ${iteration}" >&2
        printf '%s\n' "${drm_clients}" >&2
        exit 1
    fi

    echo "Lifecycle iteration ${iteration}: PASS (${board_log})"
done

echo "RK3568 lifecycle test passed: iterations=${iterations}, signal=SIGTERM"
