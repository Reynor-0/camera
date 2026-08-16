#!/bin/sh

# Usage (run as root on the RK3568 board):
#   /home/reynor/board-admin/desktop/restore_desktop_autostart_rk3568.sh
#   /home/reynor/board-admin/desktop/restore_desktop_autostart_rk3568.sh --no-start
#   /home/reynor/board-admin/desktop/restore_desktop_autostart_rk3568.sh --help
#
# Restore Weston and systemui to the BusyBox rcS boot sequence. By default the
# desktop is also started immediately; --no-start only restores the next boot.

set -eu

active_init_dir="/etc/init.d"
disabled_init_dir="/etc/init.d/desktop-disabled"
weston_service="S49weston"
systemui_service="S50systemui"
start_now=1

print_usage()
{
    echo "Usage: $0 [--no-start|--help]"
}

case "${1:-}" in
    '') ;;
    --no-start) start_now=0 ;;
    --help|-h)
        print_usage
        exit 0
        ;;
    *)
        print_usage >&2
        exit 2
        ;;
esac
if [ "$#" -gt 1 ]; then
    print_usage >&2
    exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "error: this script must run as root on the RK3568 board" >&2
    exit 1
fi

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

for service in "${weston_service}" "${systemui_service}"; do
    active_path="${active_init_dir}/${service}"
    disabled_path="${disabled_init_dir}/${service}"
    if [ -e "${disabled_path}" ]; then
        mv "${disabled_path}" "${active_path}"
        chmod 0755 "${active_path}"
        echo "Restored at boot: ${active_path}"
    else
        echo "Already enabled at boot: ${active_path}"
    fi
done
sync

if [ "${start_now}" -eq 0 ]; then
    echo "Desktop autostart is restored; the desktop was not started now."
    exit 0
fi

if ! pidof weston >/dev/null 2>&1; then
    "${active_init_dir}/${weston_service}" start
fi

remaining_checks=50
while ! pidof weston >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        echo "error: Weston did not start; autostart files are restored for the next boot" >&2
        exit 1
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

if ! pidof systemui >/dev/null 2>&1; then
    "${active_init_dir}/${systemui_service}" start
fi

remaining_checks=100
while ! pidof systemui >/dev/null 2>&1; do
    if [ "${remaining_checks}" -eq 0 ]; then
        echo "error: systemui did not start; autostart files are restored for the next boot" >&2
        exit 1
    fi
    remaining_checks=$((remaining_checks - 1))
    sleep .1
done

echo "Desktop autostart is restored and Weston/systemui are running."
