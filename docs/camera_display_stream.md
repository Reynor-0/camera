# RK3568 连续相机 DMA-BUF 经 RGA 双缓冲翻页显示

## 1. 本阶段目标

本阶段把已经实板验证的单帧链路扩展为持续动态显示：

```text
RKISP /dev/video0
  -> 4 个 V4L2 MMAP capture buffers
  -> 每个 memory plane 导出 DMA-BUF fd
  -> 同步 RGA：NV12 旋转 270°并转换 XRGB8888
  -> 2 个独立 DRM dumb framebuffers
  -> drmModePageFlip + flip-complete event
  -> VOP/DSI/LCD
```

该阶段仍使用 legacy KMS 和同步 RGA，目的是先证明持续 capture、转换和翻页的
buffer 所有权正确。它还不是最终的异步多级流水线。

## 2. 两类 buffer 的所有权

V4L2 capture pool 使用 4 个 buffer。`DQBUF` 后应用暂时拥有当前 buffer；同步
`improcess(... IM_SYNC)` 返回时，RGA 已经停止读取源 DMA-BUF，因此应用立即将其
`QBUF` 归还 ISP。RGA 输出写入独立 DRM GEM，不要求 V4L2 buffer 等待屏幕扫描。

DRM 目标使用 A/B 两块 framebuffer：

```text
初始：RGA 写 A -> drmModeSetCrtc(A)

循环：
  VOP 正在扫描 A
  RGA 只写 B
  drmModePageFlip(B)
  等待 flip-complete
  VOP 正在扫描 B，A 才恢复可写
  RGA 只写 A
```

`pageFlipAndWait()` 返回之前，上一块 framebuffer 仍可能被 VOP 扫描，绝不能被
RGA 覆盖。当前同步设计用 `writable_framebuffer` 在 A/B 间交替，代码中不存在
同时在途的第二个 page flip。

## 3. 构建、部署和运行

交叉构建并统一部署到板端 `/home/reynor`：

```bash
./tools/cross_build_rk3568.sh
ADB=/home/reynor/tools/platform-tools/adb ./tools/deploy_rk3568.sh
```

在开发板运行 10 秒：

```bash
cd /home/reynor
./run_camera_display_stream_rk3568.sh 10 /dev/video0 bt709-limited
```

脚本停止 systemui、厂商 camera 和 Weston；程序退出后关闭 CRTC，脚本再恢复桌面。
当前 BSP 报告 BT.709 Full，但板载 librga 不支持该模式，因此本阶段继续用已明确
授权的 `bt709-limited` 诊断模式推进动态链路，不能把它视为最终颜色结论。

## 4. 验收指标

程序结束时输出：

- captured/displayed frame 数量；
- 已完成 page flip 数量；
- V4L2 ERROR frame、timeout 和 sequence gap；
- 实际 capture/display FPS；
- RGA 单帧最短、平均和最长同步耗时；
- 第一帧和最后一帧 sequence；
- CRTC 是否安全关闭。

双缓冲同步模型应满足：

```text
displayed_frames = captured_frames
completed_page_flips = displayed_frames - 1
error_frames = 0
timeouts = 0
sequence_gaps = 0
```

10 秒功能验证通过后，应逐步扩展到 60 秒和 10 分钟稳定性测试，再进入异步 RGA、
三 DRM framebuffer 或统一 poll event loop 的性能阶段。

## 5. RK3568 实板结果

版本 `0.10.0` 在 `Linux 4.19.232`、`rkisp_v5`、板载 `librga 1.3.1` 和
`1080x1920p54` DSI 输出上完成 10 秒及 60 秒测试。60 秒结果：

```text
source                  1920x1080 NV12, stride=1920
capture buffers         4
DRM target buffers      2, XRGB8888, pitch=4352
captured/displayed      1796 / 1796
completed page flips    1795
V4L2 error frames       0
capture timeouts        0
sequence gaps           0
sequence                2..1797
elapsed                 60.019 s
capture/display FPS     29.92 / 29.92
RGA time                min 5194 us, avg 6883.88 us, max 8970 us
```

计数满足 `flips = displayed - 1`：第一帧通过 `drmModeSetCrtc` 建立初始扫描，后续
1795 帧均收到 flip-complete。测试退出后 CRTC 安全关闭，Weston 和 systemui 正常
恢复；内核日志未发现 RGA/IOMMU/DRM underrun 或 ISP 丢帧错误。

这证明同步双缓冲动态链路已经闭环，但尚不能替代 10 分钟/30 分钟压力测试，也未
解决驱动声明 BT.709 Full 而旧版 RGA 无对应 CSC 的颜色元数据问题。
