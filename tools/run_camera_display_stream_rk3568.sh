#!/bin/sh

# Usage (run as root on the RK3568 board):
#   /home/reynor/camera-project/scripts/run_camera_display_stream_rk3568.sh <1-3600 seconds|forever> [/dev/videoN] [auto|bt601-limited|bt601-full|bt709-limited] [--keep-desktop-stopped]
#
# 在 RK3568 上停止桌面，以双 DRM framebuffer 连续显示真实相机画面，再按开机策略
# 决定是否恢复桌面。收到 HUP/INT/TERM 时会把停止请求转发给 worker，等待其完成
# STREAMOFF 和 DRM 清理；本脚本不修改开机启动配置。

set -eu

program="/home/reynor/camera-project/bin/camera_display_stream"
run_limit="${1:-10}"
video_device="${2:-/dev/video0}"
color_mode="${3:-bt709-limited}"
keep_desktop_stopped="${4:-}"
desktop_restore_required=0
worker_pid=""

print_usage()
{
    echo "Usage: $0 <1-3600 seconds|forever> [/dev/videoN] [auto|bt601-limited|bt601-full|bt709-limited] [--keep-desktop-stopped]" >&2
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    print_usage
    exit 0
fi

restore_desktop()
{
    if [ "${desktop_restore_required}" -eq 0 ] || \
       [ "${keep_desktop_stopped}" = "--keep-desktop-stopped" ]; then
        return
    fi
    if [ ! -e /etc/init.d/S49weston ] || [ ! -e /etc/init.d/S50systemui ]; then
        echo "Desktop autostart is disabled; leaving Weston/systemui stopped."
        return
    fi

    echo "Restoring Weston and systemui..."
    nohup /usr/bin/weston -w \
        >/var/log/weston-camera-stream.log 2>&1 </dev/null &
    sleep 3
    nohup /etc/init.d/S50systemui start \
        >/var/log/systemui-camera-stream.log 2>&1 </dev/null &
    sleep 2

    if ! pidof weston >/dev/null 2>&1; then
        echo "warning: Weston did not restart; inspect /var/log/weston-camera-stream.log" >&2
    fi
}

forward_sigint()
{
    if [ -n "${worker_pid}" ]; then
        kill -INT "${worker_pid}" >/dev/null 2>&1 || true
    fi
}

forward_sigterm()
{
    if [ -n "${worker_pid}" ]; then
        kill -TERM "${worker_pid}" >/dev/null 2>&1 || true
    fi
}

if [ "${run_limit}" != "forever" ]; then
    case "${run_limit}" in
        ''|*[!0-9]*)
            print_usage
            exit 2
            ;;
    esac
    if [ "${run_limit}" -lt 1 ] || [ "${run_limit}" -gt 3600 ]; then
        print_usage
        exit 2
    fi
fi

case "${video_device}" in
    /dev/video[0-9]*) ;;
    *)
        print_usage
        exit 2
        ;;
esac

case "${color_mode}" in
    auto|bt601-limited|bt601-full|bt709-limited) ;;
    *)
        print_usage
        exit 2
        ;;
esac

if [ -n "${keep_desktop_stopped}" ] && \
   [ "${keep_desktop_stopped}" != "--keep-desktop-stopped" ]; then
    print_usage
    exit 2
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "error: this script must run as root on the RK3568 board" >&2
    exit 1
fi
if [ ! -x "${program}" ]; then
    echo "error: executable not found: ${program}" >&2
    exit 1
fi
if [ ! -c "${video_device}" ]; then
    echo "error: video capture node not found: ${video_device}" >&2
    exit 1
fi

trap restore_desktop EXIT
trap forward_sigterm HUP TERM
trap forward_sigint INT

echo "Stopping systemui, vendor camera and Weston..."
if pidof weston >/dev/null 2>&1 || pidof systemui >/dev/null 2>&1; then
    desktop_restore_required=1
fi
/etc/init.d/S50systemui stop >/dev/null 2>&1 || true
killall camera >/dev/null 2>&1 || true
killall sysvolume >/dev/null 2>&1 || true
killall systemui >/dev/null 2>&1 || true
/etc/init.d/S49weston stop >/dev/null 2>&1 || true
killall weston >/dev/null 2>&1 || true

remaining_checks=50
while pidof weston systemui sysvolume camera >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        break
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

if pidof weston systemui sysvolume camera >/dev/null 2>&1; then
    echo "error: desktop processes are still running; refusing DRM takeover" >&2
    exit 1
fi

echo "Continuously displaying ${video_device} with ${color_mode}; run limit=${run_limit}..."
if [ "${run_limit}" = "forever" ]; then
    "${program}" \
        --run-forever \
        --confirm-desktop-stopped \
        --color-mode "${color_mode}" \
        "${video_device}" \
        /dev/dri/card0 &
else
    "${program}" \
        --stream "${run_limit}" \
        --confirm-desktop-stopped \
        --color-mode "${color_mode}" \
        "${video_device}" \
        /dev/dri/card0 &
fi
worker_pid=$!

# wait 被 shell signal trap 打断时 child 可能仍在完成清理。只要 PID 仍存在就继续
# wait，确保 EXIT trap 不会在 worker 仍持有 DRM/V4L2 fd 时提前恢复桌面。
worker_status=0
while :; do
    set +e
    wait "${worker_pid}"
    wait_status=$?
    set -e
    if ! kill -0 "${worker_pid}" >/dev/null 2>&1; then
        worker_status=${wait_status}
        break
    fi
done
worker_pid=""

if [ "${worker_status}" -eq 0 ]; then
    echo "Continuous camera display worker stopped cleanly."
else
    echo "error: camera display worker exited with status ${worker_status}" >&2
fi
exit "${worker_status}"
