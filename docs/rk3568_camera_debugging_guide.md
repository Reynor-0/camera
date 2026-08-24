# RK3568 Camera 常驻进程调试指南

## 1. 文档目标

本文以项目中的常驻程序 `camera_display_stream` 为对象，介绍如何在 WSL 开发机和
RK3568 Buildroot 开发板之间完成以下调试工作：

- 判断问题位于用户态、系统调用、内核驱动还是硬件；
- 使用程序日志、退出码和 `dmesg` 完成第一轮定位；
- 使用 `strace` 判断 V4L2/RGA/DRM 系统调用是否失败或阻塞；
- 使用 GDB/GDB Server 调试异常、段错误和进程卡死；
- 启用并分析 core dump；
- 使用 Valgrind、ASan 和 UBSan 检查内存问题；
- 使用 ftrace 分析 V4L2、videobuf2、DRM 和 DMA fence 时序；
- 在板端具备 `perf` 后分析 CPU 热点和调度问题；
- 区分用户态崩溃、OOM、Kernel Oops 和 Kernel Panic；
- 为 Kernel Panic 保留串口、pstore 和符号文件证据。

本文中的命令以当前板端目录为准：

```text
/home/reynor/camera-project/bin/camera_display_stream
/home/reynor/camera-project/scripts/run_camera_display_stream_rk3568.sh
```

调试程序仍应放在 `/home/reynor` 下的项目子目录，长期日志和 core 文件放在空间更大的
`/userdata`，不要把无界诊断数据写满根分区。

## 2. 先建立分层调试模型

摄像头到屏幕不是一个单独函数，而是跨越用户态、内核和硬件的流水线：

```text
camera_display_stream
  |
  | C++ 状态机、资源所有权、异常处理
  v
libc / libstdc++ / libdrm / librga
  |
  | open / ioctl / poll / mmap / close
  v
Linux 内核
  +-- V4L2 / videobuf2 / rkisp
  +-- DMA-BUF / IOMMU
  +-- RGA driver
  +-- DRM/KMS / VOP / DSI
  |
  v
Sensor / CSI D-PHY / ISP / DDR / RGA / VOP / DSI LCD
```

不同工具只能看到其中一部分：

| 工具 | 观察层 | 最适合回答的问题 |
| --- | --- | --- |
| 应用日志和退出码 | 用户态业务 | 程序主动判定了哪个故障域 |
| `/proc/<pid>` | 进程资源 | 内存、线程、fd、等待位置是否异常 |
| `strace` | 系统调用边界 | 哪个 syscall/ioctl 失败、errno 是什么、等待多久 |
| GDB/core | 用户态指令和内存 | 在哪行崩溃、调用栈和变量是什么 |
| ASan/Valgrind | 用户态内存 | 越界、use-after-free、未初始化值、泄漏 |
| `dmesg` | 内核与驱动 | Sensor/ISP/RGA/DRM/IOMMU/OOM 是否报错 |
| ftrace | 内核动态时序 | QBUF/DQBUF、buffer done、vblank、fence 的先后关系 |
| `perf` | CPU/调度采样 | CPU 时间消耗在哪里、cache miss 和切换是否异常 |
| UART+pstore+vmlinux | 整个内核 | Oops、Panic、锁死和重启前发生了什么 |

不要只使用一个工具就下结论。例如 `strace` 看到 `poll()` 等待很久，只能证明用户态正在
等待；还要结合 V4L2 tracepoint 和 `dmesg` 判断是 Sensor 没出帧、ISP 没完成 buffer，
还是程序等待错了 fd。

## 3. 当前实板调试能力

以下内容于 2026-08-23 通过 ADB 只读检查获得。

### 3.1 用户态工具

| 能力 | 当前状态 |
| --- | --- |
| `gdb` | GNU GDB 10.2，已安装 |
| `gdbserver` | GNU gdbserver 10.2，已安装 |
| `strace` | 4.20，已安装 |
| `valgrind` | 3.18.1，已安装 |
| `dmesg` | 已安装 |
| `top/free/vmstat/slabtop` | 已安装 |
| `perf` 用户态命令 | 未安装 |
| `addr2line/readelf/objdump/nm` 板端命令 | 未安装 |
| `coredumpctl` | 未安装；系统不是 systemd |

WSL 中的 ATK 交叉工具链已经包含 AArch64 GDB 和 binutils：

```text
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gdb
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-addr2line
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-readelf
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-objdump
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-nm
```

因此符号解析应优先在 WSL 完成，板端只负责运行程序、产生 core 或提供 gdbserver。

### 3.2 内核调试能力

