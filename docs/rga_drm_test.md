# RK3568 离线 NV12 经 RGA 写入 DRM framebuffer

## 1. 阶段目标

本阶段在不接入 V4L2 实时采集的前提下，单独验证以下链路：

```text
CPU生成已知1920x1080 NV12测试图
        |
        v
DRM dumb GEM -> DMA-BUF fd
        |
        v
RGA同步作业：旋转270° + NV12/BT.709 limited -> BGRX8888
        |
        v
1080x1920 DRM dumb framebuffer DMA-BUF
        |
        v
legacy modeset -> VOP -> DSI panel
```

本阶段不存在整帧 CPU `memcpy` 到显示 buffer。CPU 只在初始化时生成一次测试图；
真正的源读取、旋转、颜色转换和目标写入由 RGA DMA 完成。

## 2. 为什么先使用离线测试图

如果直接接入相机，画面错误可能来自 V4L2 buffer 所有权、NV12 offset、RGA 参数、
DRM pitch、屏幕方向或颜色空间。离线图把输入固定为已知的红、绿、蓝、白、黑色条，
可以先回答：

- DRM GEM 能否导出并被 RGA 作为 DMA-BUF 使用。
- RGA 是否接受 1920x1080 NV12 和带对齐的 XRGB8888 目标 stride。
- 270° 旋转后的尺寸是否正好为 1080x1920。
- `RK_FORMAT_BGRX_8888` 的内存字节顺序是否匹配 little-endian
  `DRM_FORMAT_XRGB8888`。
- 同步作业返回后，VOP 是否可以安全扫描目标 framebuffer。

## 3. 内存对象

源 buffer 使用 `DRM_IOCTL_MODE_CREATE_DUMB` 按 8 bpp、1920x1620 请求。1620 行由
1080 行 Y 和 540 行 UV 组成。驱动返回的实际 `pitch` 和 `size` 才是后续依据：

```text
Y offset  = 0
UV offset = source_pitch * 1080
required  = source_pitch * (1080 + 540)
```

它只是一块 GEM 存储，不创建 KMS framebuffer ID。`DrmDumbBuffer::dmaBufFd()` 用
`drmPrimeHandleToFD()` 导出拥有型 DMA-BUF fd。

目标使用已有 `DrmDumbFramebuffer` 创建 1080x1920 XRGB8888 framebuffer。其实际
pitch 在当前板上预期为 4352 字节，因此 RGA 目标像素 stride 为 `4352 / 4 = 1088`，
而不是假设为可见宽度 1080。

源和目标 fd 都是同一内存对象的共享句柄，不是物理地址。资源释放顺序为：

```text
关闭CRTC
-> close PRIME DMA-BUF fd
-> drmModeRmFB（仅目标）
-> munmap
-> DESTROY_DUMB
-> 释放DRM master
```

实际代码保证先停止显示，再释放 framebuffer/GEM；对象析构提供异常路径兜底。

## 4. CPU 与 DMA 同步

源图由 CPU 通过 GEM mmap 写入。导出 DMA-BUF 后，写入前后分别执行：

```text
DMA_BUF_IOCTL_SYNC(START | WRITE)
CPU写Y和UV
DMA_BUF_IOCTL_SYNC(END | WRITE)
```

这样把 CPU 访问域明确交还给设备。RGA 使用同步 `IM_SYNC` 作业，成功返回后才能把
目标 framebuffer 交给 VOP 扫描。

## 5. RGA 参数

```text
source:
  format       RK_FORMAT_YCbCr_420_SP
  visible      1920x1080
  wstride      DRM源buffer实际pitch（NV12下单位等于像素）
  hstride      1080
  colorspace   IM_YUV_TO_RGB_BT709_LIMIT

destination:
  format       RK_FORMAT_BGRX_8888
  visible      1080x1920
  wstride      DRM目标pitch / 4
  hstride      1920
  colorspace   IM_COLOR_SPACE_DEFAULT

usage:
  IM_HAL_TRANSFORM_ROT_270 | IM_SYNC
```

