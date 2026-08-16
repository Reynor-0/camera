#!/bin/sh

# Usage (run as root on the RK3568 board):
#   /home/reynor/camera-project/scripts/run_rga_drm_test_rk3568.sh [1-300 seconds] [--keep-desktop-stopped]
#
# 在 RK3568 开发板上安全执行离线 NV12 -> RGA -> DRM 显示测试。

set -eu

program="/home/reynor/camera-project/bin/rga_drm_test"
duration_seconds="${1:-10}"
keep_desktop_stopped="${2:-}"
desktop_restore_required=0

print_usage()
{
    echo "Usage: $0 <1-300 seconds> [--keep-desktop-stopped]" >&2
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
        >/var/log/weston-camera-demo.log 2>&1 </dev/null &
    sleep 3
    nohup /etc/init.d/S50systemui start \
        >/var/log/systemui-camera-demo.log 2>&1 </dev/null &
    sleep 2

    if ! pidof weston >/dev/null 2>&1; then
        echo "warning: Weston did not restart; inspect /var/log/weston-camera-demo.log" >&2
    fi
}

case "${duration_seconds}" in
    ''|*[!0-9]*)
        print_usage
        exit 2
        ;;
esac

if [ "${duration_seconds}" -lt 1 ] || [ "${duration_seconds}" -gt 300 ]; then
    print_usage
    exit 2
fi

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

trap restore_desktop EXIT
trap 'exit 130' HUP INT TERM

echo "Stopping systemui, vendor camera and Weston..."
if pidof weston >/dev/null 2>&1 || pidof systemui >/dev/null 2>&1; then
    desktop_restore_required=1
fi
/etc/init.d/S50systemui stop >/dev/null 2>&1 || true
killall camera >/dev/null 2>&1 || true
killall sysvolume >/dev/null 2>&1 || true
/etc/init.d/S49weston stop >/dev/null 2>&1 || true

remaining_checks=30
while pidof weston >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        echo "error: Weston is still running; refusing DRM takeover" >&2
        exit 1
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

echo "Running offline NV12 RGA-to-DRM test for ${duration_seconds} seconds..."
"${program}" \
    --show "${duration_seconds}" \
    --confirm-desktop-stopped \
    /dev/dri/card0

echo "RGA-to-DRM test completed."