当前内核已确认启用：

```text
CONFIG_PERF_EVENTS=y
CONFIG_UPROBES=y
CONFIG_COREDUMP=y
CONFIG_KALLSYMS=y
CONFIG_DEBUG_INFO=y
CONFIG_FRAME_POINTER=y
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_FUNCTION_GRAPH_TRACER=y
CONFIG_MAGIC_SYSRQ=y
CONFIG_SLUB_DEBUG=y
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
```

当前没有确认启用 KASAN、UBSAN、KMEMLEAK、soft/hard lockup detector。用户态 ASan
和内核 KASAN 是不同能力：用户态 ASan 不能检测内核驱动里的越界。

当前还确认了以下 tracepoint group：

```text
v4l2
vb2
drm
dma_fence
sched
irq
workqueue
```

### 3.3 当前崩溃保存策略

```text
ulimit -c                   0
/proc/sys/kernel/core_pattern   core
/proc/sys/kernel/panic          0
/proc/sys/kernel/panic_on_oops  0
/proc/sys/kernel/sysrq          0
```

含义如下：

- 内核支持 core dump，但启动进程的 shell 当前禁止生成 core；
- panic 后默认不定时自动重启；
- Oops 默认不强制升级成 Panic；
- 内核编译了 Magic SysRq，但运行时禁用；
- `/sys/fs/pstore` 已挂载，不过当前为空，仍需验证 ramoops 是否能跨重启保存 Panic。

## 4. 当前构建为什么不适合源码级 GDB

现有 `tools/cross_build_rk3568.sh` 固定使用：

```text
CMAKE_BUILD_TYPE=Release
CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG
```

当前 ELF 有符号表 `.symtab`，但没有 `.debug_info`、`.debug_line` 等 DWARF section。
因此可能看到 `CaptureSession::rebuild()` 等函数名，但不能可靠查看源码行、局部变量和优化
前的执行顺序。

正式产物与调试产物必须分开：

```text
build-rk3568/          正式 Release
build-rk3568-debug/    带符号调试版本
build-rk3568-asan/     Sanitizer 专用版本
```

### 4.1 推荐的 RelWithDebInfo 构建

在 WSL 项目根目录执行：

```bash
CROSS_PREFIX=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-
TARGET_SYSROOT="$("${CROSS_PREFIX}g++" -print-sysroot)"

cmake -S . -B build-rk3568-debug \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3568-aarch64.cmake \
  -DRK3568_CROSS_COMPILE="${CROSS_PREFIX}" \
  -DRK3568_SYSROOT="${TARGET_SYSROOT}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g3 -DNDEBUG -fno-omit-frame-pointer" \
  -DCAMERA_DEMO_WARNINGS_AS_ERRORS=ON \
  -DCAMERA_DEMO_REQUIRE_DRM_PROBE=ON \
  -DCAMERA_DEMO_REQUIRE_RGA_DRM_TEST=ON

cmake --build build-rk3568-debug --parallel
cmake --install build-rk3568-debug \
  --prefix build-rk3568-debug/stage
```

`RelWithDebInfo` 保留一定优化，更容易复现接近 Release 的性能和时序问题。只有当变量被
优化掉、单步行为难以理解时，再建立 `-O0 -g3 -fno-omit-frame-pointer` 版本。

检查 DWARF 是否存在：

```bash
${CROSS_PREFIX}readelf -S \
  build-rk3568-debug/stage/bin/camera_display_stream |
grep -E '\.debug_|\.symtab'
```

调试产物建议部署到单独目录：

```bash
adb shell mkdir -p /home/reynor/camera-project/debug
adb push build-rk3568-debug/stage/bin/camera_display_stream \
  /home/reynor/camera-project/debug/camera_display_stream
```

不要 strip WSL 中保留的主符号文件。即使以后板端部署 stripped binary，也必须按版本
保存对应的 unstripped ELF、Build ID、源码 commit 和 sysroot。

## 5. 通用的第一轮诊断

### 5.1 确认进程与命令行

```sh
PID=$(pidof camera_display_stream)
echo "PID=${PID}"

readlink -f "/proc/${PID}/exe"
tr '\0' ' ' < "/proc/${PID}/cmdline"
echo
```

`pidof` 没有输出只说明当前没有这个名字的进程，还要查看包装脚本是否退出、是否换了
进程名以及程序日志，不能直接判断为内核故障。

### 5.2 查看退出码

前台运行短测试：

```sh
/home/reynor/camera-project/bin/camera_display_stream \
  --stream 10 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0

rc=$?
echo "exit code=${rc}"
```

