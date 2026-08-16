# Camera 项目文档导航

本文档目录同时包含“已经实板验证的实现”和“后续设计/历史开发计划”。阅读时应先确认
文档描述的是当前事实还是目标方案。

## 新手推荐阅读顺序

1. [摄像头到 DSI 的完整链路](rk3568_camera_to_dsi_pipeline.md)：先区分物理硬件、
   SoC 内部 IP、传输协议、内核对象和用户态对象。
2. [连续相机显示实现](camera_display_stream.md)：理解当前已经跑通的
   V4L2 -> DMA-BUF -> RGA -> DRM 双缓冲链路。
3. [工业化、Atomic KMS 与异步流水线](industrial_camera_service_atomic_async.md)：理解
   当前 demo 的工程化缺口和后续路线。
4. [项目总体架构](architecture.md)：查看更长期的 direct-scanout 目标和模块划分。

## 当前已实板验证

- [V4L2 采集与 DMA-BUF 导出计划/验收](dma_buf_export_plan.md)
- [DRM 资源、dumb buffer、legacy modeset 和 page flip](drm_probe.md)
- [离线 NV12 经 RGA 写入 DRM](rga_drm_test.md)
- [真实相机单帧显示](camera_display_once.md)
- [真实相机连续同步显示](camera_display_stream.md)

## 构建、部署与板端运维

- [交叉编译与部署](cross_compilation_rk3568.md)
- [开发与代码规范](development_guidelines.md)
- [Weston/systemui 开机启动管理](board_desktop_autostart.md)

## 设计与后续开发

- [工业化、Atomic KMS 与异步流水线](industrial_camera_service_atomic_async.md)
- [项目总体架构](architecture.md)
- [V4L2 MMAP 连续采集早期计划](v4l2_mmap_capture_plan.md)

## 开发机测试

- [vivid/vimc 虚拟摄像头测试](virtual_camera_testing.md)

