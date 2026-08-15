# RK3568 摄像头到 DSI 屏幕：硬件拓扑、软件模型与实现方案

## 1. 先给出最重要的结论

本板的推荐显示链路不是一条简单的“ISP 直连 RGA，RGA 再直连 VOP”的硬件流水线。
更准确的描述是：

```text
       板级外部器件/真实走线                         RK3568 SoC 内部硬件

 光线
  |
  v
+--------------------+  SDA/SCL（I2C控制，不传整帧）  +------------------+
| 摄像头 Sensor 芯片  |<---------------------------->| I2C 控制器       |
| Bayer RAW + MIPI TX |                               +------------------+
+---------+----------+
          |  MIPI CSI-2 over D-PHY
          |  clock/data差分lane，PCB/FPC真实走线
          v
          +==========================================>+------------------+
                                                      | CSI D-PHY RX     |
                                                      +--------+---------+
                                                               | CSI-2字节/像素流
                                                      +--------v---------+
                                                      | CSI接收器 + ISP  |
                                                      | RAW去马赛克/3A等 |
                                                      +--------+---------+
                                                               | DMA写
                          +------------------------------------v---------+
                          | 外部DDR芯片中的V4L2 NV12 capture buffer     |
                          +------------------------------------+---------+
                                                               | RGA DMA读
                                                      +--------v---------+
                                                      | RGA 2D加速器     |
                                                      | 旋转/缩放/YUV->RGB|
                                                      +--------+---------+
                                                               | DMA写
                          +------------------------------------v---------+
                          | 外部DDR芯片中的DRM XRGB display buffer      |
                          +------------------------------------+---------+
                                                               | VOP DMA读
                                                      +--------v---------+
                                                      | VOP2             |
                                                      | 扫描/合成/时序   |
                                                      +--------+---------+
                                                               |
                                                      +--------v---------+
                                                      | DSI Host         |
                                                      | DSI协议封包      |
                                                      +--------+---------+
                                                               |
                                                      +--------v---------+
                                                      | DSI D-PHY TX     |
                                                      | 电气串行化       |
                                                      +--------+---------+
                                                               |
          MIPI DSI over D-PHY，差分lane真实走线                 |
          +<====================================================+
          |
+---------v-------------+
| LCD模组               |
| DSI RX + TCON/面板像素 |
+-----------------------+
```

需要牢牢记住四件事：

1. **ISP、RGA、VOP、CSI/DSI D-PHY 都是 RK3568 芯片内部真实存在的硬件 IP。**
   Linux 驱动只是配置和调度这些硬件。
2. **`/dev/video0`、`rkisp-isp-subdev`、DRM plane、DMA-BUF fd 都是软件对象，**
   不是独立芯片，也不是 PCB 上的插座或导线。
3. **Sensor→SoC 和 SoC→LCD 是真实的高速差分物理连线；ISP→RGA→VOP 通常不是
   板上专用连线，而是各硬件通过 SoC NoC/AXI 和 DDR DMA 读写帧缓冲。**
4. **RAW Bayer→完整彩色图像由 ISP 的去马赛克等流水线完成；RGA 不做 ISP。**
   本方案中 RGA 接收的已经是 ISP 输出的 NV12，只负责旋转、缩放和 NV12→RGB。

本文既描述开发板上已验证的事实，也给出停止 Weston 后的专用摄像头显示程序方案。
凡是没有足够证据确认的板级型号，都会明确标成“待原理图/BOM 确认”。

---

## 2. 先分清五个层次

很多困惑来自把“硬件模块、协议、内核对象、设备文件和 C++ 类”放在同一张图中。
本节先定义它们的边界。

### 2.1 板上真实存在的物理器件

这些东西可以在原理图、BOM 或 PCB 上找到：

| 物理对象 | 在哪里 | 作用 |
| --- | --- | --- |
| RK3568 SoC 封装 | 开发板 PCB | 内含 CPU、CSI、ISP、RGA、VOP、DSI 等硬件 IP |
| 摄像头 Sensor/模组 | SoC 外部的外设 | 光电转换，输出 Bayer RAW；内含 MIPI CSI 发射端 |
| LCD 模组 | SoC 外部的外设 | 接收 DSI 数据并驱动液晶像素；通常含 DSI 接收端和面板控制器 |
| DDR 芯片 | SoC 外部的板级存储器 | 保存相机帧、RGA 结果和 DRM framebuffer |
| 电源、晶振、复位、背光电路 | 板级器件 | 给各器件供电、提供时钟/复位和背光控制 |
| FPC/连接器/PCB 走线 | 板级物理连线 | 承载 CSI、DSI、I2C、GPIO、PWM、电源等信号 |

严格说 DDR 是 SoC 外部物理器件，但通常不称为摄像头“外设”；它是整个系统共享的
主存。Sensor 和 LCD 才是本链路最主要的外设。

### 2.2 RK3568 内部真实存在的硬件 IP

下面这些不是 C++ 类，也不是驱动虚构的概念，而是硅片内部的数字/模拟电路：