当前项目的稳定退出码：

| 退出码 | 故障域 | 含义 |
| ---: | --- | --- |
| 0 | success | 定时结束、SIGINT/SIGTERM 正常停止 |
| 1 | runtime | 未分类运行错误 |
| 2 | usage | 参数错误 |
| 10 | configuration | 格式、颜色或固定约束不兼容 |
| 20 | capture | V4L2 open/ioctl/timeout/buffer 或恢复预算失败 |
| 30 | transform | RGA 转换失败 |
| 40 | display | DRM/modeset/page flip/清理失败 |
| 50 | internal | 状态不变量或未知异常 |
| 134 | signal | 常见为 SIGABRT |
| 135 | signal | 常见为 SIGBUS |
| 137 | signal | SIGKILL；可能是人工 kill 或 OOM killer |
| 139 | signal | SIGSEGV，典型段错误 |

退出 20/30/40 表示程序捕获异常并按故障域退出，不等于 Segmentation Fault。

### 5.3 获取进程资源快照

```sh
PID=$(pidof camera_display_stream)

cat "/proc/${PID}/status"
cat "/proc/${PID}/wchan"
ls -l "/proc/${PID}/fd"
cat /sys/kernel/debug/dri/0/clients
```

长期监控时重点关注：

- `VmRSS`：实际驻留物理内存；
- `VmData`：data/heap 规模；
- `VmStk`：主线程栈映射；
- `Threads`：线程数是否无界增长；
- `/proc/<pid>/fd` 数量：设备、DMA-BUF、DRM fd 是否泄漏；
- `wchan`：进程当前在内核的等待点；
- DRM clients：异常退出后是否仍有显示资源使用者。

## 6. 应用日志与 dmesg

### 6.1 两类日志不要混淆

应用日志记录：

- Lifecycle 状态；
- Capture/RGA/DRM 故障域；
- errno 和恢复预算；
- frame、page flip 和 timeout 统计。

`dmesg` 记录：

- Sensor、CSI、D-PHY、ISP 驱动；
- videobuf2、DMA、IOMMU；
- RGA、DRM、VOP、DSI；
- OOM killer；
- Kernel Oops/Panic；
- 部分用户态非法地址异常摘要。

程序通过 `std::cout` 或 `std::cerr` 输出的信息不会自动进入 `dmesg`。

### 6.2 实时查看内核日志

在独立 ADB shell 中：

```sh
dmesg -w
```

过滤摄像头显示链路：

```sh
dmesg -w |
grep -Ei 'imx415|rkisp|csi|dphy|v4l2|vb2|rga|drm|vop|dsi|iommu|dma|cma|oom|segfault|panic|oops|watchdog'
```

若板端 `dmesg` 实现不支持 `-w`，可先记录完整快照，再复现问题后记录第二份进行比较。

### 6.3 保留故障前后证据

```sh
mkdir -p /userdata/camera-debug

dmesg > /userdata/camera-debug/dmesg-before.log

# 在这里运行测试并复现故障。

dmesg > /userdata/camera-debug/dmesg-after.log
```

不要随便使用 `dmesg -c`，因为它会清空当前内核 ring buffer，可能删除唯一的故障证据。

### 6.4 常见关键字

| 日志 | 初步判断 |
| --- | --- |
| `s_stream: 1/0` | Sensor 开始/停止输出 |
| `lose frame` | CSI/ISP 数据面出现丢帧迹象 |
| `IOMMU page fault` | 设备访问了无效 DMA/I/O 地址 |
| `failed to ... vblank/page flip` | DRM/VOP 显示时序或资源问题 |
| `Out of memory`/`Killed process` | OOM killer 介入 |
| `Unable to handle kernel paging request` | 内核态非法访问，可能 Oops/Panic |

单条驱动日志不一定代表根因。必须用同一时间窗口内的应用日志、ftrace 和资源快照交叉
验证。

## 7. strace：观察系统调用边界

### 7.1 适用问题

`strace` 最适合回答：

- 程序是否成功打开 `/dev/video0`、`/dev/rga`、`/dev/dri/card0`；
- 哪个 ioctl 返回了错误；
- errno 是 `EBUSY`、`EINVAL`、`EIO` 还是 `ETIMEDOUT`；
- `poll()` 等待哪个 fd、等待了多久；
- mmap/munmap/close 是否按预期发生；
- 异常退出前最后一次系统调用是什么。

### 7.2 附加到常驻进程

