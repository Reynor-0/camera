# RK3568 Camera Worker 长期运行与确定性退出（0.11.0）

## 1. 本阶段目标和边界

本阶段把 `camera_display_stream` 从只能定时运行的功能 demo，推进为可以长期运行并
接受服务停止信号的同步 worker。

数据路径没有改变：

```text
4 x V4L2 MMAP/EXPBUF
    -> 同步 RGA NV12 转 XRGB8888 + 270°旋转
    -> 2 x DRM dumb framebuffer
    -> legacy SetCrtc/PageFlip + flip event
```

本阶段只改控制面：

- 支持运行到收到停止信号；
- 明确生命周期状态和停止原因；
- 正常退出统一执行 V4L2/DRM/buffer 清理；
- 运行失败按 configuration/capture/transform/display/internal 分类；
- 提供稳定退出码，供未来 supervisor 决定恢复策略；
- 提供可重复执行的 SIGTERM 实板验收工具。

本阶段还没有实现：

- V4L2 stream 自动重启；
- RGA/DRM 局部重建；
- 指数退避、熔断和 worker supervisor；
- Atomic KMS；
- 异步 RGA、fence 或三缓冲。

## 2. 两种运行模式

### 2.1 定时测试模式

底层程序：

```bash
/home/reynor/camera-project/bin/camera_display_stream \
  --stream 10 \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

到达 10 秒后，停止原因是：

```text
Stop reason: duration-elapsed
```

推荐通过板端包装脚本运行：

```bash
/home/reynor/camera-project/scripts/run_camera_display_stream_rk3568.sh \
  10 /dev/video0 bt709-limited --keep-desktop-stopped
```

### 2.2 长期 worker 模式

底层程序：

```bash
/home/reynor/camera-project/bin/camera_display_stream \
  --run-forever \
  --confirm-desktop-stopped \
  --color-mode bt709-limited \
  /dev/video0 \
  /dev/dri/card0
```

推荐包装脚本：

```bash
/home/reynor/camera-project/scripts/run_camera_display_stream_rk3568.sh \
  forever /dev/video0 bt709-limited --keep-desktop-stopped
```

包装脚本的 `forever` 会转换为底层程序的 `--run-forever`。它不修改 Weston 的开机
配置；当前板端已经通过独立 `board-admin` 脚本禁用桌面开机启动。

## 3. 生命周期状态机

当前 `PipelineController` 只管理顶层控制状态，不拥有任何 fd 或 buffer：

```text
STARTING
   | 设备、buffer、DRM session 和 STREAMON 成功
   v
RUNNING
   | 时间到 / SIGINT / SIGTERM
   v
STOPPING
   | STREAMOFF + CRTC关闭 + framebuffer释放成功
   v
STOPPED

任意阶段发生异常 -> FAILED -> RAII逆序回滚 -> 非零退出码
```

日志中的稳定标记为：

```text
Lifecycle: STARTING
Lifecycle: RUNNING
Lifecycle: STOPPING reason=SIGTERM
Lifecycle: STOPPED
```

未来的 supervisor 不应通过模糊字符串猜测进程状态；正式服务阶段会在此基础上增加
机器可读状态/心跳。本阶段日志标记主要用于验收和人工诊断。

## 4. SIGINT/SIGTERM 处理

signal handler 只记录收到的第一个信号：

```text
SIGINT/SIGTERM
    -> 写入 sig_atomic_t
    -> poll 返回或当前帧完成
    -> PipelineController 进入 STOPPING
    -> 主控制流执行清理
```

handler 不执行 ioctl、iostream、malloc 或资源释放，避免违反异步信号安全规则。

信号可能在以下等待期间到达：

- V4L2 `poll`：`EINTR` 返回后立即检查控制器，不计为 capture timeout；
- 同步 RGA：当前 job 返回后继续清理，不能在硬件读取源 buffer 时强制 QBUF；
- page flip wait：等待已提交 flip 完成，然后在下一次循环检查信号。

SIGINT 和 SIGTERM 都视为正常停止，完成清理后返回 0。日志会分别记录 `SIGINT` 或
`SIGTERM`，方便区分人工 Ctrl-C 和服务管理器停止。

## 5. 正常清理顺序

当前数据路径的 V4L2 源 buffer 与 DRM 目标 buffer 相互独立，因此本阶段采用：

```text
停止处理新帧
    -> VIDIOC_STREAMOFF
    -> 取得 completed flip 计数
    -> 关闭/恢复 CRTC 并释放 DRM master
    -> framebuffer B release
    -> framebuffer A release
    -> 其余 V4L2 DMA-BUF、mmap、REQBUFS 和 fd 由对象逆序析构