| SoC 内部硬件 | 本板探测证据 | 主要职责 |
| --- | --- | --- |
| I2C 控制器 | Sensor 位于 `fe5d0000.i2c` 的总线 4、地址 `0x1a` | 写 Sensor 寄存器、曝光、增益和模式 |
| CSI-2 D-PHY RX | `fe870000.csi2-dphy-hw`，`rockchip,rk3568-csi2-dphy-hw` | 接收 CSI 差分电气信号、恢复时钟/数据 |
| CSI-2 接收逻辑 | media graph 中的 CSI 接收部分 | 解包 CSI-2 数据类型、虚拟通道和像素流 |
| ISP | `fdff0000.rkisp`，`rockchip,rk3568-rkisp` | Bayer RAW 图像处理和格式输出 |
| DMA/IOMMU/NoC/DDR 控制器 | SoC 基础设施 | 在硬件模块与 DDR 之间搬运和寻址数据 |
| RGA2 | `fdeb0000.rk_rga`，驱动 `rga2` | 2D blit、旋转、缩放和颜色格式转换 |
| VOP2 | `fe040000.vop`，驱动 `rockchip-vop2` | 从内存扫描 framebuffer、图层合成、生成显示时序 |
| DSI Host | `fe070000.dsi`，驱动 `dw-mipi-dsi` | 把像素/命令组织成 MIPI DSI 协议包 |
| DSI D-PHY TX | 与 DSI Host 配套的 SoC PHY | 将 DSI 数据变成高速差分电气信号送到引脚 |

`display-subsystem` 是设备树和 DRM 驱动用于聚合显示组件的逻辑节点，不能把它当成
另一块独立显示芯片。

### 2.3 板上的物理线路和传输协议

“线”和“协议”也不是一回事：线是铜走线/引脚，协议是线上的信号规则。

| 用途 | 物理连接 | 电气/链路层 | 上层含义 |
| --- | --- | --- | --- |
| 配置 Sensor | 两根主要信号线 SDA/SCL，加电源/地 | I2C | 寄存器读写，不承载整帧像素 |
| Sensor 图像输入 | CSI clock/data 差分 lane，经 FPC/PCB | MIPI D-PHY RX | MIPI CSI-2 数据包，承载 RAW 像素和帧边界 |
| SoC 内部访存 | 芯片内部互连，不是板外线 | AXI/NoC、DMA、IOMMU | ISP/RGA/VOP 访问 DDR |
| LCD 图像输出 | DSI clock/data 差分 lane，经 PCB/FPC | MIPI D-PHY TX | MIPI DSI 视频/命令包 |
| LCD 辅助控制 | GPIO、PWM、可能还有 I2C/SPI | 各自协议 | 复位、使能、背光亮度等 |

CSI-2 和 DSI 可以使用相同家族的 D-PHY 电气层，但它们是不同的上层协议：

- CSI-2 用于摄像头到主机，方向通常是 Sensor → SoC。
- DSI 用于主机到显示面板，方向通常是 SoC → Panel。
- 两条链路各有自己的 PHY 端点，不能把摄像头 CSI lane 接到显示 DSI lane。

### 2.4 Linux 内核创建的软件对象

Linux 用软件对象描述、配置和暴露硬件：

| 软件对象 | 是什么 | 不是什么 |
| --- | --- | --- |
| platform device/driver | 设备树节点与驱动的绑定 | 不是新增的物理芯片 |
| media entity | 内核媒体框架中的功能实体 | 不保证“一实体等于一芯片” |
| media pad/link | 数据入口、出口及可选路由的抽象 | 不是 PCB 焊盘、铜线或 FPC lane |
| V4L2 subdev | Sensor、CSI、ISP 子模块的配置接口 | 不是用户直接读取整帧的文件 |
| `/dev/video0` | ISP mainpath 的字符设备和 DMA 队列入口 | 不是摄像头插座，也不是 ISP 本体 |
| `/dev/rga` | 向 RGA 驱动提交作业的字符设备 | 不是帧数据永久存放处 |
| `/dev/dri/card0` | Rockchip DRM/KMS 显示设备接口 | 不是显卡插槽，也不是 framebuffer 本身 |
| DRM connector/CRTC/plane/FB | 内核显示对象模型 | 不一定与物理器件一一对应 |
| DMA-BUF fd | 一个共享内存对象的进程文件描述符 | 不是物理导线，也不是传输协议 |

Media Controller 图是**Linux 对媒体数据路由的模型**。它尽量反映硬件流水线，但其中
的 `rkisp-csi-subdev` 和 `rkisp-isp-subdev` 可以是同一 SoC/同一驱动内部的两个逻辑
功能块；图上的箭头表示启用的数据路由，不代表两块独立 IC 之间一定有 PCB 连线。

### 2.5 用户态库和本项目 C++ 对象

这些完全属于软件：

- `libdrm`：把 DRM/KMS ioctl 封装成较易使用的 API。
- `librga`/im2d：构造 RGA 作业并交给内核驱动。
- V4L2 ioctl：配置 ISP 输出、申请/排队/取回相机缓冲区。
- `rkaiq_3A_server`：根据统计数据计算自动曝光、自动白平衡等参数并配置 ISP/Sensor。
- 将来本项目中的 `CameraCapture`、`RgaBlitter`、`DrmDisplay`、`FrameSlot` 等 C++ 类：
  它们只是资源管理和状态机封装，名字与硬件类似也不意味着创建了硬件。

---

## 3. 已验证的真实板级/驱动拓扑

### 3.1 Sensor 身份存在一处必须保留的疑点

板上 media entity 名称为：

```text
m00_b_imx415 4-001a-1
```

但同一 I2C 设备的设备树 `compatible` 为：

```text
sony,imx335
```

因此，软件名称只能证明当前 BSP 如何命名和绑定该节点，**不能单凭它断定 PCB 上焊接的
硅片一定是 IMX415 或 IMX335**。可能存在驱动复用、设备树沿用或厂商命名不一致。
最终型号应查看开发板原理图、BOM、模组标签或 Sensor 芯片丝印。本文后续统一称其为
“Sensor”，并把 IMX415/IMX335 写作待硬件资料确认项。

已确定的事实是：