```sh
PID=$(pidof camera_display_stream)
mkdir -p /userdata/camera-debug

strace \
  -ff \
  -ttT \
  -yy \
  -s 128 \
  -e trace=openat,close,ioctl,poll,ppoll,mmap,munmap,read,write \
  -o /userdata/camera-debug/strace-camera \
  -p "${PID}"
```

参数说明：

- `-ff`：每个线程单独输出；
- `-ttT`：记录时间以及 syscall 耗时；
- `-yy`：尽量显示 fd 对应对象；
- `-s 128`：提高字符串截取长度；
- `-e trace=...`：只跟踪相关 syscall，降低开销；
- `-p`：附加到现有进程。

按 Ctrl-C 通常只让 `strace` 脱离，不会终止目标进程。

### 7.3 从启动开始跟踪

```sh
strace \
  -ff \
  -ttT \
  -yy \
  -s 128 \
  -e trace=openat,close,ioctl,poll,ppoll,mmap,munmap,read,write \
  -o /userdata/camera-debug/strace-camera \
  /home/reynor/camera-project/bin/camera_display_stream \
  --stream 5 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

### 7.4 典型解释

```text
poll([{fd=/dev/video0, ...}], 1, 2000) = 0
```

表示用户态等了 2 秒仍没有可 DQBUF 的帧。下一步查看 V4L2/vb2 tracepoint 和 ISP 日志。

```text
ioctl(/dev/video0, VIDIOC_DQBUF, ...) = -1 EIO
```

表示采集 ioctl 在驱动边界返回 I/O error。应用随后应进入 capture 故障域。

```text
ioctl(/dev/dri/card0, ..., ...) = -1 EBUSY
```

可能表示 DRM master、CRTC、connector 或 framebuffer 仍被其他进程占用，需要结合
`/sys/kernel/debug/dri/0/clients` 判断。

### 7.5 局限和副作用

- `strace` 不显示 C++ 函数内部逻辑；
- 不显示 RGA/VOP 硬件内部耗时；
- 不检测 heap/stack 越界；
- 高频 ioctl 跟踪会改变实时性；
- 大量输出可能快速占用存储。

因此应限定 syscall、缩短采集时间并把输出放到 `/userdata`。

## 8. GDB 与 gdbserver

### 8.1 两种方式

板端直接运行 GDB 适合临时实验：

```sh
gdb --args /home/reynor/camera-project/debug/camera_display_stream \
  --stream 5 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

更推荐由板端 `gdbserver` 控制进程、WSL 使用带 sysroot 的交叉 GDB。这样源码、符号和
调试界面都留在开发机。

### 8.2 附加到常驻进程

板端终端：

```sh
PID=$(pidof camera_display_stream)
gdbserver --attach :2345 "${PID}"
```

WSL 终端：

```bash
adb forward tcp:2345 tcp:2345

/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gdb \
  build-rk3568-debug/stage/bin/camera_display_stream
```

进入 GDB 后：

```gdb
set pagination off
set sysroot /opt/atk-dlrk356x-toolchain/aarch64-buildroot-linux-gnu/sysroot
target remote 127.0.0.1:2345
```

附加时目标进程会暂停，所以 LCD 可能停在当前 framebuffer，V4L2 队列也会逐渐没有空闲
buffer。未来有 supervisor/watchdog 时，还必须先进入维护模式，避免调试暂停被误判为
服务失效。

### 8.3 常用命令

```gdb
info threads
thread apply all bt
thread apply all bt full

thread 2
frame 3
info args
info locals

p variable
p/x value
x/32bx address

info registers
disassemble /m

continue
detach
quit
```

`detach` 会让目标进程继续运行。若直接杀死 gdbserver 或错误地发送 kill，可能让
Camera/DRM 资源来不及按项目正常路径清理。

### 8.4 捕获 C++ 异常

当前程序用异常携带 V4L2/RGA/DRM 错误，再由顶层映射成退出码。若只在顶层看到退出
20/30/40，可以在异常刚抛出时停止：

```gdb
catch throw
continue
bt full
```

项目相关断点示例：

```gdb
break CaptureSession::rebuild
break CaptureSession::queue
break rotateNv12ToBgrx8888
break main
```

### 8.5 调试进程卡死

画面停止但 PID 还存在时，先执行：

```gdb
thread apply all bt full
```

常见停点：

| 停点 | 可能原因 |
| --- | --- |
| `poll/ppoll` | 等待 V4L2 frame 或 DRM event |
| V4L2 ioctl | 驱动或 buffer 状态异常 |
| RGA ioctl | job 没返回或 fence/硬件问题 |
| DRM event wait | page flip/vblank 未完成 |
| `futex` | mutex/condition variable/线程死锁 |
| 大量重复函数栈帧 | 递归导致 stack overflow |

