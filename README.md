# RK3568 Camera Display

本项目在 RK3568 Buildroot 开发板上验证摄像头采集、DMA-BUF、RGA 硬件旋转/颜色
转换以及 DRM/KMS 独占显示链路。

当前已跑通的主链路为：

```text
RKISP V4L2 -> DMA-BUF -> RGA -> 双 DRM framebuffer
            -> legacy page flip -> VOP -> DSI LCD
```

它目前是同步双缓冲 demo，尚不是完整的无人值守工业服务。项目状态、Atomic KMS、
异步流水线和故障恢复路线见：

- [文档导航](docs/README.md)
- [工业化、Atomic KMS 与异步流水线](docs/industrial_camera_service_atomic_async.md)
- [当前连续显示实现](docs/camera_display_stream.md)