- Sensor 是 RK3568 外部的物理器件。
- 它连接在 SoC `fe5d0000.i2c` 控制器产生的 I2C bus 4 上，地址为 `0x1a`。
- 当前媒体总线格式为 `SGBRG10_1X10 3864x2192 @ 30 FPS`。
- 这表示输入是 10-bit Bayer RAW，Bayer 排列为 GBRG；它不是 RGB888 图像。

### 3.2 Linux media graph

开发板当前启用的路由为：

```text
[外部物理 Sensor]
  media entity: m00_b_imx415 4-001a-1
  SGBRG10_1X10 3864x2192 @ 30 FPS
        |
        | 板级真实连线：MIPI CSI-2 over D-PHY
        v
[SoC 内部 CSI D-PHY/接收器]
  media entity: rockchip-csi2-dphy0
  char node: /dev/v4l-subdev2
        |
        | SoC内部硬件数据路由；media link 是其软件模型
        v
[SoC 内部 CSI/ISP 输入逻辑]
  media entity: rkisp-csi-subdev
  char node: /dev/v4l-subdev1
        |
        v
[SoC 内部 ISP]
  media entity: rkisp-isp-subdev
  char node: /dev/v4l-subdev0
  crop 3864x2192 -> 3840x2160
        |
        +----> rkisp_mainpath -> /dev/video0
        |
        +----> rkisp_selfpath -> /dev/video1
```

这里 `/dev/v4l-subdev*` 和 `/dev/video*` 是驱动创建的字符设备。真正的图像处理发生在
Sensor 和 SoC 硬件中，字符设备只让用户程序发 ioctl、排 DMA buffer 和取得完成事件。

其他视频节点：

| 节点 | 软件接口所对应的功能 |
| --- | --- |
| `/dev/video0` | ISP mainpath，本文使用的处理后输出 |
| `/dev/video1` | ISP selfpath，另一路处理后输出 |
| `/dev/video2`～`/dev/video4` | RAW writer，输出尚未经过完整 ISP 的 Bayer 数据 |
| `/dev/video5`、`/dev/video6` | RAW reader，从内存向 ISP 回灌 RAW |
| `/dev/video7` | ISP statistics，供 3A 算法读取统计量 |
| `/dev/video8` | ISP parameters，把调参结果提交给 ISP |
| `/dev/video-camera0` | 指向 `/dev/video0` 的符号链接，不是另一个硬件设备 |

### 3.3 已验证的相机输出

本项目已在 `/dev/video0` 实测：

```text
1920x1080 NV12，约 30 FPS
V4L2 multi-planar API
memory plane count = 1
stride              = 1920 bytes
sizeimage           = 3110400 bytes
4 个 capture buffer 均可导出 DMA-BUF fd
连续 100 帧：0 error，0 timeout，0 sequence gap
```

“memory plane 为 1”不代表 NV12 只有 Y：

```text
同一个 DMA-BUF / DDR 内存对象

offset 0                         Y，1920 x 1080 bytes
offset 1920 * 1080 = 2073600    UV 交错，1920 x 540 bytes
有效总长度                       3110400 bytes
```

驱动映射长度可能因 1088 行对齐而成为 3133440，但有效 UV offset 仍应使用驱动返回并
经实测确认的图像布局，不能拿 allocation length 猜测色度平面起点。

当前颜色元数据：

```text
colorspace        = sRGB
transfer          = Rec.709
YCbCr encoding    = Rec.709
quantization      = Full Range
```

### 3.4 已验证的显示输出

```text
/dev/dri/card0       Rockchip display-subsystem
DSI-1                connected, enabled
panel mode           1080x1920 @ 约 54.44 Hz
pixel clock          121 MHz
DSI bus format       RGB888_1X24
primary plane        Smart0-win0，支持 LINEAR XRGB8888
overlay plane        Esmart0-win0，支持 LINEAR NV12 和 XRGB8888
```

完整时序：

```text
H: 1080, 1102, 1122, 1144
V: 1920, 1929, 1936, 1943
flags: -hsync, -vsync
```

`/dev/dri/card1` 是 RKNPU，不是显示 KMS card。connector、CRTC、plane 的数字 ID 会随
启动和驱动注册顺序变化，程序必须运行时枚举，不能硬编码曾观察到的 163/115。

面板设备树只报告 `simple-panel-dsi`，未报告可靠的商品型号和物理尺寸；具体 LCD 模组
仍需查原理图/BOM。

---

## 4. 一帧图像到底经历了什么

### 4.1 Sensor：光线变成 Bayer RAW

Sensor 上每个感光像素通常只测量红、绿、蓝中的一种颜色，按 Bayer 阵列排列。
当前 `SGBRG10` 中：

- `GBRG` 是色彩滤镜阵列顺序。
- `10` 表示每个采样具有 10-bit 有效精度。
- RAW 数据还不是每个像素都含 R、G、B 三通道的完整彩图。

Sensor 内部可能做黑电平或坏点等有限处理，但面向 SoC 输出的仍属于 Bayer RAW。
曝光时间、模拟/数字增益、帧率和输出模式由 SoC 通过独立的 I2C 控制线配置。

### 4.2 CSI D-PHY：它像 Ethernet PHY 吗

**只在“负责物理电气层”这一点上相似；它绝不是 Ethernet switch。**