GDB 只能告诉你用户线程停在哪里。若停在 ioctl/poll，还要继续用 ftrace 和 `dmesg`
检查内核侧。

## 9. Core dump

### 9.1 为单次调试启用 core

在板端调试 shell 中：

```sh
mkdir -p /userdata/camera-debug/cores

echo '/userdata/camera-debug/cores/core.%e.%p.%t' \
  > /proc/sys/kernel/core_pattern

ulimit -c unlimited

exec /home/reynor/camera-project/debug/camera_display_stream \
  --run-forever \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

注意：

- `core_pattern` 是系统全局设置；
- `ulimit` 是进程继承属性；
- 在另一个已经退出的 shell 执行 `ulimit`，不会改变现有服务；
- 通过包装脚本启动时，`ulimit -c unlimited` 必须写在 `exec`/启动 worker 之前；
- core 可能很大，应写入 `/userdata`，并设置清理策略。

测试完成后可恢复当前模式：

```sh
echo core > /proc/sys/kernel/core_pattern
```

shell 退出后，它设置的 `ulimit` 不会影响以后新启动的独立 shell。

### 9.2 拉回 WSL

```bash
adb shell ls -lh /userdata/camera-debug/cores
adb pull /userdata/camera-debug/cores ./camera-cores
```

Linux 可能截断 `%e` 展开的进程名，因此应以板端 `ls` 返回的真实文件名为准，不要根据
完整可执行文件名猜测 core 路径。

### 9.3 离线分析

```bash
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gdb \
  build-rk3568-debug/stage/bin/camera_display_stream \
  camera-cores/<实际的-core-文件名>
```

GDB 中：

```gdb
set sysroot /opt/atk-dlrk356x-toolchain/aarch64-buildroot-linux-gnu/sysroot
info threads
thread apply all bt full
info registers
```

必须使用产生 core 时完全匹配的：

- unstripped ELF；
- sysroot 动态库；
- 源码 commit；
- 编译参数；
- 构建 ID。

同名但重新编译过的 ELF 不能保证正确解析旧 core。

## 10. perf：分析 CPU 与调度性能

### 10.1 当前还不能直接运行

板端内核已经启用 `CONFIG_PERF_EVENTS=y`，但 rootfs 没有 `perf` 用户态程序。WSL 中通过
APT 安装的普通 `perf` 是 x86_64 程序，不能直接复制到 AArch64 开发板。

推荐方式：

1. 找到与板端完全匹配的 Rockchip 4.19.232 BSP 内核源码；
2. 在 Buildroot 中启用 Linux tools/perf，或者从该内核源码交叉编译 `tools/perf`；
3. 将 AArch64 `perf` 及所需运行库部署到 `/home/reynor/camera-project/tools/`；
4. 先用 `perf version`、简单 `perf stat` 验证内核 ABI 和 PMU 事件；
5. 不要用不匹配的发行版 perf 代替 vendor BSP 工具后直接得出结论。

### 10.2 安装后的基本用法

统计 10 秒：

```sh
PID=$(pidof camera_display_stream)

perf stat \
  -p "${PID}" \
  -e cycles,instructions,cache-misses,context-switches,page-faults \
  -- sleep 10
```

热点采样：

```sh
perf record \
  -F 99 \
  -g \
  --call-graph fp \
  -p "${PID}" \
  -- sleep 30

perf report
```

实时热点：

```sh
perf top -p "${PID}"
```

程序应使用 `-g -fno-omit-frame-pointer`，否则 `--call-graph fp` 可能只能得到不完整调用
栈。采样频率不要一开始设得很高，先观察 perf 自身开销。

### 10.3 能看见和看不见的时间

`perf` 能看见：

- CPU 执行的 C++ 函数；
- syscall 和驱动占用 CPU 的部分；
- 调度、上下文切换和 cache miss；
- 高 CPU busy loop。

`perf` 不能单独表示：

- Sensor 曝光等待；
- ISP/RGA 独立硬件运行的完整耗时；
- VOP 正在扫描 framebuffer 的时间；
- 外部 DSI LCD 行为。

这些必须结合应用帧时间戳、V4L2/DRM tracepoint 和硬件日志。

`perf trace` 是 perf 的 syscall/tracepoint 子命令，不是独立的 `trace` 程序；当前同样因
板端没有 `perf` 而不能使用。

## 11. ftrace：内核事件时序

### 11.1 为什么它适合 Camera Pipeline

V4L2、videobuf2、DRM 和 dma fence 都存在硬件中断/内核 worker 参与。应用线程可能只
看到一个阻塞 ioctl，而 ftrace 能记录：

```text
QBUF
-> driver/hardware
-> buffer done
-> DQBUF
-> DRM vblank queued
-> vblank delivered
```

### 11.2 五秒安全抓取示例

```sh
TRACE=/sys/kernel/debug/tracing
OUT=/userdata/camera-debug/ftrace-camera.txt