程序先调用 `imcheck_t()`，通过后才调用 `improcess()`。本阶段使用板端 SDK 自带的
`librga.so.2.1.0`，不替换板端库，以保持和当前 RGA2 内核驱动的 BSP 配套关系。

首次实板调用曾按相机当前元数据尝试 `IM_YUV_BT709_FULL_RANGE -> IM_RGB_FULL`，
板载 librga 明确返回 `Unsupported full csc mode`。因此离线硬件通路使用该版本明确
支持的 BT.709 limited。接入真实相机前必须优先验证 ISP 能否输出 limited range；
不能把 full-range NV12 静默按 limited 解释，否则黑白电平和整体对比度会错误。

## 6. 构建、部署和运行

所有 AArch64 产物必须从 WSL 交叉构建，再通过 ADB 放入板端
`/home/reynor/camera-project`：

```bash
./tools/cross_build_rk3568.sh
ADB=/home/reynor/tools/platform-tools/adb ./tools/deploy_rk3568.sh
```

部署结果包括：

```text
/home/reynor/camera-project/bin/rga_drm_test
/home/reynor/camera-project/scripts/run_rga_drm_test_rk3568.sh
```

推荐运行入口：

```bash
cd /home/reynor/camera-project/scripts
./run_rga_drm_test_rk3568.sh 10
```

脚本会停止 systemui、厂商 camera 和 Weston，运行测试后自动恢复桌面。只有明确希望
保持桌面停止时才使用：

```bash
./run_rga_drm_test_rk3568.sh 10 --keep-desktop-stopped
```

## 7. 验收状态

- [x] GEM 存储与 framebuffer 均实现 DMA-BUF 导出和 fd 生命周期管理。
- [x] NV12 源图考虑实际 pitch、UV offset 和 DMA-BUF CPU 同步。
- [x] RGA 参数检查、同步旋转/转换和错误信息已实现。
- [x] `rga_drm_test` 通过 AArch64 C++11 `-Werror` 交叉构建。
- [x] 板端 `ldd` 确认 `libdrm.so.2`、`librga.so.2` 等依赖完整。
- [x] 板端 `imcheck_t` 和 `improcess` 成功。
- [ ] 屏幕人工确认色条方向、顺序和颜色正常。
- [x] 退出后 Weston/systemui 恢复且无 DRM client/GEM 遗留。

本次 RK3568 实板运行结果：

```text
rga_drm_test 0.8.0
RGA_api version: v1.3.1_[11]
RGA hardware: RGA_2_Enhance
source:      1920x1080 NV12, pitch=1920, DMA-BUF fd=4
destination: 1080x1920 XRGB8888, pitch=4352, DMA-BUF fd=5
rotation:    270 degrees
RGA elapsed: 4722 us
```

`run_rga_drm_test_rk3568.sh 10` 运行 10 秒并以 0 退出。退出后 Weston 重新
成为 DRM master，systemui 恢复，DSI-1 恢复为 `1080x1920p54`；DRM client
列表中无 `rga_drm_test`。由于 ADB 无法判断面板上色条的主观颜色和方向，人工
视觉确认仍保留为单独验收项。

下一阶段把离线源替换为 V4L2 `VIDIOC_EXPBUF` 得到的真实相机 DMA-BUF，
并先实现“取一帧、RGA 同步处理、显示、再 QBUF”的单帧链路。该阶段必须
同时查询和校验 V4L2 返回的 `colorspace`/`ycbcr_enc`/`quantization`，不得默认真实
相机与本离线 limited-range 测试图相同。

## 8. 参考资料

- [Rockchip librga 官方示例](https://github.com/airockchip/librga/blob/main/samples/im2d_api_demo/rgaImDemo.cpp)
- [Rockchip RGA 开发指南](https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_CN.md)
