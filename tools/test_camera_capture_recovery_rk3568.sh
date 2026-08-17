#!/usr/bin/env bash

# Usage (run in WSL after deploying the project to the RK3568 board):
#   ADB=/home/reynor/tools/platform-tools/adb ./tools/test_camera_capture_recovery_rk3568.sh [/dev/videoN] [color-mode]
#
# 本脚本通过 ADB 运行一次“恢复成功”和一次“恢复预算耗尽”诊断。要求板端已停止
# Weston/systemui/vendor camera，要求 root；脚本不修改启动配置，也不会自动恢复桌面。
# 若测试中断，EXIT trap 只会终止本脚本确认不存在旧 worker 后启动的测试 worker。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
adb_command="${ADB:-adb}"
adb_serial="${ADB_SERIAL:-}"
video_device="${1:-/dev/video0}"
color_mode="${2:-bt709-limited}"
board_program="/home/reynor/camera-project/bin/camera_display_stream"
board_log_directory="/home/reynor/camera-project/logs"
active_test_worker=0

print_usage()
{
    echo "Usage: ADB=/path/to/adb ${project_dir}/tools/test_camera_capture_recovery_rk3568.sh [/dev/videoN] [auto|bt601-limited|bt601-full|bt709-limited]"
}

cleanup_test_worker()
{
    if [[ "${active_test_worker}" -eq 1 ]]; then
        "${adb_args[@]}" shell \
            "worker=\$(pidof camera_display_stream 2>/dev/null || true); if [ -n \"\$worker\" ]; then kill -TERM \$worker; fi" \
            >/dev/null 2>&1 || true
    fi
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    print_usage
    exit 0
fi
if [[ "$#" -gt 2 || ! "${video_device}" =~ ^/dev/video[0-9]+$ ]]; then
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

trap cleanup_test_worker EXIT

availability="$(
    "${adb_args[@]}" shell \
        "if [ \"\$(id -u)\" = 0 ] && [ -x ${board_program} ] && [ -c ${video_device} ]; then echo ok; else echo missing; fi" |
        tr -d '\r'
)"
if [[ "${availability}" != "ok" ]]; then
    echo "error: root shell, deployed worker, or video node is unavailable" >&2
    exit 1
fi

conflicting_processes="$(
    "${adb_args[@]}" shell \
        "for name in camera_display_stream weston systemui sysvolume camera; do pid=\$(pidof \$name 2>/dev/null || true); [ -n \"\$pid\" ] && echo \"\$name=\$pid\"; done" |
        tr -d '\r'
)"
if [[ -n "${conflicting_processes}" ]]; then
    echo "error: display/camera processes are running; stop them before recovery testing:" >&2
    printf '%s\n' "${conflicting_processes}" >&2
    exit 1
fi

"${adb_args[@]}" shell "mkdir -p ${board_log_directory}"

audit_resources()
{
    local worker
    local drm_clients
    local video_holders

    worker="$(
        "${adb_args[@]}" shell \
            "pidof camera_display_stream 2>/dev/null || true" |
            tr -d '\r\n'
    )"
    if [[ -n "${worker}" ]]; then
        echo "error: worker remained after recovery test: ${worker}" >&2
        return 1
    fi

    drm_clients="$(
        "${adb_args[@]}" shell \
            "cat /sys/kernel/debug/dri/0/clients 2>/dev/null" |
            tr -d '\r'
    )"
    if grep -q camera_display <<<"${drm_clients}"; then
        echo "error: DRM client remained after recovery test" >&2
        printf '%s\n' "${drm_clients}" >&2
        return 1
    fi

    video_holders="$(
        "${adb_args[@]}" shell \
            "for p in /proc/[0-9]*; do for fd in \"\$p\"/fd/*; do target=\$(readlink \"\$fd\" 2>/dev/null || true); [ \"\$target\" = ${video_device} ] && echo \${p#/proc/}; done; done" |
            tr -d '\r\n'
    )"
    if [[ -n "${video_holders}" ]]; then
        echo "error: ${video_device} remained open by PID ${video_holders}" >&2
        return 1
    fi
}

run_case()
{
    local case_name="$1"
    local duration_seconds="$2"
    local injected_recoveries="$3"
    local expected_status="$4"
    local board_log="${board_log_directory}/capture-recovery-${case_name}.log"
    local status_output
    local remote_status

    echo "Capture recovery case ${case_name}: starting..."
    active_test_worker=1
    status_output="$(
        "${adb_args[@]}" shell \
            "${board_program} --stream ${duration_seconds} --confirm-desktop-stopped --color-mode ${color_mode} --inject-capture-timeout-recoveries ${injected_recoveries} ${video_device} /dev/dri/card0 >${board_log} 2>&1; result=\$?; echo __status=\$result"
    )"
    active_test_worker=0
    remote_status="$(
        sed -n 's/.*__status=\([0-9][0-9]*\).*/\1/p' <<<"${status_output}" |
            tr -d '\r\n'
    )"
    if [[ "${remote_status}" != "${expected_status}" ]]; then
        echo "error: ${case_name} returned ${remote_status:-unknown}, expected ${expected_status}" >&2
        "${adb_args[@]}" shell "cat ${board_log}" >&2 || true
        return 1
    fi
    audit_resources
    echo "Capture recovery case ${case_name}: process/resource result PASS (${board_log})"
}

run_case success 4 1 0
success_log="$(
    "${adb_args[@]}" shell \
        "cat ${board_log_directory}/capture-recovery-success.log" |
        tr -d '\r'
)"
if ! grep -Fq "Recovery capture: SUCCEEDED level=L1 attempt=1" <<<"${success_log}" ||
   ! grep -Fq "Capture stream recoveries: attempted=1 succeeded=1 failed=0 budget_exhausted=0" <<<"${success_log}" ||
   ! grep -Fq "Lifecycle: STOPPED" <<<"${success_log}"; then
    echo "error: successful recovery log is missing required markers" >&2
    printf '%s\n' "${success_log}" >&2
    exit 1
fi

run_case budget-exhausted 10 4 20
budget_log="$(
    "${adb_args[@]}" shell \
        "cat ${board_log_directory}/capture-recovery-budget-exhausted.log" |
        tr -d '\r'
)"
success_count="$(
    grep -Fc "Recovery capture: SUCCEEDED level=L1" <<<"${budget_log}" || true
)"
if [[ "${success_count}" -ne 3 ]] ||
   ! grep -Fq "Recovery capture: BUDGET_EXHAUSTED level=L1" <<<"${budget_log}" ||
   ! grep -Fq "Lifecycle: FAILED domain=capture" <<<"${budget_log}" ||
   ! grep -Fq "Exit code: 20" <<<"${budget_log}"; then
    echo "error: budget exhaustion log is missing required markers" >&2
    printf '%s\n' "${budget_log}" >&2
    exit 1
fi

echo "RK3568 capture recovery test passed: L1 success and bounded failure verified."