mkdir -p /userdata/camera-debug

echo 0 > "${TRACE}/tracing_on"
echo nop > "${TRACE}/current_tracer"
echo > "${TRACE}/trace"

echo 1 > "${TRACE}/events/v4l2/enable"
echo 1 > "${TRACE}/events/vb2/enable"
echo 1 > "${TRACE}/events/drm/enable"
echo 1 > "${TRACE}/events/dma_fence/enable"

echo 1 > "${TRACE}/tracing_on"
sleep 5
echo 0 > "${TRACE}/tracing_on"

cat "${TRACE}/trace" > "${OUT}"

echo 0 > "${TRACE}/events/v4l2/enable"
echo 0 > "${TRACE}/events/vb2/enable"
echo 0 > "${TRACE}/events/drm/enable"
echo 0 > "${TRACE}/events/dma_fence/enable"
```

这里故意不设置只跟踪 `camera_display_stream` PID，因为 V4L2 buffer complete、DRM
vblank、DMA fence signal 可能由 IRQ 或内核 worker 产生。只按用户进程 PID 过滤会漏掉
关键完成事件。

### 11.3 使用规则

- ftrace 是系统全局状态；
- 开始前确认没有其他测试正在使用 tracing；
- 只启用需要的 event group；
- 使用短窗口；
- 完成后关闭所有本次启用的事件；
- `trace_pipe` 是消费式读取，不适合作为事后再次读取的静态文件；
- `function_graph` 比 tracepoint 开销更高，只在 tracepoint 不够时使用。

## 12. 用户态内存故障

“heap over”可能指两种完全不同的问题：

```text
heap buffer overflow：写出了分配边界
heap exhaustion：内存用尽，无法继续分配
```

必须先区分。

### 12.1 Segmentation Fault

典型原因：

- 空指针或野指针；
- use-after-free；
- 数组越界；
- 已经 munmap 后继续访问；
- 错误的 buffer 长度/stride；
- stack overflow；
- 之前发生的内存破坏在稍后才表现为崩溃。

典型退出码为 139。推荐顺序：

```text
core + GDB
-> ASan/UBSan
-> Valgrind
-> dmesg 辅助判断
```

不要只使用最后一次崩溃指令推断根因。heap overflow 可能在第 100 帧发生，但直到第
1000 帧释放对象时才崩溃。

### 12.2 Stack overflow 与 stack buffer overflow

Stack overflow 常见于无限递归或非常深的调用链。GDB 中通常会看到大量重复栈帧：

```gdb
thread apply all bt
```

Stack buffer overflow 是局部数组越界。建议编译：

```text
-fstack-protector-strong
-fno-omit-frame-pointer
```

若 stack protector 发现 canary 被破坏，程序通常触发 SIGABRT，退出码可能是 134，而
不是 139。

ASan 常见报告名称：

```text
stack-overflow
stack-buffer-overflow
```

### 12.3 Heap buffer overflow、UAF 和 double free

AddressSanitizer 最适合定位：

- heap-buffer-overflow；
- use-after-free；
- stack-buffer-overflow；
- global-buffer-overflow；
- double-free；
- 部分泄漏。

交叉工具链包含 `libasan` 和 `libubsan`，但当前板端系统库目录没有 sanitizer 共享库。
调试版本可以静态链接，或者把共享库放在项目自己的目录后通过 `LD_LIBRARY_PATH` 使用，
不要直接污染板端 `/usr/lib`。

建议的专用构建参数：

```text
-O1
-g3
-fno-omit-frame-pointer
-fsanitize=address,undefined
```

链接时同样带：

```text
-fsanitize=address,undefined
-static-libasan
-static-libubsan
```

Sanitizer 版本只用于诊断，不作为正式常驻产物。它会增加内存、CPU 和地址空间开销，
也可能改变原来的竞争时序。

### 12.4 Valgrind

板端已安装 Valgrind：

```sh
valgrind \
  --tool=memcheck \
  --leak-check=full \
  --track-origins=yes \
  --num-callers=30 \
  --error-exitcode=99 \
  /home/reynor/camera-project/debug/camera_display_stream \
  --stream 5 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

Valgrind 会显著降低执行速度，可能人为制造 V4L2 timeout 或 page flip 延迟。优先用于：