| 比较项 | Ethernet PHY | MIPI D-PHY |
| --- | --- | --- |
| 核心角色 | 数字 MAC 与线缆电气信号之间转换 | CSI/DSI 控制器与差分 lane 电气信号之间转换 |
| 是否处理物理电气层 | 是 | 是 |
| 是否等同上层协议 | 否，Ethernet MAC 在其上方 | 否，CSI-2/DSI 协议控制器在其上方 |
| 是否交换/路由多个端口 | PHY 本身通常不做；switch 才做 | 不做，D-PHY 不是 switch |
| 是否学习地址/转发网络帧 | 否 | 否 |
| 常见实现 | 可为外置 PHY 芯片 | RK3568 端是 SoC 内部集成硬件 IP |

摄像头方向有两个物理端点：

```text
Sensor 内部 D-PHY TX === 差分 lane/FPC/PCB === RK3568 内部 D-PHY RX
```

D-PHY 负责高速/低功耗状态、电气驱动/接收、时钟与数据恢复、lane 级传输。CSI-2
接收器才理解 CSI-2 包头、数据类型、虚拟通道、帧开始/结束和 RAW10 打包。它们可以
集成得很紧，但职责仍然不同。

显示方向恰好相反：

```text
RK3568 D-PHY TX === 差分 lane/FPC/PCB === LCD 模组 D-PHY RX
```

### 4.3 ISP：RAW 到彩色图像的核心硬件

本板 ISP 对应真实 SoC 硬件 `fdff0000.rkisp`。驱动 `rkisp_hw/rkisp_v5` 通过寄存器、
中断和 DMA 控制它。因此回答是：**这里的 ISP 确实是硬件 ISP，不是 CPU 上的软件
滤镜，也不是 RGA 或 GPU 冒充的 ISP。**

典型 ISP 流水线包括：

```text
Bayer RAW
  -> 黑电平/坏点/镜头阴影等校正
  -> 去马赛克 demosaic：每个像素重建 R、G、B
  -> 白平衡、颜色校正矩阵
  -> 降噪、锐化、Gamma/色调等
  -> RGB/YUV 颜色空间转换
  -> 缩放/裁剪和 mainpath 输出
  -> NV12 DMA 写入 DDR
```

具体启用了哪些模块和参数由 Rockchip IQ 文件、3A 结果及驱动配置决定。不要把上表理解
成每个算法在当前 IQ 配置中一定开启。

对“RAW 到 RGB 是 ISP 还是 RGA 做的”应精确回答：

- **Bayer RAW 去马赛克为完整 RGB，是 ISP 的职责。**
- ISP 随后可以再把内部 RGB 域结果转换为 YUV/NV12，当前 `/dev/video0` 就输出 NV12。
- RGA 收到的是已经完成去马赛克和主要图像质量处理的 NV12，不再接触 Bayer RAW。
- RGA 的 NV12→XRGB8888 是普通 YUV→RGB 色彩转换，不是 RAW 去马赛克，也不是 ISP。
- GPU 在本目标方案中完全不需要参与。

### 4.4 ISP 输出以后：从流式线路变为内存交换

Sensor 到 ISP 之间是按像素/行/帧连续到达的硬件流。ISP mainpath 输出时由 DMA 把
结果写入 DDR 中的 V4L2 buffer。从此以后：

```text
ISP --DMA write--> DDR capture buffer
RGA --DMA read---> DDR capture buffer
RGA --DMA write--> DDR display buffer
VOP --DMA read---> DDR display buffer
```

这些 DMA 请求通过 RK3568 内部 NoC/AXI、IOMMU 和 DDR 控制器到达外部 DDR 芯片。
所以在本方案中：

- ISP 与 RGA 不是用一组板级像素线直连。
- RGA 与 VOP 也不是用一组板级像素线直连。
- 它们共享的是 DDR 中的内存对象和同步所有权。
- DMA-BUF fd 只是 Linux 用来让不同驱动引用同一块内存的句柄，不承载像素数据本身。
- “零拷贝”表示 CPU 不做 `memcpy`，并不表示硬件从不读写 DDR。

### 4.5 RGA：真实的 2D 硬件，不是 GPU，也不是 ISP

RGA 对应真实硬件 `fdeb0000.rk_rga`，本板有 `/dev/rga`、RGA2 内核驱动和 librga
2.1.0。它擅长规则的二维像素操作：

- 旋转 90°/180°/270°、镜像。
- 缩放、裁剪和矩形搬运。
- NV12/YUV 与 RGB 像素格式转换。
- alpha blend、填充等 2D 操作。

它不负责：

- Bayer demosaic。
- 自动曝光/自动白平衡计算。
- 镜头 IQ 调校和完整 ISP 流水线。
- 3D 图形、shader 或 Qt Quick 场景渲染。

RGA 作业的真实行为是“从一个或多个 DDR buffer DMA 读取，计算后 DMA 写到另一个
DDR buffer”。在首版中：

```text
输入：1920x1080 NV12，来自 V4L2 DMA-BUF
操作：旋转 270° + Rec.709 full-range YUV->RGB
输出：1080x1920 XRGB8888，写入 DRM buffer
```

旋转 90° 还是 270° 必须以实拍标定图确认。厂商桌面相机的代码路径曾固定旋转 270°，
这是首选初值，但不能代替最终安装方向验证。

### 4.6 VOP：扫描显示和合成硬件

VOP2 对应真实硬件 `fe040000.vop`。其主要工作是：

1. 按屏幕刷新节奏从 DDR framebuffer 读取像素。
2. 对一个或多个硬件 plane 做裁剪、缩放、alpha、可能的颜色空间转换和合成。
3. 生成 1080x1920 模式所需的 active、blanking、hsync/vsync 等显示时序。
4. 把最终像素流送给 DSI Host。

DRM 中的概念映射如下：

