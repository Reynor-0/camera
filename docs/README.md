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
4. [长期 worker 与确定性退出](camera_worker_lifecycle.md)：查看 0.11.0 已完成的第一步
   工业化实现和实板验收。
5. [V4L2 采集流局部恢复](capture_stream_recovery.md)：理解 0.12.0 的 L1 stream
   recovery、恢复预算和故障注入。
6. [V4L2 CaptureSession 会话重建](capture_session_recovery.md)：理解 0.13.0 的 L2
   close/open、buffer pool 重建和独立预算。
7. [RK3568 Camera 常驻进程调试指南](rk3568_camera_debugging_guide.md)：学习应用日志、
   dmesg、strace、GDB/core、Valgrind/ASan、ftrace、perf 和 Kernel Panic 的分层定位方法。
8. [倒车影像实战项目设计](reverse_camera_practical_project.md)：在当前实板链路上加入
   CAN 倒挡控制、无 MCU 模拟、gPTP 时间模型和明确的 DRM 不自主恢复边界。
9. [停车场车牌识别与 SQLite 入库设计](parking_lot_lpr_project.md)：把持续采集显示基线
   扩展为 HyperLPR3 低频识别、多帧确认和板端轻量数据库入库系统。
10. [车载网关与倒车影像共存方案](rk3568_gateway_reverse_camera_project.md)：基于实板
   CAN、双 Ethernet/PTP、BusyBox 和现有 V4L2/RGA/DRM 能力设计双进程域控制器原型。
11. [项目总体架构](architecture.md)：查看更长期的 direct-scanout 目标和模块划分。

## 当前已实板验证

- [V4L2 采集与 DMA-BUF 导出计划/验收](dma_buf_export_plan.md)
- [DRM 资源、dumb buffer、legacy modeset 和 page flip](drm_probe.md)
- [离线 NV12 经 RGA 写入 DRM](rga_drm_test.md)
- [真实相机单帧显示](camera_display_once.md)
- [真实相机连续同步显示](camera_display_stream.md)
- [长期 worker、退出码与 SIGTERM 清理](camera_worker_lifecycle.md)
- [V4L2 采集超时、L1 stream recovery 与恢复预算](capture_stream_recovery.md)
- [V4L2 L2 CaptureSession rebuild](capture_session_recovery.md)

## 构建、部署与板端运维

- [交叉编译与部署](cross_compilation_rk3568.md)
- [开发与代码规范](development_guidelines.md)
- [RK3568 Camera 常驻进程调试指南](rk3568_camera_debugging_guide.md)
- [Weston/systemui 开机启动管理](board_desktop_autostart.md)

## 设计与后续开发

- [RK3568 倒车影像实战项目设计](reverse_camera_practical_project.md)
- [RK3568 Linux 停车场车牌识别与 SQLite 入库项目设计](parking_lot_lpr_project.md)
- [RK3568 Linux 车载网关与倒车影像共存技术方案](rk3568_gateway_reverse_camera_project.md)
- [工业化、Atomic KMS 与异步流水线](industrial_camera_service_atomic_async.md)
- [项目总体架构](architecture.md)
- [V4L2 MMAP 连续采集早期计划](v4l2_mmap_capture_plan.md)

## 开发机测试

- [vivid/vimc 虚拟摄像头测试](virtual_camera_testing.md)