- 短时运行；
- 不依赖实时帧率的状态机；
- 离线 buffer 处理；
- 模拟设备测试；
- 内存泄漏和未初始化读取。

它不能检查 RGA/VOP 硬件内部 DMA 越界，也不能检测内核驱动内存破坏。

### 12.5 Heap exhaustion、内存泄漏和 OOM

内存耗尽可能表现为：

- `new` 抛出 `std::bad_alloc`；
- 系统持续变慢；
- OOM killer 杀死进程；
- 进程收到 SIGKILL，shell 看到 137；
- DMA/CMA 分配失败，但普通 `free` 看起来仍有内存。

检查：

```sh
free -m

PID=$(pidof camera_display_stream)
cat "/proc/${PID}/status"

dmesg |
grep -Ei 'out of memory|oom|killed process|cma|dma allocation'
```

判断思路：

- `VmRSS` 持续增长：优先怀疑用户态泄漏；
- fd 数持续增长：优先怀疑 fd/DMA-BUF/DRM handle 生命周期；
- `MemAvailable` 下降但进程 RSS 不变：检查内核 slab、page cache、DMA/CMA；
- 只有固定分辨率或长稳后失败：核对 buffer/stride、每轮 rebuild 是否释放完整；
- exit 137 必须查 `dmesg`，不能仅凭退出码断定 OOM，因为 `kill -9` 也是 137。

## 13. Kernel Oops 与 Kernel Panic

### 13.1 与用户态崩溃的区别

```text
用户态 SIGSEGV
  -> 通常只终止 camera_display_stream
  -> 内核、ADB、网络通常继续工作

Kernel Oops
  -> 内核发现严重错误
  -> 可能杀当前任务，也可能继续运行在不可靠状态

Kernel Panic
  -> 内核停止或按照策略重启
  -> ADB、网络和屏幕都可能立即消失
```

如果只有 Camera 进程退出而 ADB 正常，先按用户态问题处理。如果整板失联、串口出现
`Unable to handle kernel paging request`、`Oops`、`Call trace` 或 `Kernel panic`，再转向
内核调试。

### 13.2 必须提前保存的构建证据

每个板端内核版本必须归档：

- 未 strip 的 `vmlinux`；
- `System.map`；
- `.config`；
- 精确内核源码 commit；
- DTB/DTS；
- 内核模块及其符号；
- 编译器版本；
- rootfs/BSP 版本；
- 与版本关联的应用 ELF 和 Build ID。

只有压缩后的 `Image`/`boot.img` 通常不足以进行完整地址解析。

### 13.3 串口是 Panic 的主要证据通道

Panic 时 ADB 依赖的 USB、调度和用户态服务都可能停止，因此必须使用独立调试 UART：

```text
PC 串口程序持续记录
-> 从 U-Boot/内核启动开始保存
-> 运行 Camera 故障测试
-> Panic 后仍保留最后输出
```

串口参数应以当前板卡手册和实际 U-Boot 配置为准，不能凭通用 RK3568 参数猜测。

### 13.4 pstore

开发板已经挂载：

```text
pstore on /sys/fs/pstore type pstore
```

异常重启后第一时间检查：

```sh
ls -lh /sys/fs/pstore

for file in /sys/fs/pstore/*; do
    [ -f "${file}" ] || continue
    echo "===== ${file} ====="
    cat "${file}"
done
```

当前目录为空不代表功能一定失效，只表示当前没有保留下来的记录。必须在可恢复、已备份
的实验环境中验证 ramoops reserved-memory、写入和跨重启保留是否完整。

不要直接在当前业务板上主动触发 Panic。主动 Panic 可能导致文件系统、日志和数据库
写入中断，测试前必须明确备份、供电、串口和恢复流程。

### 13.5 解析内核调用栈

使用与板端完全匹配的 `vmlinux`：

```bash
/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-addr2line \
  -f \
  -C \
  -e /path/to/vmlinux \
  0xffffff8008123456
```

或者在对应内核源码树中：

```bash
scripts/decode_stacktrace.sh \
  /path/to/vmlinux \
  /path/to/System.map \
  < panic.log
```

地址必须来自同一次完整日志。重新编译后即使源码看起来相同，地址和内联关系也可能改变。

### 13.6 内核 heap/stack 越界

用户态 GDB、Valgrind 和 ASan不能检查内核驱动内存。若怀疑 rkisp、RGA、DRM 或 vendor
driver 内部越界，需要单独构建诊断内核：