```

正常清理成功会输出：

```text
Cleanup V4L2: STREAMOFF complete
Cleanup DRM: CRTC safely disabled
Cleanup buffers: framebuffer release complete
Lifecycle: STOPPED
```

如果显式清理发生异常，局部对象的析构函数仍执行不抛异常的兜底回滚，并输出：

```text
Lifecycle: FAILED domain=<domain>
Cleanup: RAII rollback executed; external resource audit is required
```

“RAII rollback executed”不等于可以忽略外部核对。故障测试仍应检查 DRM clients、
`/dev/video0` 持有者和进程 PID。

## 6. 故障域和退出码

| 退出码 | 故障域 | 当前含义 | 未来 supervisor 建议 |
| --- | --- | --- | --- |
| 0 | success | 时间到、SIGINT 或 SIGTERM 后正常退出 | 不作为崩溃处理 |
| 1 | runtime | 未归类运行故障的保留码 | 报警并受控重启 |
| 2 | usage | 命令行参数错误 | 不重启，修正配置 |
| 10 | configuration | 格式、颜色元数据或固定约束不兼容 | 不忙重试 |
| 20 | capture | V4L2 打开、ioctl、超时或 buffer 错误 | 后续先局部恢复 |
| 30 | transform | RGA 调用失败 | 后续重建 RGA 域 |
| 40 | display | DRM 打开、modeset、flip 或清理失败 | 后续重建显示域 |
| 50 | internal | 生命周期/状态不变量或未知异常 | 完整重启并报警 |

异常分类依靠主循环在每次进入 V4L2、RGA、DRM 或内部状态操作前设置当前故障域。
底层异常的原始操作、设备路径、errno 和错误文本仍然保留。

## 7. 板端包装脚本的信号转发

包装脚本现在把 worker 作为子进程运行并保存 PID：

```text
包装脚本收到 INT  -> kill -INT worker
包装脚本收到 TERM -> kill -TERM worker
包装脚本收到 HUP  -> kill -TERM worker
```

脚本不会立刻退出，而是继续 `wait`。只有 worker 已完成 V4L2/DRM 清理并退出后，脚本
才执行自己的 `EXIT` trap。这样可以避免 worker 仍持有 `/dev/dri/card0` 时脚本提前
恢复其他显示服务。

## 8. 自动化生命周期验收

在 WSL 中运行一轮：

```bash
ADB=/home/reynor/tools/platform-tools/adb \
ADB_SERIAL=8b34888e45c927c6 \
  ./tools/test_camera_display_lifecycle_rk3568.sh \
  1 /dev/video0 bt709-limited
```

执行三轮：

```bash
ADB=/home/reynor/tools/platform-tools/adb \
ADB_SERIAL=8b34888e45c927c6 \
  ./tools/test_camera_display_lifecycle_rk3568.sh \
  3 /dev/video0 bt709-limited
```

脚本支持 1..1000 次迭代。每轮执行：

1. 通过一个持续存在的前台 ADB 会话启动板端 forever wrapper；
2. 等待 `camera_display_stream` PID 出现；
3. 允许真实链路运行约 2 秒；
4. 对 worker 发送 SIGTERM；
5. 等待 worker 和包装脚本退出；
6. 检查停止原因、STREAMOFF、framebuffer 清理和 STOPPED 标记；
7. 检查 debugfs 中没有残留 `camera_display` DRM client。

之所以不使用远端 `nohup ... &`，是因为本板旧版 adbd/BusyBox shell 会在创建它的
远端 shell 结束时终止后台任务。让 WSL 持有前台 ADB 会话可以避免这个差异。

## 9. RK3568 实板结果

版本：

```text
camera_display_stream 0.11.0 (C++11, target=rk3568-aarch64-linux)
```

定时 3 秒验证：

```text
stop reason          duration-elapsed
captured/displayed   86 / 86
completed flips      85
error/timeouts/gaps  0 / 0 / 0
cleanup              STREAMOFF + CRTC disabled + framebuffer release
exit                 0
```

forever + SIGTERM 单轮记录：

```text
stop reason          SIGTERM
captured/displayed   56 / 56
completed flips      55
error/timeouts/gaps  0 / 0 / 0
elapsed              2.021 s
cleanup              三项标记完整
exit                 0
```

随后连续执行 3 轮 lifecycle test，三轮全部 PASS。每轮结束后：

- `camera_display_stream` PID 不存在；
- `/sys/kernel/debug/dri/0/clients` 没有 camera client；
- 没有进程持有 `/dev/video0`。

无破坏性失败注入结果：

```text
/dev/video99 不存在             -> capture       exit 20
BT.709 Full + auto 不受旧RGA支持 -> configuration exit 10
/dev/dri/not-present 不存在      -> display       exit 40
```

三类异常后同样没有残留 DRM client 或 `/dev/video0` 持有者。

## 10. 当前完成定义与下一步

本阶段的功能实现、AArch64 `-Werror` 构建、额外启用 `-Wconversion`
和 `-Wsign-conversion` 的严格构建、定时退出、SIGTERM 三轮重复验收和三类失败退出码
验证已经完成。1..1000 次压力工具已经提供，但本次只执行了 3 轮，不能把它等同于
1000 轮或 24 小时长稳报告。

后续 0.12.0 已进入“同步模式下的局部恢复”，其中 L1 stream recovery、恢复预算和
故障注入见[V4L2 采集流局部恢复](capture_stream_recovery.md)。阶段 II 的完整路线仍为：

1. 单个错误帧保持当前丢弃策略；
2. 连续采集超时后执行一次 `STREAMOFF -> QBUF all -> STREAMON`；
3. stream restart 失败后重建 V4L2 session；
4. page flip 失败后安全重建 DRM session；
5. 增加恢复次数、恢复耗时、退避和恢复预算。