| DRM 软件对象 | 大致表示 |
| --- | --- |
| framebuffer | 对内存像素布局的描述，指向 GEM/内存对象 |
| plane | 一个可由 VOP window 扫描/合成的图层 |
| CRTC | 一条显示扫描和时序流水线 |
| encoder | 从 CRTC 到输出接口的编码/路由抽象 |
| connector `DSI-1` | 与面板连接的逻辑输出端 |

它们是软件 API 模型，不意味着 PCB 上有名为 CRTC 或 plane 的独立芯片。

### 4.7 DSI Host、DSI D-PHY 和 LCD 各做什么

三者职责必须分开：

```text
VOP 最终 RGB 像素/时序
       |
       v
DSI Host：形成 DSI 视频包、命令包、lane 分配和链路时序
       |
       v
DSI D-PHY TX：把数字 lane 数据转换成 SoC 引脚上的高速差分信号
       |
       v
PCB/FPC 物理差分线
       |
       v
LCD D-PHY RX + DSI 接收器：恢复并解析数据
       |
       v
面板控制/TCON/源极和栅极驱动：真正刷新液晶像素
```

DSI 本身不做 Bayer demosaic，不负责相机旋转，也不是通用图像处理器。面板接收到的
已经是最终显示方向、最终尺寸的 RGB888 像素流。背光通常另由 PWM/GPIO/背光驱动
控制，它只决定亮度，不承载图像像素。

---

## 5. 硬件、协议和软件对象的逐项对照表

| 名称 | 分类 | 是否物理存在 | 位于哪里 | 数据如何经过 |
| --- | --- | --- | --- | --- |
| Sensor | 外部物理外设 | 是 | 摄像头模组/PCB | 光→Bayer RAW；CSI 发出 |
| I2C bus 4 addr 0x1a | 物理总线+协议地址 | 线路和控制器是；地址是协议概念 | Sensor 与 RK3568 之间 | 少量寄存器控制数据 |
| CSI-2 | 数据传输协议 | 否，它是规则 | Sensor 到 SoC | RAW10 数据包 |
| CSI D-PHY RX | SoC 内部硬件 IP | 是 | RK3568 内 | 接收差分 lane |
| `rockchip-csi2-dphy0` | media entity | 否，软件对象 | Linux 内核 | 表示/配置相应硬件功能 |
| ISP | SoC 内部硬件 IP | 是 | RK3568 内 | RAW→彩色处理→NV12 |
| `rkisp-isp-subdev` | V4L2 subdev/entity | 否，软件对象 | Linux 内核 | 配置 ISP pad、格式和裁剪 |
| `/dev/video0` | 字符设备/API 端点 | 否，软件对象 | Linux `/dev` | V4L2 mainpath DMA buffer 队列 |
| V4L2 buffer | 内核/用户态共享描述 | 否 | 软件元数据 | 指向 DDR 中的物理页/映射 |
| DDR buffer | 外部存储器中的数据区域 | 承载它的 DDR 是物理硬件 | 板上 DDR | 保存整帧像素 |
| DMA-BUF fd | Linux 共享句柄 | 否 | 进程 fd 表/内核 | 让 V4L2、RGA/DRM 引用共享内存 |
| RGA | SoC 内部硬件 IP | 是 | RK3568 内 | DDR读→旋转/CSC→DDR写 |
| `/dev/rga`/librga | 驱动接口/用户库 | 否 | 内核和用户态 | 向 RGA 提交作业 |
| VOP | SoC 内部硬件 IP | 是 | RK3568 内 | 从 DDR 扫描、合成、出时序 |
| DRM plane/CRTC | 内核显示对象 | 否 | DRM/KMS | 配置 VOP 的 window/显示路径 |
| `/dev/dri/card0` | DRM 字符设备 | 否 | Linux `/dev` | KMS ioctl 入口 |
| DSI Host | SoC 内部硬件 IP | 是 | RK3568 内 | DSI 协议封包 |
| DSI | 数据传输协议 | 否，它是规则 | SoC 到 LCD | RGB 视频/命令包 |
| DSI D-PHY TX | SoC 内部硬件 IP | 是 | RK3568 内 | 高速差分发送 |
| LCD panel | 外部物理外设 | 是 | 显示模组 | DSI解码并显示像素 |
| Weston | 用户态合成器 | 否 | Linux 进程 | 占有 DRM master、合成桌面 |
| Qt/GStreamer | 用户态框架 | 否 | Linux 库/进程 | 厂商桌面相机的高级软件路径 |
| Mali GPU | SoC 内部硬件 IP | 是，但目标方案不用 | RK3568 内 | 厂商 Qt Quick 合成可能使用 |

---

## 6. 厂商桌面相机为何显示成竖屏

对目标板现有桌面相机的只读分析表明，它大致走的是：

```text
/dev/video0 NV12 1280x720
  -> Qt Multimedia / GStreamer camerabin
  -> videoconvert（环境允许使用 RGA）
  -> QVideoFrame RGB32/BGR32
  -> QImage
  -> QTransform 固定旋转 270°，SmoothTransformation
  -> Qt Quick 将 QImage 建成纹理
  -> Mali/Wayland/Weston 合成
  -> Weston XR24 720x1280 framebuffer
  -> VOP 缩放到 1080x1920
  -> DSI panel
```

关键证据是厂商程序的 `CameraItem::updateImage` 路径存在 `QTransform` 270° 和
`QImage::transformed(..., SmoothTransformation)`；运行时又打开 `/dev/mali0` 和
`/dev/dri/card0`。因此原桌面效果不是简单的 Sensor 或 DSI 自动竖屏：

