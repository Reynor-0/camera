# RK3568 V4L2 CaptureSession 会话重建（0.13.0）

## 1. L2 与上一阶段 L1 的区别

0.12.0 的 L1 recovery 复用同一个 video fd 和同一组 buffer：

```text
同一 V4L2 fd
同一组 MMAP / DMA-BUF
STREAMOFF -> QBUF all -> STREAMON
```

如果 STREAMOFF、QBUF 或 STREAMON 自己失败，这个 fd 和 buffer pool 的状态已经不能
可靠证明。0.13.0 增加 L2 session rebuild：

```text
销毁旧 V4L2BufferQueue
    -> best-effort STREAMOFF
    -> 关闭 DMA-BUF fd
    -> munmap
    -> REQBUFS(count=0)
关闭旧 /dev/video0 fd
    -> 重新 open /dev/video0
    -> S_FMT + G_FMT
    -> REQBUFS + QUERYBUF + mmap
    -> EXPBUF
    -> QBUF all
    -> STREAMON
    -> 验证新实际格式和颜色元数据
```

这会替换完整的 V4L2 用户态会话，但不会重启进程、RGA 或 DRM。

## 2. CaptureSession 真正拥有些什么

新增 `CaptureSession` 是 V4L2 会话资源的唯一上层所有者：

```text
CaptureSession
  |
  +-- CaptureSessionConfig
  |     设备路径、请求尺寸、fourcc、buffer 数和颜色请求
  |
  +-- V4L2Device
  |     唯一拥有 video fd
  |
  +-- V4L2BufferQueue
  |     借用 video fd
  |     唯一拥有 MMAP、DMA-BUF fd 和驱动 buffer pool
  |
  `-- VideoFormat
        每一代 G_FMT 返回的实际格式
```

成员声明和销毁顺序保证 `V4L2BufferQueue` 一定先于其借用的 `V4L2Device` 销毁。
`CaptureSession` 禁止复制和移动，防止 fd 或 buffer 所有权出现两个对象。

每次成功建立 session 会增加 `generation`：首次启动为 1，第一次 L2 成功后为 2。

## 3. 为什么必须先关闭旧 fd 再打开新 fd

RKISP capture node 代表同一条硬件 pipeline。如果旧 fd 和旧 queue 仍存在时就打开并
配置替代 fd，两个会话可能争夺格式、streaming 状态或驱动资源。

因此 `rebuild()` 不采用“先创建新会话，成功后再交换”的常见无停机模式，而是：

```text
queue_.reset()
device_.reset()
open replacement
build replacement queue
全部成功后提交为当前 session
```

代价是 rebuild 期间没有新帧；好处是硬件所有权清楚。如果替代 session 初始化失败，
对象保持没有活动 queue 的状态，顶层按 capture 故障退出，不会继续使用半初始化资源。

## 4. 哪些错误触发 L2

当前触发点包括：

- L1 的 STREAMOFF、QBUF 或 STREAMON 失败；
- V4L2 `poll()` 报告错误；
- `VIDIOC_DQBUF` 失败或返回无法验证的 metadata；
- 已由应用处理完成的 buffer 无法重新 QBUF；
- 错误帧在丢弃时无法重新 QBUF。

同步 RGA 已经读取完成后发生 QBUF 失败，旧源 buffer 不再被 RGA 使用，可以安全销毁
整个 capture session。如果 DQBUF metadata 无法验证，程序也不会尝试把所有权不明的
buffer 单独 QBUF，而是通过销毁 session 重新建立所有权。

## 5. L2 期间屏幕为什么不会立即消失

V4L2 capture buffer 和 DRM framebuffer 是两组独立存储。L2 只销毁前者：

```text
旧 V4L2 session 销毁 / 新 session 创建
                     |