- KASAN；
- UBSAN；
- 更严格的 SLUB debug；
- frame pointer 和完整 debug info；
- 必要的 lockup/hung-task detector；
- 串口和 pstore。

这些功能开销很大，可能改变实时性和内存布局，只用于实验镜像，不能直接作为性能或
正式版本。

## 14. 按现象选择工具

| 现象 | 第一工具 | 第二工具 | 主要证据 |
| --- | --- | --- | --- |
| 程序退出 20 | 应用日志 | strace+dmesg+V4L2 ftrace | 哪个采集操作失败 |
| 程序退出 30 | 应用日志 | strace+dmesg | RGA ioctl/参数/硬件错误 |
| 程序退出 40 | DRM clients+dmesg | strace+DRM ftrace | master/modeset/flip/vblank |
| 画面冻结但进程存在 | GDB all-thread bt | strace+ftrace | 等待点和内核完成事件 |
| CPU 过高 | top/perf stat | perf record | 热点函数/调度/cache |
| RSS 持续增长 | `/proc/<pid>/status` | ASan/Valgrind | 泄漏分配栈 |
| fd 持续增长 | `/proc/<pid>/fd` | strace | 哪类 fd 未关闭 |
| exit 139 | core+GDB | ASan | fault address 和破坏来源 |
| exit 134 | core+GDB | stack protector/ASan 日志 | abort 原因 |
| exit 137 | dmesg | 内存/服务日志 | OOM 还是人为 SIGKILL |
| ADB 消失、整板停住 | 串口 | pstore+vmlinux | Kernel Oops/Panic/lockup |
| 只在长稳中失败 | 有界资源监控 | core/ftrace/日志轮转 | 泄漏、累积错误、温升 |

## 15. 推荐的标准调试流程

### 阶段 1：不附加调试器

```text
1. 记录程序版本、命令行和 Git commit
2. 记录开始时 dmesg、内存、fd 和 DRM clients
3. 前台运行并保留 stdout/stderr
4. 记录退出码
5. 保存结束时 dmesg 和资源快照
```

很多配置、设备占用和驱动错误在这一阶段就能定位。

### 阶段 2：系统调用定位

```text
1. 短时间 strace
2. 只跟踪 open/ioctl/poll/mmap/close
3. 找到最后成功和第一个失败 syscall
4. 用 dmesg 对齐相同时间窗口
```

### 阶段 3：用户态源码定位

```text
1. 使用 RelWithDebInfo
2. gdbserver attach 或从启动运行
3. catch throw
4. 崩溃时保存 all-thread bt full
5. 必要时启用 core
```

### 阶段 4：内存和时序工具

```text
内存破坏 -> ASan/UBSan -> Valgrind
内核时序 -> V4L2/vb2/DRM/dma_fence ftrace
CPU 性能 -> perf stat/record
```

### 阶段 5：内核故障

```text
1. 串口持续记录
2. 保存 pstore
3. 锁定完全匹配的 vmlinux/System.map/config/source
4. 解析第一个 Oops，而不是只看后续连锁报错
5. 必要时构建 KASAN/SLUB debug 实验内核
```

## 16. 调试操作的安全边界

- GDB 附加会暂停 Camera worker，未来 supervisor/watchdog 必须进入维护模式；
- `strace`、Valgrind、ASan、ftrace 和 perf 都会改变原始时序；
- `SIGKILL` 不会执行项目的 STREAMOFF/DRM/RAII 正常清理，只用于最后手段；
- `core_pattern`、ftrace、sysctl 是系统级状态，测试后必须恢复；
- 不把 core、trace、原始帧和无限日志写入根分区；
- 不在没有备份、串口和恢复路径时主动触发 Kernel Panic；
- 不把 sanitizer/debug kernel 的性能结论当成 Release 性能；
- 不用另一个版本的 ELF 或 `vmlinux` 解析旧 core/Panic；
- 调试时仍要保持 V4L2 buffer、DMA-BUF、RGA 和 DRM framebuffer 的所有权规则。

## 17. 最重要的判断顺序

遇到问题时始终按以下顺序：

```text
进程是否存在
-> 退出码是什么
-> 应用最后一个 Lifecycle/故障域是什么
-> dmesg 有没有驱动/OOM/Oops
-> strace 最后一个 syscall/ioctl 是什么
-> GDB 所有线程停在哪里
-> 是否需要 core/ASan/Valgrind
-> 是否需要 ftrace/perf
-> 整板失联才转向 UART+pstore+vmlinux
```

这套顺序可以避免看到“画面不动”就直接归咎于摄像头，也避免把程序主动返回的 display
40 误判成 Kernel Panic。
