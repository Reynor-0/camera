# RK3568 V4L2 到 DRM/KMS 实时显示设计

## 1. 文档目的

本文描述外部 Bayer Sensor 经 D-PHY 和硬件 ISP 处理后，通过 V4L2 capture node（例如
`/dev/video0`）输出视频帧，并在用户态使用 DMA-BUF 和 DRM/KMS 将画面实时显示
到屏幕上的整体设计。当前 media entity 使用 IMX415 名称，但设备树 compatible 为
IMX335，精确物理料号应以原理图、BOM 或芯片丝印为准。

当前开发板的摄像头为 1920x1080 横屏输出，DSI panel 为 1080x1920 竖屏，实际
落地需要 RGA 完成旋转。经过板端 media graph、RGA、DRM plane 和厂商相机程序
验证后的具体方案，见
[RK3568 摄像头到 DSI 屏幕：硬件拓扑、软件模型与实现方案](rk3568_camera_to_dsi_pipeline.md)。
已完成的“离线 NV12 DMA-BUF -> RGA 旋转/转换 -> DRM framebuffer”实板
验证、内存布局和板端运行方法见
[RK3568 离线 NV12 经 RGA 写入 DRM framebuffer](rga_drm_test.md)。
真实 V4L2 capture DMA-BUF 的单帧端到端所有权流程、实板数据和颜色空间限制见
[RK3568 真实相机单帧 DMA-BUF 经 RGA 显示](camera_display_once.md)。
本文保留通用 direct-scanout 架构和早期设计背景；涉及当前板卡的实现优先级、颜色
转换、双 buffer pool 和 Weston 退出方案时，以该详细文档为准。

第一阶段的目标是打通一条低延迟、无 CPU 图像拷贝的专用显示链路：

```text
Sensor -> CSI-2/D-PHY -> ISP -> V4L2/DDR -> RGA -> DRM/KMS/VOP -> DSI/D-PHY -> Panel
```

D-PHY、sensor 和 ISP 的驱动配置属于内核态及 media pipeline 范畴。用户态程序
以可用的 V4L2 capture node 为输入，并假定 ISP 已经能够输出 NV12、YUYV 等处理
后的图像格式。如果 video node 输出的仍然是 RAW Bayer 数据，则不能直接进入本文
描述的 KMS 显示路径，需要先由 ISP 完成 demosaic；RGA 的普通 YUV/RGB 转换不能
替代 Bayer ISP。

## 2. 当前项目评估

当前目录为：

```text
project1/
├── CMakeLists.txt
├── inc/
│   ├── v4l2_buffer.hpp
│   └── v4l2_device.hpp
├── src/
│   ├── v4l2_buffer.cpp
│   └── v4l2_device.cpp
├── tests/
├── tools/
└── docs/
```

`src`、头文件、测试和工具分目录的方向是合理的，但目前所有源码和 CMake 文件
均为空，尚未形成可编译的程序。当前结构还存在以下不足：

1. 缺少应用程序入口和命令行配置。
2. 缺少 DRM/KMS 显示模块。
3. 缺少 V4L2 与 DRM 之间的格式及内存布局协商。
4. `V4l2Buffer` 容易将共享帧错误地绑定到 V4L2 模块；一个帧还需要同时保存
   DMA-BUF、DRM GEM handle、framebuffer ID 和显示状态。
5. 缺少 buffer 所有权状态机、异常退出流程及硬件诊断工具。

建议将 `inc` 改为 `include/camdisplay`，使内部命名空间、CMake include path 和未来
安装导出关系更清楚。

## 3. 关键可行性条件

DMA-BUF 只解决跨设备共享内存的问题，不会自动完成图像转换。direct-scanout
必须同时满足以下条件。

### 3.1 像素格式兼容

V4L2 输出格式必须能映射为目标 KMS plane 支持的 DRM fourcc，例如：

| V4L2 格式 | DRM 格式 | 说明 |
| --- | --- | --- |
| `V4L2_PIX_FMT_NV12` | `DRM_FORMAT_NV12` | 首选，带宽较低 |
| `V4L2_PIX_FMT_YUYV` | `DRM_FORMAT_YUYV` | 取决于 KMS plane 能力 |
| 32-bit RGB 格式 | 对应 `DRM_FORMAT_XRGB8888` 等 | 容易显示，但内存带宽较大 |

除了 fourcc，还必须匹配每个 plane 的 stride、offset、内存对象数量和 format
modifier。不能因为两个设备都支持名为 NV12 的格式，就直接假定其内存布局一定
兼容。

