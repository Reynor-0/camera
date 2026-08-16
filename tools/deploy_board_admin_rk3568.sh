#!/usr/bin/env bash

# Usage (run in WSL from the project root or any directory):
#   ADB=/home/reynor/tools/platform-tools/adb ./tools/deploy_board_admin_rk3568.sh
#
# Deploy board-level administration scripts separately from camera project files.
# This script only writes /home/reynor/board-admin; it does not change /etc/init.d.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
adb_command="${ADB:-adb}"
board_directory="/home/reynor/board-admin/desktop"
admin_scripts=(
    disable_desktop_autostart_rk3568.sh
    restore_desktop_autostart_rk3568.sh
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
    exit 1
fi
if [[ "$("${adb_command}" get-state 2>/dev/null)" != "device" ]]; then
    echo "error: no usable ADB device is connected" >&2
    "${adb_command}" devices -l >&2
    exit 1
fi

"${adb_command}" shell "mkdir -p ${board_directory}"
for script_name in "${admin_scripts[@]}"; do
    local_script="${project_dir}/tools/board_admin/${script_name}"
    board_script="${board_directory}/${script_name}"
    "${adb_command}" push "${local_script}" "${board_script}"
    "${adb_command}" shell "chmod 0755 ${board_script}"
    echo "Deployed board administration script: ${board_script}"
done

echo "Board administration deployment completed. No boot service was changed."
