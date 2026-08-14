#!/bin/sh

# 在 RK3568 开发板上安全执行独占 DRM 色条测试。
#
# 默认流程：
#   1. 停止 systemui、厂商 camera 和 Weston。
#   2. 确认 Weston 不再持有 DRM master。
#   3. 运行 /home/reynor/drm_probe 独占色条测试。
#   4. 无论测试成功、失败或收到退出信号，都恢复 Weston 和 systemui。
#
# 本脚本必须部署到开发板 /home/reynor 后以 root 运行。

set -eu

program="/home/reynor/drm_probe"
duration_seconds="${1:-5}"
keep_desktop_stopped="${2:-}"
desktop_restore_required=0

print_usage()
{
    echo "Usage: $0 <1-300 seconds> [--keep-desktop-stopped]" >&2
}

restore_desktop()
{
    if [ "${desktop_restore_required}" -eq 0 ] || \
       [ "${keep_desktop_stopped}" = "--keep-desktop-stopped" ]; then
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

# EXIT trap 覆盖正常退出和错误路径；信号 trap 只把控制流导向 EXIT。
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

# killall 返回后仍给进程最多 3 秒清理 fd，防止与 drm_probe 竞争 master。
remaining_checks=30
while pidof weston >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        echo "error: Weston is still running; refusing DRM takeover" >&2
        exit 1
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

echo "Running exclusive DRM color bars for ${duration_seconds} seconds..."
"${program}" \
    --show-color-bars "${duration_seconds}" \
    --confirm-desktop-stopped \
    /dev/dri/card0

echo "DRM color-bar test completed."