### 3.2 DMA-BUF 导入能力

第一版推荐由 V4L2/ISP 驱动分配 capture buffer，然后通过 `VIDIOC_EXPBUF` 导出
DMA-BUF fd，由 DRM 作为 importer 导入。该方案让 ISP 驱动决定 DMA 内存、对齐和
IOMMU 映射，通常比先从 DRM 或通用 heap 分配内存更容易跑通。

运行时需要验证：

- V4L2 支持 `V4L2_MEMORY_MMAP` 和 `VIDIOC_EXPBUF`。
- DRM 设备具有 `DRM_PRIME_CAP_IMPORT`。
- 目标 plane 支持对应的 format/modifier。
- `drmModeAddFB2()` 或 `drmModeAddFB2WithModifiers()` 能接受实际布局。

### 3.3 显示系统所有权

直接操作 `/dev/dri/cardN` 通常要求应用获得 DRM master。专用嵌入式系统、没有
Weston/X11 的场景适合直接使用 KMS。如果系统已经运行 Wayland/X11 compositor，
应用不应抢占 DRM master；此时应增加 Wayland DMA-BUF/EGL 显示后端，由 compositor
管理最终显示。

`/dev/dri/renderD*` 只能用于渲染，不能替代 `cardN` 完成 KMS modeset。

## 4. 总体架构

```text
                         ┌──────────────────────┐
                         │   Command line/config│
                         └──────────┬───────────┘
                                    │
┌─────────────────┐       ┌────────▼─────────┐       ┌─────────────────┐
│  V4l2Capture    │──────▶│ DisplayPipeline │──────▶│   KmsDisplay    │
│ format/queue/DQ │ frame │ state/event loop │  FB  │ plane/CRTC/flip │
└────────┬────────┘       └────────┬─────────┘       └────────┬────────┘
         │                         │                          │
         └─────────────────────────▼──────────────────────────┘
                              DmaFrame pool
                   DMA-BUF / layout / state / timestamps
```

第一版采用单线程事件循环，同时监听 V4L2 fd 和 DRM fd。这样能把 capture、page
flip 和 buffer 回收放在同一个所有权模型中，避免过早引入跨线程同步。

## 5. 推荐目录结构

```text
project1/
├── CMakeLists.txt
├── cmake/
│   └── CompilerOptions.cmake
├── include/camdisplay/
│   ├── unique_fd.hpp
│   ├── error.hpp
│   ├── video_format.hpp
│   ├── dma_frame.hpp
│   ├── v4l2_capture.hpp
│   ├── kms_display.hpp
│   ├── format_negotiator.hpp
│   └── display_pipeline.hpp
├── src/
│   ├── v4l2_capture.cpp
│   ├── kms_display.cpp
│   ├── format_negotiator.cpp
│   └── display_pipeline.cpp
├── apps/
│   └── camera_display.cpp
├── tools/
│   ├── v4l2_probe.cpp
│   └── kms_probe.cpp
├── tests/
│   ├── format_negotiator_test.cpp
│   ├── frame_state_test.cpp
│   └── pipeline_test.cpp
└── docs/
    └── architecture.md
```

项目受目标开发板工具链限制，统一使用 ISO C++11、CMake 和 libdrm，禁止依赖
GNU 方言扩展或 C++14 及以上语言特性。第一版不引入 OpenCV、Qt 或其他可能产生
隐式图像拷贝的框架。最终程序通过 AArch64 工具链交叉编译，并运行在 64 位 Linux
用户态的 RK3568 开发板。详细要求参见[项目开发与注释规范](development_guidelines.md)
和 [RK3568 交叉编译指南](cross_compilation_rk3568.md)。

## 6. 核心数据模型

### 6.1 `UniqueFd`

对 Linux fd 进行不可复制、可移动的 RAII 封装，保证异常和提前返回时 fd 被正确
关闭。DMA-BUF fd、video fd 和 DRM fd 均使用该类型管理。

### 6.2 `VideoFormat`

统一保存 V4L2 与 DRM 需要共享的格式信息：

```text
width / height
V4L2 fourcc / DRM fourcc
single-planar or multi-planar
color space / YCbCr encoding / quantization
format modifier
per-plane pitch / offset / size
```

必须保存 `VIDIOC_S_FMT` 之后由 `VIDIOC_G_FMT` 返回的实际格式，不能继续使用应用
最初请求的假定值。

### 6.3 `DmaFrame`

图像 plane 和 DMA 内存对象必须分开描述：