- 相机原始方向由用户态相机程序旋转。
- Qt Quick/Wayland/Weston/GPU 参与最终桌面合成。
- 当前 DRM primary plane 的 rotation 仍为 0；也就是说观察到的竖屏不是 VOP plane
  旋转产生的。
- GStreamer 的颜色转换可能使用 RGA，但固定 270° 的 QImage 变换仍属于软件图像路径。

这能解释现象，但不是专供摄像头服务时最简洁、可控的架构。

---

## 7. 停止 Weston 后的目标架构

### 7.1 首版推荐链路

```text
Sensor RAW10
  -> CSI-2/D-PHY RX
  -> 硬件 ISP + rkaiq 3A
  -> /dev/video0: 1920x1080 NV12，4个capture DMA buffer
  -> 硬件 RGA: 270°旋转 + NV12->XRGB8888
  -> 3个1080x1920 XRGB8888 DRM display buffer
  -> VOP Smart0-win0 primary plane
  -> DSI Host + D-PHY TX
  -> 1080x1920 panel
```

首版明确不使用：Qt、GStreamer、Wayland、Weston、Mali GPU、CPU 逐像素转换。

选择 XRGB8888 作为首版显示格式的理由：

- 当前 primary plane 已验证支持 LINEAR XRGB8888。
- RGB 显示可避开初版 VOP YUV colorspace/range 属性配置的不确定性。
- RGA 能在同一作业中完成旋转和 NV12→RGB。
- 内存带宽增加但调试路径直观，适合作为正确性基线。

注意这里存在两个含义不同的颜色转换：

1. ISP 内部完成 Bayer RAW 去马赛克，并为 mainpath 生成 NV12。
2. RGA 为显示把 NV12 转回 XRGB8888。

第二步不是重复做 ISP；它只是为了让 VOP primary plane 扫描一个易验证的 RGB
framebuffer。性能稳定后可尝试“RGA 只旋转并输出 NV12 → VOP overlay plane 做
YUV→RGB”，减少显示缓冲带宽，但必须先通过 DRM atomic `TEST_ONLY` 和颜色标定验证。

### 7.2 为什么不用 GPU

旋转、缩放和 YUV/RGB CSC 都是 RGA 的固定功能任务。GPU 会引入 EGL/GBM、shader、
纹理导入和显式/隐式 fence 等额外复杂度。除非以后需要复杂 UI、透视、shader 或多层
特效，否则 GPU 没有必要。

### 7.3 为什么不用 DRM plane 直接旋转

板上已知情况：

- `Esmart0-win0` 能扫描 LINEAR NV12，但未广告 90°/270° rotation。
- 某些 Cluster plane 广告 rotation，但格式 modifier 约束倾向 AFBC，不能假定接受当前
  LINEAR NV12 DMA-BUF。

只有把真实 framebuffer、format、modifier、rotation、src/dst rectangle 交给 atomic
`TEST_ONLY` 成功，才能证明某个 plane 可直接完成该组合。首版使用 RGA 更稳妥。

---

## 8. 软件模块设计

建议最终新增独立可执行程序，例如 `camera_kms_display`，内部模块如下：

```text
CameraKmsApp
  +-- CameraCapture       V4L2格式、buffer、STREAMON/DQBUF/QBUF
  +-- CameraMetadata      stride/offset/colorspace/sequence/timestamp
  +-- RgaTransform        DMA-BUF导入、旋转、CSC、作业同步
  +-- DrmDisplay          connector/CRTC/plane/mode/FB/page-flip
  +-- FrameScheduler      capture与display buffer所有权状态机
  +-- SignalHandler       SIGINT/SIGTERM安全退出
```

### 8.1 `CameraCapture`

职责：

1. 打开 `/dev/video0`。
2. 用 `VIDIOC_QUERYCAP` 确认 `VIDEO_CAPTURE_MPLANE` 和 streaming。
3. `VIDIOC_S_FMT` 请求 1920x1080 NV12，再 `VIDIOC_G_FMT` 接受驱动返回的实际参数。
4. 保存每个 memory plane 的 `bytesperline`、`sizeimage`；独立保存 NV12 image plane
   的 offset。
5. `VIDIOC_REQBUFS` 申请 4 个 MMAP capture buffers。
6. 对每个 plane 调 `VIDIOC_EXPBUF` 得到 DMA-BUF fd。
7. 全部 `QBUF` 后 `STREAMON`。
8. 用 `poll` + `DQBUF` 取得完整帧，校验 `bytesused`、error flag、sequence 和 timestamp。

不要把“V4L2 multi-planar API”和“NV12 有两个 image plane”混为一谈。

### 8.2 `DrmDisplay`

职责：

1. 打开 `/dev/dri/card0`，确认获得 DRM master。
2. 枚举并选择状态为 connected 的 `DSI-1`。
3. 选择面板报告的 1080x1920 mode，而不是在代码中伪造时序。
4. 从 connector 找 encoder/possible CRTCs，再从 CRTC 找兼容 plane。
5. 选择支持 LINEAR XRGB8888 的 primary plane。
6. 创建 3 个 1080x1920、32-bpp DRM dumb/GEM buffer。
7. 使用 `drmModeAddFB2` 创建 XRGB8888 framebuffer。
8. 把 GEM handle 用 PRIME 导出为 DMA-BUF fd，供 RGA 写入。
9. 初次 modeset，之后 page flip；必须消费 DRM event。

首版可采用 legacy modeset/page flip，因为 BSP 的 Weston 环境曾设置
`WESTON_DISABLE_ATOMIC=1`。并行开发一个 atomic `TEST_ONLY` 探针，验证无误后再切换
到完整 atomic commit。

### 8.3 `RgaTransform`

