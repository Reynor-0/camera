#!/bin/sh

# Usage (run as root on the RK3568 board):
#   /home/reynor/board-admin/desktop/disable_desktop_autostart_rk3568.sh
#   /home/reynor/board-admin/desktop/disable_desktop_autostart_rk3568.sh --help
#
# Disable Weston and systemui at subsequent boots, then stop the currently
# running desktop. The operation is reversible with the matching restore script.

set -eu

active_init_dir="/etc/init.d"
disabled_init_dir="/etc/init.d/desktop-disabled"
weston_service="S49weston"
systemui_service="S50systemui"

print_usage()
{
    echo "Usage: $0 [--help]"
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    print_usage
    exit 0
fi
if [ "$#" -ne 0 ]; then
    print_usage >&2
    exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "error: this script must run as root on the RK3568 board" >&2
    exit 1
fi

# Validate the complete state before moving either service. This avoids turning
# an unexpected missing/duplicate file into a partially disabled boot setup.
for service in "${weston_service}" "${systemui_service}"; do
    active_path="${active_init_dir}/${service}"
    disabled_path="${disabled_init_dir}/${service}"
    if [ -e "${active_path}" ] && [ -e "${disabled_path}" ]; then
        echo "error: service exists in both active and disabled locations: ${service}" >&2
        exit 1
    fi
    if [ ! -e "${active_path}" ] && [ ! -e "${disabled_path}" ]; then
        echo "error: service was not found in either location: ${service}" >&2
        exit 1
    fi
done

mkdir -p "${disabled_init_dir}"
for service in "${weston_service}" "${systemui_service}"; do
    active_path="${active_init_dir}/${service}"
    disabled_path="${disabled_init_dir}/${service}"
    if [ -e "${active_path}" ]; then
        mv "${active_path}" "${disabled_path}"
        echo "Disabled at boot: ${active_path}"
    else
        echo "Already disabled at boot: ${active_path}"
    fi
done

# Stop clients before the compositor. systemui's vendor init script does not
# stop sysvolume or the camera app, so explicitly terminate those desktop clients.
"${disabled_init_dir}/${systemui_service}" stop >/dev/null 2>&1 || true
killall camera >/dev/null 2>&1 || true
killall sysvolume >/dev/null 2>&1 || true
killall systemui >/dev/null 2>&1 || true
"${disabled_init_dir}/${weston_service}" stop >/dev/null 2>&1 || true
killall weston >/dev/null 2>&1 || true

# SIGTERM-based vendor stop commands can return before Weston has finished
# releasing DRM. Allow all desktop processes up to five seconds to exit.
remaining_checks=50
while pidof weston systemui sysvolume camera >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        break
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

# A wedged desktop must not retain /dev/dri/card0 in headless-camera mode.
# Restrict forced termination to the four exact vendor desktop process names.
if pidof weston systemui sysvolume camera >/dev/null 2>&1; then
    killall -9 camera sysvolume systemui weston >/dev/null 2>&1 || true
    sleep 1
fi

sync

if pidof weston systemui sysvolume camera >/dev/null 2>&1; then
    echo "error: desktop processes are still running after the stop request" >&2
    exit 1
fi

echo "Desktop autostart is disabled."
echo "Preserved services: ${disabled_init_dir}/${weston_service} and ${disabled_init_dir}/${systemui_service}"
echo "rkaiq 3A, ADB, networking and all other init services were left unchanged."
