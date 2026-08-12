# 使用 vivid 和 vimc 进行本机 V4L2 测试

## 1. 目的

x86_64 开发机没有真实 `/dev/video0` 时，可以使用 Linux 内核的虚拟媒体驱动验证
V4L2 用户态程序。对本项目最有价值的两个驱动是：

- **vivid**：Virtual Video Test Driver。它能生成测试图案，支持 MMAP、USERPTR、
  DMA-BUF streaming I/O、多种 YUV/RGB 格式以及 single/multi-planar API。
- **vimc**：Virtual Media Controller Driver。它模拟 sensor、debayer、scaler 和
  capture node 组成的 media pipeline，更接近真实 ISP/media-controller 拓扑。

当前第一阶段主要验证 `QUERYCAP`、`ENUM_FMT`、`ENUM_FRAMESIZES`、`S_FMT` 和
`G_FMT`，因此应首先使用 vivid。vimc 更适合后续验证 media topology 配置。

## 2. 能验证和不能验证的内容

| 内容 | vivid/vimc | RK3568 实板仍需验证 |
| --- | --- | --- |
| 设备打开、错误处理 | 可以 | 是 |
| `VIDIOC_QUERYCAP` | 可以 | 是 |
| single/multi-planar 分支 | vivid 可以同时覆盖 | 是 |
| 格式和尺寸枚举 | 可以 | 是 |
| `S_FMT/G_FMT` actual format | 可以 | 是 |
| MMAP、QBUF/DQBUF、帧状态机 | vivid 可以 | 是 |
| 测试图案和 FPS/sequence | vivid 可以 | 是 |
| media-controller topology | vimc 可以 | 是 |
| RKISP/IMX415 私有行为 | 不可以 | 必须 |
| ISP 输出的真实 stride/modifier | 不可以 | 必须 |
| ISP DMA-BUF 导入 DRM | 不能代表 RK3568 | 必须 |
| RKVOP plane 格式与缩放能力 | 不可以 | 必须 |
| RK3568 IOMMU/cache coherency | 不可以 | 必须 |

因此虚拟设备可以证明程序对标准 V4L2 UAPI 的基本使用正确，但不能证明 RK3568
上的最终 zero-copy pipeline 一定成立。

## 3. 环境要求

确认当前内核提供模块：

```bash
modinfo vivid
modinfo vimc
```

如果命令提示找不到模块，需要安装与当前运行内核完全匹配的 extra modules 包，或
启用以下内核配置后重新构建内核：

```text
CONFIG_VIDEO_VIVID=m
CONFIG_VIDEO_VIMC=m
```

Ubuntu/Debian 可选安装 `v4l-utils`，它提供 `v4l2-ctl`、`media-ctl` 和
`v4l2-compliance`：

```bash
sudo apt install v4l-utils
```

项目自身的第一阶段测试不强制依赖 `v4l-utils`。

加载模块需要宿主机 root 和 `CAP_SYS_MODULE`。在非特权 Docker/容器中，即使容器
内用户是 root，也可能得到：

```text
modprobe: ERROR: could not insert 'vimc': Operation not permitted
```

这种错误表示运行环境禁止修改宿主内核模块，不代表 V4L2 程序或模块本身有问题。
需要在宿主机终端执行加载命令，或为专用测试容器配置相应 capability；不建议为了
测试给日常容器开放完整 privileged 权限。

## 4. 自动创建虚拟摄像头

先构建本机程序：

```bash
cmake -S . -B build
cmake --build build -j
```

加载测试模块：

```bash
sudo ./tools/virtual_camera_modules.sh load
```

脚本创建两个仅含 capture node 的 vivid 实例：

```text
instance 0: V4L2 single-planar capture API
instance 1: V4L2 multi-planar capture API
```

它还会加载 vimc，并列出创建出的所有 `/dev/video*` 节点。设备编号不一定从 0
开始，不能在测试脚本中硬编码 `/dev/video0`。

随时查看状态：

```bash
./tools/virtual_camera_modules.sh status
```

## 5. 运行项目测试

自动测试所有 vivid capture node：

```bash
./tools/test_virtual_v4l2_probe.sh
```

测试至少验证：

- `camera_demo` 能成功探测虚拟 capture node。
- 输出包含格式列表。
- 至少覆盖一个 single-planar node。
- 至少覆盖一个 multi-planar node。

也可以手动运行输出中列出的节点：

```bash
./build/camera_demo /dev/video0
./build/camera_demo /dev/video1
```

设置 vivid webcam 支持的离散格式示例：

```bash
./build/camera_demo /dev/video0 640 360 YUYV
```

注意：命令行 FOURCC 必须正好四个字符。应先观察格式枚举输出，再将其中一个 fourcc
原样传给设置格式命令；不同内核版本和 single/multi-planar 实例公开的格式列表可能
不同。

## 6. 使用 v4l-utils 交叉验证

如果安装了 `v4l-utils`，可将项目输出与标准工具比较：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-compliance -d /dev/video0
media-ctl -p
```

`camera_demo` 报告的 driver、card、capture API、fourcc 和尺寸应与 `v4l2-ctl`
一致。`v4l2-compliance` 的范围远大于项目自身测试，某些未被当前应用使用的 API
失败不一定阻塞当前里程碑，但应保存结果供后续分析。

## 7. vimc 的特殊说明

vimc 的目标是模拟一条复杂 media pipeline：

```text
virtual sensor -> debayer -> scaler -> capture node
```

能力探测和格式枚举可以直接进行；真正 STREAMON 前，各个已连接 subdevice 的 pad
格式必须兼容，否则采集会失败。安装 `media-ctl` 后，可根据当前内核公开的 entity
名称配置 topology。实体名称可能随内核版本变化，因此应先执行：

```bash
media-ctl -p -d platform:vimc
```

再参考 Linux 内核 vimc 文档配置 sensor、debayer、scaler 和 capture 格式，而不要
将某个版本的 entity ID 硬编码进应用程序。

## 8. 清理测试环境

显式卸载模块：

```bash
sudo ./tools/virtual_camera_modules.sh unload
```

如果有程序仍打开虚拟设备，内核会拒绝卸载并返回 busy。应先结束使用这些节点的
测试程序，不要强制杀死不属于本项目的进程。

## 9. 当前工作区的检查结果

当前 x86_64 主机内核为 6.8，并已经安装：

```text
/lib/modules/<kernel>/kernel/drivers/media/test-drivers/vivid/vivid.ko
/lib/modules/<kernel>/kernel/drivers/media/test-drivers/vimc/vimc.ko
```

但是当前会话没有 `CAP_SYS_MODULE`，实际 `modprobe vimc` 返回
`Operation not permitted`，所以无法在此会话内创建 `/dev/video*` 并完成真实 ioctl
测试。应在宿主机终端运行第 4 节命令。

## 10. 参考资料

- [Linux Kernel: Virtual Video Test Driver (vivid)](https://docs.kernel.org/admin-guide/media/vivid.html)
- [Linux Kernel: Virtual Media Controller Driver (vimc)](https://docs.kernel.org/admin-guide/media/vimc.html)