每帧构造源/目标描述：

```text
source:
  fd          = capture DMA-BUF fd
  format      = RK_FORMAT_YCbCr_420_SP / 与 NV12 匹配的枚举
  width       = 1920
  height      = 1080
  wstride     = 1920
  hstride     = 1080 或由驱动/导入要求验证后的值
  colorspace  = Rec.709 full range

destination:
  fd          = DRM buffer PRIME fd
  format      = XRGB8888 对应的 RGA 格式
  width       = 1080
  height      = 1920
  stride      = DRM create-dumb 返回的 pitch

operation:
  rotation    = 270 degrees（以实拍验证后固化）
  conversion  = NV12 -> XRGB8888
```

必须先调用 `imcheck`/等效校验，再提交同步 RGA 作业。首版等待 RGA 完成后才 page flip，
这样最容易保证 VOP 不会扫描仍在写入的 buffer。之后若改成异步作业，必须加入 fence，
不能只靠调用顺序猜同步。

### 8.4 buffer 状态机

capture buffer：

```text
QUEUED_TO_CAMERA
    -> CAPTURED_BY_DQBUF
    -> READING_BY_RGA
    -> RGA_DONE
    -> QUEUED_TO_CAMERA
```

display buffer：

```text
FREE
    -> WRITING_BY_RGA
    -> READY_FOR_FLIP
    -> SCANNING_OUT
    -> page-flip event后旧buffer变FREE
```

禁止的行为：

- RGA 尚未读完 capture buffer 就 `QBUF` 给摄像头覆盖。
- VOP 正在扫描 display buffer 时让 RGA 重写它。
- page flip 提交后不处理 event，导致状态永远无法回收。
- 把 DMA-BUF fd 当成永远有效的全局编号；fd 生命周期必须由 RAII 管理。

推荐 4 个 capture buffer + 3 个 display buffer。首版同步 RGA 可牺牲一点流水并行度换取
正确性；稳定后再引入 fence 和异步流水。

### 8.5 主循环伪代码

```cpp
initialize_drm();
allocate_three_display_buffers();
export_display_buffers_as_dmabuf();

initialize_v4l2();
allocate_and_export_four_capture_buffers();
queue_all_capture_buffers();
stream_on();

while (!stop_requested) {
    CaptureFrame frame = camera.dequeue_with_timeout();
    DisplayBuffer &dst = display.acquire_free_buffer();

    rga.rotate_and_convert_sync(
        frame.dmabuf_fd,
        frame.layout,
        dst.dmabuf_fd,
        dst.pitch,
        Rotation::Deg270,
        ColorSpace::BT709Full);

    display.page_flip(dst.framebuffer_id);
    display.wait_and_handle_flip_event();

    camera.requeue(frame.index);
}

stream_off();
restore_or_disable_crtc();
```

真正实现时可以在 RGA 完成后立即 requeue capture buffer，不必等待 page flip；但 display
buffer 要到旧画面的 flip-complete event 后才能复用。

---

## 9. 服务切换与设备独占

Weston 当前是 `/dev/dri/card0` 的 DRM master。专用程序启动顺序：

```text
1. 停止发起桌面UI的 systemui/相机等应用
2. 停止 Weston
3. 确认没有其他进程持有 /dev/dri/card0 或相机节点
4. 保持 rkaiq_3A_server 运行
5. 启动 camera_kms_display
```

本 BSP 相关脚本包括：

- `/etc/init.d/S49weston`
- `/etc/init.d/S50systemui`
- `/etc/init.d/S40rkaiq_3A`

停止桌面时不要误停 `S40rkaiq_3A`。它不是显示服务，而是相机图像质量控制链的一部分。
如果其进程与 `/dev/video0` 有资源冲突，应先确认它只使用 statistics/parameters/subdev，
再决定具体调整，不能直接假定 3A 可以删除。

退出顺序：

```text
停止接收新帧
-> 等待在途RGA作业完成
-> 等待/取消在途page flip
-> V4L2 STREAMOFF
-> 关闭DMA-BUF/GEM/FB
-> 释放DRM master
-> 按需重启Weston/systemui
```

---

## 10. 分阶段验证计划

### 阶段 A：硬件身份和板级资料闭环

- 从原理图/BOM/丝印确认 Sensor 究竟是 IMX415 还是 IMX335。
- 确认 CSI lane 数、lane mapping、时钟、复位、供电和 I2C 地址。
- 确认 LCD 模组型号、DSI lane 数、video/command mode、reset/backlight 连接。
- 保存设备树相应节点，形成“原理图网络名 ↔ DT endpoint ↔ media entity”对照表。

### 阶段 B：相机输入基线

- 固定 1920x1080 NV12，连续采集至少 10 分钟。
- 统计 FPS、sequence gaps、timeouts、error flag。
- 每隔一定帧保存原始 NV12，用 ffmpeg/自制工具检查颜色和画面方向。
- 改变亮度场景，确认 AE/AWB 仍正常，证明 3A 服务有效。

### 阶段 C：DRM 独占和纯色测试

- 停止 Weston 后取得 DRM master。
- 用 XRGB8888 dumb buffer 显示红、绿、蓝、灰和彩条。
- 验证 1080x1920 mode、pitch、RGB 通道顺序、边界和背光。
- 验证双/三缓冲 page flip 无撕裂、无 event 堆积。

### 阶段 D：RGA 离线测试

- 给 RGA 输入带文字和方向箭头的已知 NV12 测试图。
- 分别测试 90°/270°，确认目标内存为 1080x1920。
- 检查 stride、UV offset、颜色范围、红蓝通道和裁剪边缘。
- 用 CPU 参考转换仅做离线比对，不进入实时路径。