DRM framebuffer 不变 + VOP 继续扫描最近一帧
```

因此短暂恢复期间屏幕保持最后一帧。新 session 验证成功后，下一帧继续经 RGA 写入未被
扫描的 DRM framebuffer，再正常 page flip。

## 6. 两级预算和退避

| 级别 | 动作 | 当前预算 | 退避 |
| --- | --- | --- | --- |
| L1 | 同 fd STREAMOFF/QBUF/STREAMON | 60 秒最多 3 次 | 无 |
| L2 | close/open 并重建完整 session | 60 秒最多 2 次 | 200 ms、400 ms |

L1 预算耗尽表示系统正在持续抖动，本次不会绕过限制继续 L2。L1 的实际 ioctl 失败才会
升级 L2。L2 第三次请求在 60 秒窗口内会被拒绝，worker 以 capture 域退出码 `20`
结束，避免反复 open/close 形成恢复风暴。

当前短退避在主线程执行，因此 SIGTERM 最多可能多等待约 400 ms。以后统一事件循环阶段
应改为 `timerfd`，让退避等待和停止信号同时可被 poll。

## 7. 新格式为什么必须重新验证

每次重新打开设备后，驱动都可以调整 S_FMT 请求。L2 成功并不等于原格式仍然成立，
所以程序重新检查：

- 实际尺寸仍为 1920x1080；
- fourcc 仍为 NV12；
- memory plane 数、stride 和 sizeimage 安全；
- 颜色空间、YCbCr encoding 和量化范围仍可由当前 RGA 模式处理。

只有验证通过才回到主循环。否则按 configuration 域退出，绝不会拿旧格式参数解释新
buffer。

## 8. 指标和日志

典型升级恢复日志：

```text
Recovery capture: FAILED level=L1 attempt=1
Recovery capture: STARTING level=L2 attempt=1 ... backoff_ms=200
Recovery capture: SUCCEEDED level=L2 attempt=1 generation=2 elapsed_ms=...
Recovery capture: VALIDATED level=L2 generation=2 ...
```

正常退出统计增加：

```text
Capture session recoveries: attempted=1 succeeded=1 failed=0 budget_exhausted=0
Capture session recovery total ms: ...
Final capture session generation: 2
```

恢复后第一帧重新建立 V4L2 sequence 连续性基线，后续 sequence 跳变继续累计。

## 9. 故障注入与 ADB 验收

下面两个选项组合使用：

```text
--inject-capture-timeout-recoveries N
--inject-stream-recovery-failures N
```

第二个选项在 L1 修改 queue 前抛出诊断失败，因此不会故意把真实驱动置于异常状态；
随后走与真实 L1 ioctl 失败相同的 L2 代码。

WSL 自动验收命令：

```bash
ADB=/home/reynor/tools/platform-tools/adb \
ADB_SERIAL=8b34888e45c927c6 \
  ./tools/test_camera_capture_recovery_rk3568.sh \
  /dev/video0 bt709-limited
```

脚本现在覆盖四组：

1. L1 一次成功并继续显示；
2. L1 第四次请求被预算拒绝；
3. L1 注入失败后 L2 一次成功并继续显示；
4. L2 前两次成功，第三次被独立预算拒绝。

每组结束都检查 worker PID、DRM client 和 `/dev/video0` fd 持有者。

## 10. RK3568 实板验收结果

板端版本：

```text
camera_display_stream 0.13.0 (C++11, target=rk3568-aarch64-linux)
```

注入一次 L1 失败后，L2 重建并继续运行：

```text
L1                       FAILED（诊断注入）
L2 attempt 1             SUCCEEDED，132 ms
new generation           2
revalidated format       1920x1080 NV12，stride=1920，BT.709 limited
captured/displayed       101 / 101
sequence gaps            0
final state/exit         STOPPED / 0
```

连续注入三次 L1 失败以验证 L2 预算：

```text
L2 attempt 1             SUCCEEDED，generation=2，128 ms
L2 attempt 2             SUCCEEDED，generation=3，131 ms
L2 attempt 3             BUDGET_EXHAUSTED
final domain/exit        capture / 20
```

扩展后的 ADB 脚本四组 L1/L2 用例全部 PASS。每组结束后均无 worker PID、DRM client
或 `/dev/video0` fd 持有者；随后 forever + SIGTERM 生命周期回归也通过。

## 11. 仍未实现的边界

- 新 session 第一次创建失败后不会在同一 worker 内反复 open；
- `/dev/video0` 消失时还没有 WaitingForDevice 状态和长期设备探测；
- 没有针对真实 Sensor 断电、CSI 错误或 ISP reset 的实物故障注入；
- RGA 和 DRM session 仍不会局部重建；
- 没有周期心跳、最后成功帧时间和 supervisor；
- 退避尚未接入统一可中断事件循环。

下一小阶段应实现 DRM session recovery：page flip/DRM event 失败后停止新的显示提交，
安全销毁 CRTC display 和两个 framebuffer，重新 probe/modeset，再继续使用仍健康的
CaptureSession。之后才适合迁移 Atomic KMS。
