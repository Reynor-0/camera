# RK3568 V4L2 采集流局部恢复（0.12.0）

## 1. 这一阶段解决什么问题

`camera_display_stream` 0.11.0 遇到连续 3 次采集超时会直接退出。0.12.0 把这个行为
改成第一层局部恢复（L1 stream recovery）：

```text
连续 3 次，每次等待 1000 ms 仍没有相机帧
    -> 检查 60 秒恢复预算
    -> VIDIOC_STREAMOFF
    -> 对原来的 4 个 MMAP buffer 重新 VIDIOC_QBUF
    -> VIDIOC_STREAMON
    -> 继续采集、RGA 转换和 DRM 翻页
```

恢复只重启 RKISP/V4L2 的 capture queue，不重启进程，不重新分配图像内存，也不重建
RGA、DRM framebuffer 或 KMS 显示状态。

## 2. 为什么可以复用原来的 buffer

`VIDIOC_STREAMOFF` 成功后，V4L2 驱动停止向 buffer 写入并清空 queue，所有 buffer
所有权回到应用。它不会自动解除现有 `mmap`，也不会关闭 `VIDIOC_EXPBUF` 得到的
DMA-BUF fd。因此原 buffer pool 可以按下面的状态重新使用：

```text
Streaming
   |
   | VIDIOC_STREAMOFF
   v
BuffersAllocated（4 个 buffer 都归应用）
   |
   | QBUF buffer 0..3
   v
BuffersAllocated（4 个 buffer 都归驱动）
   |
   | VIDIOC_STREAMON
   v
Streaming
```

恢复入口只位于 `poll()` 连续超时路径。此时程序没有持有一块已经 DQBUF、仍被 RGA
读取的源 buffer，所以执行 STREAMOFF 不会破坏正在进行的 RGA 作业。

DRM 端与 V4L2 端使用不同的物理存储。采集恢复期间 VOP 继续扫描最近一次成功显示的
DRM framebuffer，因此屏幕会暂时停在最后一帧，而不是变黑或显示未完成的数据。

## 3. 恢复预算为什么必要

如果 Sensor、CSI 或 ISP 已经永久故障，无限制执行 STREAMOFF/ON 会产生重启风暴：

- CPU 和内核反复执行 ioctl；
- 日志不断增长；
- 真正的故障原因被大量重复信息淹没；
- 可能进一步干扰驱动恢复。

当前固定策略是：

| 参数 | 当前值 |
| --- | --- |
| 单次 poll 超时 | 1000 ms |
| 触发恢复的连续超时数 | 3 |
| 滑动时间窗口 | 60 s |
| 窗口内最多恢复尝试 | 3 |

每次真正开始恢复时记录一个单调时钟时间。尝试第四次时，如果前三次仍处于最近 60 秒
窗口内，本次恢复会被拒绝，worker 以 capture 域退出码 `20` 结束。以后由 supervisor
决定是否经过更长退避后重新启动 worker。

这不是“最多只能恢复三次”。运行足够久以后，早于 60 秒的记录会移出窗口，预算会
自然恢复。

## 4. 日志和指标怎么看

开始一次恢复：

```text
Recovery capture: STARTING level=L1 attempt=1 \
  reason=consecutive-timeouts attempts_in_window=1
```

恢复成功：

```text
Recovery capture: SUCCEEDED level=L1 attempt=1 elapsed_ms=...
```

预算耗尽：

```text
Recovery capture: BUDGET_EXHAUSTED level=L1 window_seconds=60 limit=3
Lifecycle: FAILED domain=capture
Exit code: 20
```

正常结束时还会输出：

```text
Capture stream recoveries: attempted=1 succeeded=1 failed=0 budget_exhausted=0
Capture recovery total ms: ...
```

V4L2 `sequence` 可能在 STREAMOFF/ON 后继续递增，也可能从较小值重新开始。恢复后的
第一帧只建立新的 sequence 连续性基线，不会把驱动合法重置误计成大量丢帧；之后的
跳号仍继续累计。

## 5. 故障注入如何工作

真实断开 Sensor/CSI 可能影响 BSP 状态，不适合作为每次编译后的自动测试。因此程序
提供显式诊断选项：

```bash
--inject-capture-timeout-recoveries N
```

它在至少 10 帧正常显示后，让用户态控制流连续 N 组把 `frame_ready` 当作 false，
从而经过与真实 poll 超时相同的计数、预算和 STREAMOFF/ON 代码。它不会修改驱动、
设备树、Sensor 或 ISP，也不声称验证了真实硬件断流。

一次恢复成功测试：

```bash
/home/reynor/camera-project/bin/camera_display_stream \
  --stream 4 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  --inject-capture-timeout-recoveries 1 \
  /dev/video0 /dev/dri/card0
```

四组注入会在 60 秒内消费前三次预算，并让第四次按 capture 错误退出：

```bash
/home/reynor/camera-project/bin/camera_display_stream \
  --stream 10 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  --inject-capture-timeout-recoveries 4 \
  /dev/video0 /dev/dri/card0
```

在 WSL 中可运行完整的两用例和资源审计：

```bash
ADB=/home/reynor/tools/platform-tools/adb \
ADB_SERIAL=8b34888e45c927c6 \
  ./tools/test_camera_capture_recovery_rk3568.sh \
  /dev/video0 bt709-limited
```

测试前必须已停止 Weston、systemui 和厂商 camera。测试脚本不会修改或恢复开机配置。
每个用例结束后都会检查：

- `camera_display_stream` PID 已消失；
- DRM debugfs clients 没有残留 worker；
- 没有进程继续持有 `/dev/video0`。

## 6. RK3568 实板验收结果

板端版本：

```text
camera_display_stream 0.12.0 (C++11, target=rk3568-aarch64-linux)
```

一次注入恢复后继续运行 4 秒：

```text
L1 recovery             1 次，成功，123 ms
captured/displayed      108 / 108
sequence gaps           0
final state             STOPPED
exit                    0
```

在 60 秒窗口内注入四组超时：

```text
attempt 1               成功，127 ms
attempt 2               成功，135 ms
attempt 3               成功，123 ms
attempt 4               BUDGET_EXHAUSTED
final domain/code       capture / 20
```

两组用例后均无 `camera_display_stream` PID、DRM client 或 `/dev/video0` fd 持有者。
随后原有 forever + SIGTERM 生命周期回归也通过。

这些数字来自用户态故障注入，证明真实 RK3568 驱动能够完成 STREAMOFF/重新
QBUF/STREAMON 并继续出帧，但还没有覆盖物理断开 Sensor 或 CSI 错误。

## 7. 本阶段明确没有实现什么

0.12.0 是阶段 II 的 L1 子阶段，不应被误认为完整自动恢复：

- STREAMOFF、QBUF 或 STREAMON 本身失败时，还不会重开 `/dev/video0`；
- `ENODEV/EIO/EPIPE` 还不会进入 WaitingForDevice 或 L2 session rebuild；
- RGA 失败仍按 transform 域退出；
- page flip 失败仍按 display 域退出，不会重建 DRM session；
- 尚无 supervisor、指数退避、心跳或配置文件；
- 故障注入验证的是控制面，不替代拔插、断流和 24/72 小时实测。

下一小阶段应把 V4L2 设备、格式和 buffer pool 封装为可销毁重建的
`CaptureSession`。L1 失败或可识别的 V4L2 设备错误先进入 L2 session rebuild；超过
L2 预算才让 worker 退出。随后再单独实现 DRM session recovery。