```text
DmaFrame
├── capture buffer index
├── memory objects[]
│   ├── DMA-BUF fd
│   ├── DRM GEM handle
│   └── allocation size
├── image planes[]
│   ├── memory object index
│   ├── pitch
│   └── offset
├── DRM framebuffer ID
├── timestamp / sequence
└── ownership state
```

该模型可以同时支持：

- NV12：一个内存对象包含 Y、UV 两个图像 plane。
- NV12M：两个内存对象分别保存 Y 和 UV。
- 多个图像 plane 引用同一个 DRM GEM handle。

### 6.4 `V4l2Capture`

负责：

- `VIDIOC_QUERYCAP` 和格式枚举。
- single-planar/multi-planar API 适配。
- `S_FMT`、`G_FMT`。
- `REQBUFS`、`QUERYBUF`、`EXPBUF`。
- `QBUF`、`DQBUF`、`STREAMON`、`STREAMOFF`。
- 输出 capture timestamp、sequence、bytesused 和错误状态。

### 6.5 `KmsDisplay`

负责：

- connector、encoder、CRTC、plane 枚举和自动选择。
- 查询 plane 支持的 format/modifier。
- DMA-BUF fd 到 GEM handle 的 PRIME import。
- 创建和销毁 DRM framebuffer。
- Atomic `TEST_ONLY`、modeset 和 nonblocking page flip。
- DRM event 处理。
- 正常退出时禁用 plane 或恢复原显示状态。

所有 connector、CRTC 和 plane ID 都应在运行时查询，不能硬编码。

### 6.6 `FormatNegotiator`

从 V4L2 输出能力和 DRM plane 能力中选择交集，按以下顺序优先：

1. NV12 direct-scanout。
2. YUYV/UYVY direct-scanout。
3. RGB direct-scanout。
4. 返回“需要转换后端”，而不是继续提交一个已知不兼容的 framebuffer。

### 6.7 `DisplayPipeline`

负责 buffer 状态机、事件循环和丢帧策略，不负责底层 ioctl 细节。

## 7. Buffer 所有权和同步

每个 buffer 使用以下状态：

```text
QueuedToCapture
       │ DQBUF
       ▼
     Ready ───── dropped ─────┐
       │ atomic commit         │
       ▼                       │
PendingScanout                 │
       │ flip complete         │
       ▼                       │
  ScanningOut                  │
       │ 被下一帧替换           │
       └──────── QBUF ◀────────┘
```

必须遵循以下规则：

1. `QBUF` 后 buffer 属于 V4L2 驱动，用户态和 DRM 不能开始新的写操作。
2. `DQBUF` 后 ISP 已经完成该帧写入，buffer 才能提交显示。
3. 当前正在扫描输出的 buffer 不能重新 `QBUF`，否则 ISP 可能覆盖正在显示的内容。
4. 提交下一帧时请求 page-flip event。
5. 收到 flip-complete event 后，上一帧才可以重新进入 capture queue。
6. 任意时刻最多存在一个 pending page flip。

第一版建议申请 4 个 capture buffer：一个正在 scanout、一个可能 pending、其余留给
ISP 采集。实际数量需要结合驱动规定的最小队列深度调节。

当摄像头帧率高于屏幕刷新/提交速度时，采用“最新帧优先”策略：保留最新的 Ready
帧，把尚未显示的旧 Ready 帧直接重新 QBUF。这样牺牲部分帧完整性以控制端到端
延迟。

## 8. 程序运行流程

### 8.1 初始化

1. 解析 video node、DRM card、输出格式、分辨率和 buffer 数量。
2. 打开 V4L2 设备，查询能力及格式。
3. 打开 DRM card，启用 universal-plane 和 atomic client capability。
4. 选择 connector、CRTC 和 plane。
5. 计算 V4L2/DRM 格式交集。
6. 设置 V4L2 格式，并读取驱动返回的实际布局。
7. 以 `V4L2_MEMORY_MMAP` 请求 capture buffers。
8. 对每个 memory plane 执行 `VIDIOC_EXPBUF`。
9. 将 DMA-BUF 导入 DRM，并创建 framebuffer ID。
10. 使用 Atomic `TEST_ONLY` 验证 plane 配置。
11. 将所有 capture buffer QBUF，然后 STREAMON。

`VIDIOC_EXPBUF` 属于 MMAP buffer 模式的扩展，但 direct-scanout 路径不要求程序
为了 CPU 访问而实际 mmap 每一个 buffer。

### 8.2 事件循环

事件循环至少监听：

- V4L2 fd：新的 capture buffer 可以 DQBUF。
- DRM fd：page flip 已完成。
- signal fd 或等价机制：SIGINT/SIGTERM 安全退出。