当前小阶段已用 BT.709 limited NV12 色条完成 270° 旋转、XRGB8888 转换和
DRM 显示的实板验证，单次 RGA 作业实测 4722 us。详细结果见
[RK3568 离线 NV12 经 RGA 写入 DRM framebuffer](rga_drm_test.md)。带方向标识的
人工视觉验收、90°路径和 CPU 参考图比对尚未执行，不将整个阶段 D 误标为完成。

### 阶段 E：端到端

- V4L2 DQBUF → RGA → DRM page flip → V4L2 QBUF。
- 先同步单线程保证正确，再引入多 buffer 流水。
- 记录 capture、RGA、flip 三段时间和端到端延迟。
- 监控 DDR 带宽、RGA 超时、DRM underrun、ISP error 和温度。

### 阶段 F：可选优化

- atomic `TEST_ONLY` 验证 NV12 overlay、rotation、modifier、CSC 属性。
- 若验证成功，比较 RGA→NV12→VOP CSC 与首版 XRGB 路径的带宽和颜色。
- 引入显式 fence 或驱动支持的可靠隐式同步。
- 加入 watchdog、信号恢复、自动重启和长期运行统计。

---

## 11. 常见误解的直接回答

### “ISP 是软件还是硬件？”

本板的 ISP 是 RK3568 内部真实硬件，地址节点为 `fdff0000.rkisp`。`rkisp_v5`、
`rkisp-isp-subdev` 和 `/dev/video0` 是控制/描述它的软件层。

### “RAW 到 RGB 是 ISP 还是 RGA？”

Bayer RAW 的去马赛克和主要图像质量处理是 ISP。RGA 只对 ISP 已生成的 NV12 做普通
YUV→RGB、旋转和缩放。若用户问题中的“RGB”实际是“RGA”的笔误，答案仍是 ISP。

### “D-PHY 是不是像 Ethernet PHY/Switch？”

它像 Ethernet PHY 的地方是负责最底层电气收发；它不是 switch，不做多端口交换、
地址学习或路由。CSI-2/DSI 控制器类似其上层协议控制逻辑，D-PHY 只处理 lane 物理层。

### “media-ctl 图上的每个框都是一个硬件吗？”

不是。Sensor entity 对应外部物理器件，CSI/ISP entities 多数映射到 SoC 硬件功能；
pads、links、`/dev/v4l-subdev*` 和 `/dev/video*` 本身都是内核软件模型。

### “ISP、RGA、VOP 是否直接串线？”

本目标方案在 ISP 输出后通过 DDR buffer 交换数据。ISP、RGA、VOP 各自用 DMA 经
SoC 内部 NoC/AXI 访问 DDR，不是板上三组视频线首尾相连。

### “VOP 与 DSI 有什么区别？”

VOP 读取 framebuffer、合成图层并生成显示时序；DSI Host 把结果封成 DSI 包；DSI
D-PHY 把包变为差分电气信号。三者不能互相替代。

### “停止 Weston 会不会让硬件不能显示？”

不会。Weston 是 DRM/KMS 的用户态客户端和桌面合成器。停止它后，专用程序可成为
DRM master，直接配置同一套 VOP/DSI 硬件。前提是先正确停止其他占用者。

### “DMA-BUF 是一条总线或协议吗？”

不是。DMA-BUF 是 Linux 内核的共享内存机制和 fd 句柄。像素仍在 DDR 中，硬件经
DMA/NoC 访问它。

### “零拷贝是不是一帧只写一次内存？”

不是。本方案因旋转而需要 ISP 写 capture buffer、RGA 读它并写 display buffer、VOP
再读 display buffer。零 CPU 拷贝只表示 CPU 不逐像素复制整帧。

---

## 12. 技术目的和最终边界

本方案的技术目的，是把“桌面环境里能显示相机”收敛为一条可解释、可测量、可独占的
嵌入式视频通路：

- Sensor/CSI/ISP 负责可靠获得并处理 RAW 图像。
- DDR/DMA-BUF 负责跨硬件模块共享帧，不做 CPU 大块复制。
- RGA 负责屏幕安装方向和显示像素格式适配。
- DRM/KMS/VOP 负责稳定扫描和显示时序。
- DSI Host/D-PHY 负责把最终 RGB 像素可靠送到物理 LCD。
- rkaiq 保留图像质量控制；Weston、Qt、GStreamer 和 GPU 从专用实时路径移除。

最终验收不只是“屏幕能亮”，而应同时满足：方向正确、颜色正确、无丢帧/撕裂、资源
所有权明确、异常可恢复，并且每个软件对象都能追溯到它控制的硬件或内存资源。

---

## 13. 参考资料

- [Rockchip RK3568 Brief Datasheet](https://www.rock-chips.com/uploads/pdf/2022.8.26/191/RK3568%20Brief%20Datasheet.pdf)
- [Rockchip RK3568 Datasheet V1.3](https://opensource.rock-chips.com/images/b/b6/Rockchip_RK3568_Datasheet_V1.3-20220929P.PDF)
- [Linux V4L2 sub-device userspace API](https://docs.kernel.org/userspace-api/media/v4l/dev-subdev.html)
- [Linux Media Controller device nodes](https://cdn.kernel.org/doc/html/latest/userspace-api/media/v4l/open.html)

本文中的板级节点、格式、分辨率、驱动绑定和运行时行为均来自当前开发板的只读探测；
芯片规格性描述以 Rockchip 数据手册为依据。Sensor 和 LCD 的精确料号必须以开发板
原理图/BOM/实物丝印为最终依据。
