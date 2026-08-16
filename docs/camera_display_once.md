# RK3568 真实相机单帧 DMA-BUF 经 RGA 显示

## 1. 阶段目标

本阶段把离线 NV12 源替换为 `/dev/video0` 真实采集帧，验证第一条端到端
零 CPU 整帧拷贝链路：

```text
RKISP /dev/video0
  -> V4L2 MMAP buffer
  -> VIDIOC_EXPBUF（同一存储的DMA-BUF fd）
  -> VIDIOC_DQBUF（buffer归应用）
  -> RGA同步读取、旋转270°、NV12转XRGB8888
  -> 独立DRM dumb framebuffer DMA-BUF
  -> VIDIOC_QBUF（RGA返回后立即归还源buffer）
  -> DRM modeset -> VOP -> DSI panel
```

程序名为 `camera_display_once`。它只取一帧，用于把格式、颜色元数据、
DMA-BUF 导出、RGA 和 DRM 错误分层；本阶段尚不实现连续帧 page flip。

## 2. 真实内存与所有权

V4L2 buffer 由 RKISP capture driver 分配。MMAP 地址和 `VIDIOC_EXPBUF` 返回的 fd
引用同一块 DDR 存储，导出时没有 `memcpy`。当前实板布局为：

```text
1920x1080 NV12
memory plane count = 1
pitch              = 1920 bytes
Y offset           = 0
UV offset          = 1920 * 1080
sizeimage          = 3110400 bytes
```

所有权转换严格按以下顺序：

```text
QueuedToCapture
  -> VIDIOC_DQBUF
Ready / application owns source
  -> RGA improcess(IM_SYNC)
RGA done / source no longer read
  -> VIDIOC_QBUF
QueuedToCapture again
  -> VIDIOC_STREAMOFF（单帧测试结束capture）
```

RGA 写入的是另一块 DRM GEM buffer，因此 VOP 后续扫描不会阻止源 capture
buffer 归还摄像头。这一点与“直接把 V4L2 buffer 作为 scanout”不同。

## 3. 颜色元数据与当前限制

程序通过 `VIDIOC_S_FMT` 明确请求：

```text
colorspace   Rec.709
xfer_func    Rec.709
ycbcr_enc    Rec.709
quantization Limited Range
```

RKISP 实际 `G_FMT` 回填：

```text
colorspace=3     V4L2_COLORSPACE_REC709
xfer=1           V4L2_XFER_FUNC_709
ycbcr=2          V4L2_YCBCR_ENC_709
quantization=1   V4L2_QUANTIZATION_FULL_RANGE
```

这证明当前 BSP 接受 Rec.709 矩阵元数据，但强制输出 Full Range。板载 librga
1.3.1 的普通 CSC 只提供 BT.709 Limited、BT.601 Limited 和 BT.601 Full，没有
BT.709 Full。默认运行因此会在 RGA 前拒绝，防止静默产生错误颜色。

为了把“BT.601/BT.709 矩阵差异”和“Full/Limited 量化范围差异”拆开，程序提供
三个板载 RGA 实际支持的强制诊断模式：

```text
bt601-limited
bt601-full
bt709-limited
```

强制模式会有意忽略驱动回填的颜色元数据，只改变 RGA 对同一类 NV12 capture
buffer 的解释方式。旧的 `--allow-bt601-full-fallback` 仍兼容，并等价于
`bt601-full`。这些模式只是诊断入口，不是最终产品配置。

每次运行还会统计 NV12 有效 Y 区域，跳过 stride 尾部和 UV 数据，输出
`min/p01/p50/p99/max/mean` 及超出 limited 名义范围的像素数量。任意自然场景都不
保证包含绝对黑白，因此不能只凭一帧的 min/max 下结论；必须分别采集受控黑场和
受控白场。

最终方案应优先让 ISP/BSP 正确输出 BT.709 Limited；如果无法修改，则需选择
支持 BT.709 Full 的新 RGA 运行库/驱动组合，或引入经证实的 GPU/custom CSC
路径。

## 4. 构建、部署与运行

所有产物必须交叉构建后部署到板端 `/home/reynor/camera-project`：

```bash
./tools/cross_build_rk3568.sh
ADB=/home/reynor/tools/platform-tools/adb ./tools/deploy_rk3568.sh
```

自动检查驱动元数据（当前 BSP 报告 BT.709 Full，因此会明确失败）：

```bash
cd /home/reynor/camera-project/scripts
./run_camera_display_once_rk3568.sh 10 /dev/video0
```

三种硬件 CSC 对照显示：

```bash
./run_camera_display_once_rk3568.sh 10 /dev/video0 bt601-limited
./run_camera_display_once_rk3568.sh 10 /dev/video0 bt601-full
./run_camera_display_once_rk3568.sh 10 /dev/video0 bt709-limited
```

脚本负责停止 systemui、厂商 camera 和 Weston，以及在正常退出或错误后恢复
桌面。

推荐测试顺序：

1. 完全遮住镜头并固定场景，运行任意一个模式，记录 Y 统计。如果黑场的主要分布
   靠近 16，更符合 Limited；靠近 0，更符合 Full。
2. 使用均匀明亮白色目标重复测试。如果高光能接近 235 且很少超过 235，更符合
   Limited；能稳定扩展到 255，则更符合 Full。自动曝光可能使普通白纸达不到上限，
   所以黑场判断通常更可靠。
3. 保持相机和场景不动，对比 `bt601-limited` 与 `bt709-limited`。二者亮度范围相同，
   差别主要应体现在红、绿、蓝的色相和饱和度；若亮度明显变化，应继续检查 RGA/BSP。
4. 保持场景不动，对比 `bt601-limited` 与 `bt601-full`。二者矩阵相同；如果 Full
   模式黑色抬升、画面发白，实际样本更符合 Limited。

直方图只能较可靠地诊断量化范围。判断 BT.601 或 BT.709 矩阵需要标准色卡、已知
光源以及参考颜色值；仅凭任意自然画面的观感不能形成严格结论。

## 5. RK3568 实板结果

```text
camera_display_once 0.9.0
source buffer       = 0
source sequence     = 2
source DMA-BUF fd   = 4
destination fd      = 10
destination pitch   = 4352 bytes
rotation            = 270 degrees
RGA mode            = BT.601 full diagnostic fallback
RGA elapsed         = 7147 us
```

- [x] 真实 V4L2 MMAP buffer 的每个 plane 可导出 DMA-BUF。
- [x] DQBUF 后 RGA 可直接读取 capture DMA-BUF，无 CPU 整帧拷贝。
- [x] RGA 同步返回后源 buffer 已 QBUF，随后 STREAMOFF 成功。
- [x] 目标 DRM framebuffer 显示 10 秒并正常清理。
- [x] Weston 重新成为 DRM master，systemui 和 `1080x1920p54` DSI 输出恢复。
- [x] 严格路径能发现并拒绝 BT.709 Full/BT.709 Limited 不匹配。
- [ ] 现场人工确认捕获的真实画面方向和内容正确。
- [ ] 最终 BT.709 Full 精确转换方案闭环。

下一小阶段已经实现：保留 capture streaming，引入两个 DRM 目标 buffer，按
`DQBUF -> RGA -> page flip event -> 回收旧目标 buffer` 连续显示。由于 RGA 完成后就
不再读源帧，V4L2 buffer 可在每次 RGA 返回后立即 QBUF，不必等 page flip。详见
[连续相机 RGA 双缓冲翻页显示](camera_display_stream.md)。