主逻辑：

```text
video fd readable:
    dequeue all available frames
    discard stale Ready frames
    if no flip is pending:
        commit newest frame

DRM flip event:
    recycle previous scanout frame to V4L2
    promote pending frame to current scanout
    if a newer Ready frame exists:
        commit it
```

### 8.3 正常退出

资源释放顺序为：

```text
停止提交新帧
-> 禁用 KMS plane 或恢复原显示状态
-> 等待最后一次 atomic commit 完成
-> VIDIOC_STREAMOFF
-> 删除 DRM framebuffer ID
-> 关闭 GEM handles
-> 关闭 DMA-BUF fds
-> 释放 V4L2 buffers
-> 关闭设备 fd
```

释放仍在使用的 framebuffer 可能会禁用 plane；仍被导出的 V4L2 buffer 也可能使
`REQBUFS(count=0)` 返回 `EBUSY`，因此销毁顺序必须显式实现并测试。

## 9. 功能开发顺序与验收标准

### 阶段 0：目标板能力探测

建议先执行：

```bash
media-ctl -p -d /dev/media0
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
drm_info
modetest -c
modetest -p
```

需要形成一份目标板能力记录：

- video node 和 media topology。
- capture buffer 类型。
- V4L2 格式、分辨率、stride 和 sizeimage。
- connector/CRTC/plane 关系。
- plane 支持的 format/modifier。
- PRIME import 和 Atomic KMS 能力。
- compositor 占用情况。

验收标准：找到至少一组明确兼容的 V4L2/DRM format-layout 组合，或者明确判定
必须增加转换后端。

### 阶段 1：V4L2 独立采集

功能点：

1. `O_NONBLOCK` 打开设备。
2. 查询并设置格式。
3. 请求 4 个 MMAP buffer。
4. QBUF、STREAMON。
5. 使用 poll、DQBUF、QBUF 连续采集。
6. 输出 FPS、sequence、timestamp 和错误帧统计。
7. 支持 SIGINT 安全退出。

验收标准：

- 连续采集 5 至 10 分钟无卡死。
- sequence 基本连续，错误帧有明确日志。
- FPS 符合 sensor/ISP 配置。
- 能保存单帧，并离线确认尺寸、UV 顺序和颜色正确。

### 阶段 2：DRM/KMS 独立显示

先用 DRM dumb buffer 显示 RGB 色条，实现：

- [x] connector、CRTC 自动选择。
- [x] 单 framebuffer 首次 legacy modeset。
- [ ] plane 自动选择和 Atomic `TEST_ONLY`。
- 双 framebuffer page flip。
- flip event 处理。
- [x] 退出时关闭测试 CRTC，由板端脚本恢复 Weston。

验收标准：持续翻页无撕裂，不硬编码 DRM object ID，并能处理无连接显示器等错误。

### 阶段 3：单帧 DMA-BUF direct-scanout

实现 V4L2 `EXPBUF`、DRM PRIME import 和 `AddFB2`，只显示一帧并保持该 buffer
不重新进入 capture queue。

验收标准：静态画面的颜色、宽高、UV 顺序、stride 和裁剪全部正确。

如果本阶段失败，应优先检查：

- V4L2 fourcc 到 DRM fourcc 的映射。
- single-planar 和 multi-planar 的差异。
- pitch 和 offset。
- linear/tiled/compressed modifier。
- DRM PRIME import 和 IOMMU 限制。

### 阶段 4：实时 direct-scanout

实现单线程事件循环、4-buffer 状态机、flip-complete 后回收和最新帧优先策略。

验收标准：

- 连续显示 30 分钟无花屏、死锁和 fd 泄漏。
- ISP 不会覆盖正在 scanout 的 buffer。
- captured、displayed、dropped 统计自洽。
- Ctrl+C 可以可靠恢复显示并退出。

### 阶段 5：工程化

增加：

- 命令行参数和配置校验。
- requested/actual format 完整日志。
- `EINTR`、`EAGAIN`、`ENODEV`、`EPIPE` 处理。
- stream restart。
- HDMI hotplug。
- color encoding/range 属性配置。
- 单元测试和目标板集成测试。

## 10. 命令行接口建议

```text
camera-display \
  --video /dev/video0 \
  --drm /dev/dri/card0 \
  --width 3840 \
  --height 2160 \
  --format NV12 \
  --buffers 4 \
  [--connector-id N] \
  [--plane-id N] \
  [--mode preferred] \
  [--drop-policy latest]
```

如果没有指定 connector、CRTC 或 plane，应自动选择，并在日志中输出最终选择结果。

## 11. 测试设计

### 11.1 单元测试

- V4L2 fourcc 到 DRM fourcc 的格式映射。
- NV12/NV12M memory-object 与 image-plane 建模。
- connector/CRTC/plane 选择算法。
- buffer 状态合法转换。
- dropped frame 策略。
- 部分初始化失败时的 RAII 清理。

### 11.2 目标板集成测试

- 采集 100、1000、持续 30 分钟帧测试。
- Atomic `TEST_ONLY` 能力测试。
- 单帧 direct-scanout。
- 摄像头 30 FPS、屏幕 60 Hz 的持续显示。
- SIGINT/SIGTERM 退出。
- HDMI 拔插。
- STREAMOFF 后重新启动。
- 通过 `/proc/<pid>/fd` 检查 fd 泄漏。

在进入目标板测试前，先按照[虚拟摄像头测试指南](virtual_camera_testing.md)使用
vivid 在开发主机覆盖 single-planar 和 multi-planar V4L2 分支，并使用 vimc 验证
media-controller 相关开发。虚拟驱动不能模拟 RKISP、RKVOP、IOMMU 或真实 DMA-BUF
跨设备兼容性，因此不能替代本节目标板验收。

### 11.3 运行指标

至少记录：

- captured frames。
- displayed frames。
- dropped frames。
- V4L2 error frames。
- capture FPS 和 display FPS。
- DQBUF timestamp 与 flip timestamp。
- 当前/峰值 Ready queue 深度。

## 12. 格式不兼容时的后备路径

按以下顺序处理：

1. 调整 ISP 输出为 KMS plane 原生支持的 NV12/YUYV。
2. 选择支持 YUV 的 overlay plane。
3. 使用 SoC 2D blitter、RGA 或 V4L2 M2M 转换。
4. 将 DMA-BUF 导入 EGLImage，使用 GPU shader 做 YUV 到 RGB 转换。
5. 仅在调试或保底场景使用 CPU 转换和 memcpy。

显示层可以在后续演进为：

```text
DisplayBackend
├── DirectScanoutBackend   # 零拷贝，第一阶段实现
├── GpuConvertBackend      # 格式不兼容时使用
└── CpuConvertBackend      # 调试和保底
```

不建议第一版就实现三个后端，但公共帧模型和 pipeline 不应依赖某一个具体后端。

## 13. 第一版完成定义

满足以下条件即可认为 MVP 闭环：

- 支持一个固定的 NV12 或 YUYV 分辨率。
- V4L2 MMAP buffer 能导出 DMA-BUF。
- DRM 能通过 PRIME 导入并创建 framebuffer。
- 使用 Atomic KMS 全屏显示。
- 具备至少 4 个 buffer 的所有权管理。
- 收到 flip event 后才回收上一帧。
- 提供实时 FPS 和 dropped-frame 统计。
- Ctrl+C 安全退出并释放全部资源。
- 在目标板连续运行 30 分钟无花屏、死锁和明显资源泄漏。

## 14. 参考资料

- [V4L2 Streaming I/O (Memory Mapping)](https://docs.kernel.org/userspace-api/media/v4l/mmap.html)
- [V4L2 DMA-BUF importing](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html)
- [VIDIOC_EXPBUF](https://docs.kernel.org/userspace-api/media/v4l/vidioc-expbuf.html)
- [VIDIOC_QBUF / VIDIOC_DQBUF](https://docs.kernel.org/userspace-api/media/v4l/vidioc-qbuf.html)
- [DMA-BUF buffer exchange](https://docs.kernel.org/userspace-api/dma-buf-alloc-exchange.html)
- [DRM userspace API](https://docs.kernel.org/gpu/drm-uapi.html)
- [DRM/KMS](https://docs.kernel.org/gpu/drm-kms.html)

V4L2 MMAP 连续采集阶段见
[V4L2 MMAP 连续采集计划](v4l2_mmap_capture_plan.md)，DMA-BUF 导出阶段见
[DMA-BUF 导出开发计划](dma_buf_export_plan.md)。当前已完成的 DRM 资源探测与未绑定
Dumb Buffer 生命周期验证见 [DRM/KMS 资源探测](drm_probe.md)。

真实相机单帧链路见[单帧 DMA-BUF 经 RGA 显示](camera_display_once.md)；在此基础上
实现的持续 V4L2 streaming、双 DRM framebuffer 和同步 flip-complete 所有权闭环见
[连续相机 RGA 双缓冲翻页显示](camera_display_stream.md)。
