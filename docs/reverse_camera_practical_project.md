# RK3568 倒车影像实战项目设计

## 1. 文档状态与项目结论

本文是建立在当前 `camera_display_stream` 0.13.0 实板成果之上的下一阶段设计，描述
一个可以逐步接入真实车辆 CAN 和 gPTP 网络的倒车影像服务。

本文中的状态分为：

- **现有事实**：已经在 RK3568 开发板上验证过；
- **本项目目标**：下一阶段准备实现，但目前还不是代码事实；
- **远期接口**：为真实 MCU、CAN 总线和 gPTP 节点预留，不应在第一阶段过度实现。

最重要的设计结论是：

1. 保留现有 `V4L2 -> DMA-BUF -> RGA -> DRM/KMS -> DSI` 图像链路；
2. 新增车辆信号层，由倒挡信号决定屏幕显示相机、待机黑屏还是故障提示；
3. 没有 MCU 时，先使用 `vcan` 或 Unix 本地模拟器生成车辆信号；
4. gPTP 用于跨节点统一时间和延迟追踪，不负责传输图像，也不决定是否进入倒车画面；
5. 保留已经实现的 V4L2 L1/L2 恢复；
6. 按当前决定，**不实现 DRM 域的进程内自主恢复**。DRM 初始化、modeset 或 page
   flip 失败后，停止继续提交，完成能够完成的清理，并以 display 域退出码退出；
7. `camera_display_stream` 继续保留为硬件链路诊断工具，另建
   `reverse_camera_service`，避免把车辆业务继续堆进现有大文件。

本项目是倒车影像工程原型，不自动等同于量产车规或法规认证产品。量产前还需要完整的
危害分析、功能安全目标、EMC/电源设计、实车视场标定和法规验证。

## 2. 项目要做成什么

### 2.1 用户可观察行为

RK3568 上电后，服务长期运行并独占车机屏幕：

```text
未挂倒挡
    -> 屏幕显示预先准备好的黑色待机画面
    -> 摄像头保持预热和采集，避免挂 R 挡后重新启动 Sensor/ISP

收到有效 R 挡信号
    -> 丢弃触发前的旧帧
    -> 取第一张触发后的新帧
    -> RGA 旋转并转换颜色
    -> DRM page flip 到倒车画面
    -> 持续显示最新相机帧

收到有效非 R 挡信号
    -> 短暂保持倒车画面，过滤换挡触点抖动
    -> page flip 回待机黑屏

相机暂时不出帧
    -> 执行当前已有的 V4L2 L1/L2 有界恢复
    -> 恢复期间保留最近有效画面，并显示故障状态（实现提示层后）

DRM/KMS 发生故障
    -> 不在进程内重建 DRM session
    -> 禁止继续复用所有权不确定的 framebuffer
    -> 记录 display 故障并退出 40
```

### 2.2 本阶段不做什么

- 不实现 DRM connector/CRTC/framebuffer 的进程内自动重建；
- 不迁移 Atomic KMS，继续使用当前验证过的 legacy SetCrtc/PageFlip；
- 不把相机图像通过 CAN 传输；CAN 只承载车辆控制信号；
- 不为了模拟 gPTP 而修改系统实时时钟；
- 不假装模拟 CAN 帧就是某一真实车型的 OEM 报文；
- 不在第一版加入轨迹线转向模型、录像、AI 检测和多摄像头拼接；
- 不重新启动 Weston。产品模式下显示资源仍由倒车影像服务独占。

## 3. 当前基线与需要新增的部分

### 3.1 当前已经实板验证的图像路径

```text
摄像头 Sensor
    -> MIPI CSI-2 / D-PHY
    -> RK3568 ISP
    -> /dev/video0
    -> 4 个 V4L2 MMAP buffer
    -> VIDIOC_EXPBUF 导出 DMA-BUF fd
    -> RGA 读取 NV12，旋转 270°并转换为 XRGB8888
    -> 2 个 DRM dumb framebuffer A/B
    -> drmModeSetCrtc / drmModePageFlip
    -> VOP
    -> DSI
    -> 1080x1920 LCD
```

当前路径没有 CPU 整帧 `memcpy`。RGA 会读取 V4L2 buffer 并写入另一块 DRM buffer，
因此仍存在一次由硬件完成的内存读写和格式转换。

### 3.2 当前已有的工程基础

- `CaptureSession` 唯一拥有 V4L2 fd、buffer pool、mmap 和 DMA-BUF；
- 采集超时支持 L1 `STREAMOFF/QBUF/STREAMON`；
- L1 失败可升级为 L2 close/open 并重建完整 V4L2 session；
- 两级恢复都有滑动窗口预算，避免恢复风暴；
- RGA 同步完成后才归还源 V4L2 buffer；
- 双 DRM framebuffer 保证 RGA 不覆盖 VOP 当前扫描的 buffer；
- page flip 等待 flip-complete 后才复用旧显示 buffer；
- `SIGINT/SIGTERM` 能进入正常清理路径；
- 故障域已经有稳定退出码：capture 20、transform 30、display 40。

### 3.3 新项目需要增加的业务模块

```text
                         +-------------------------+
SocketCAN / vcan ------->| VehicleSignalSource     |
Unix simulator --------->| CAN 解码、CRC、alive   |
                         +------------+------------+
                                      |
                                      v
                         +------------+------------+
                         | ReverseViewController   |
                         | 倒挡锁存、去抖、超时策略 |
                         +------+-------------+----+
                                |             |
                                v             v
                         CameraDisplay     Health/Log
                         Pipeline          + TimeSource
                                |
                                v
                       V4L2 -> RGA -> DRM
```

## 4. 硬件、总线和软件对象的边界

| 名称 | 类型 | 所在位置 | 本项目中的作用 |
| --- | --- | --- | --- |
| 倒车摄像头 Sensor | 物理外设 | SoC 外部 | 感光并输出 RAW Bayer |
| MIPI D-PHY | 物理收发 IP | Sensor 与 SoC 两端 | 把高速差分电信号转换为可处理的数据流 |
| MIPI CSI-2 | 数据协议 | Sensor 到 RK3568 | 规定图像包、虚拟通道和数据类型 |
| ISP | SoC 内部硬件 IP | RK3568 | RAW 去马赛克、曝光/白平衡等，输出 NV12 |
| DDR | 物理内存 | 板上 | 保存 V4L2 和 DRM 图像 buffer |
| RGA | SoC 内部硬件 IP | RK3568 | 旋转、缩放和 YUV 到 RGB 转换 |
| VOP | SoC 内部显示硬件 IP | RK3568 | 按刷新时序扫描 framebuffer |
| DSI Host/PHY | SoC 内部硬件 IP | RK3568 | 将显示像素送往 LCD 面板 |
| CAN 控制器 | SoC/MCU 内部硬件 IP | 未来车辆节点 | 收发 CAN frame |
| CAN 收发器 | 物理芯片 | 控制器与 CANH/CANL 之间 | 将逻辑电平转换为差分总线电平 |
| SocketCAN | Linux 软件接口 | RK3568 内核/用户态 | 用 socket 形式收发 CAN/CAN FD 帧 |
| `vcan` | Linux 虚拟网卡 | 内核软件对象 | 无真实总线时模拟 SocketCAN |
| gPTP | 网络时间同步协议 | 以太网网络 | 让不同 ECU 的时钟接近同一时间基准 |
| `reverse_camera_service` | 用户态进程 | RK3568 | 控制车辆信号、相机和显示策略 |

CAN 和 gPTP 是两条不同链路：

```text
车辆控制面：MCU --CAN--> RK3568 SocketCAN --gear=R--> 显示策略

时间同步面：gPTP Grandmaster --Ethernet/802.1AS--> RK3568 PHC/系统时钟

图像数据面：Sensor --CSI-2--> ISP --DDR/RGA--> VOP --DSI--> LCD
```

gPTP 不会把相机帧“同步到屏幕”，也不会替代 V4L2 timestamp 或 DRM vblank。它为来自
不同 ECU 的事件提供可比较的全局时间；单机内部的超时和状态机仍必须使用单调时钟。

## 5. CAN 倒挡信号设计

### 5.1 为什么没有 MCU 时不能直接依赖真实 `can0`

真实 CAN 总线需要：

- RK3568 CAN 控制器；
- 外部 CAN 收发器；
- CANH/CANL 线束；
- 总线两端正确的终端电阻；
- 至少另一个活动节点为发送帧提供 ACK。

如果只有开发板一个节点，即使 `can0` 能打开，发送也可能因没有 ACK 不断重试并最终
进入 error-passive 或 bus-off。因此第一阶段优先级是：

1. `vcan0`：内核支持时最接近真实 SocketCAN API；
2. Unix datagram 模拟：Buildroot 内核没有 `vcan` 时的确定性后备方案；
3. 真实 `can0`：接入 MCU、收发器和线束之后再启用。

业务状态机只依赖统一的 `VehicleSignalSource` 接口，所以更换信号源不改变显示逻辑。

### 5.2 项目自定义的模拟 CAN 帧

第一版定义一个**仅供本项目测试**的 Classic CAN 帧。它不是行业标准，也不代表真实
车型 DBC；未来接入车辆时必须由真实报文定义替换。

| 字段 | 约定 |
| --- | --- |
| CAN ID | 标准 11-bit ID `0x351`，运行时可配置 |
| 周期 | 20 ms，即 50 Hz |
| DLC | 8 bytes |
| Byte 0 bits 0..2 | 挡位：0=P、1=R、2=N、3=D、7=Invalid |
| Byte 0 bit 3 | ignition_on |
| Byte 0 bit 7 | payload_valid |
| Byte 1 bits 0..3 | 4-bit rolling counter，0..15 循环 |
| Byte 2..3 | 车速，little-endian，单位 0.01 km/h |
| Byte 4 bit 0 | 模拟 MCU healthy |
| Byte 4 bit 1 | 独立 reverse switch，供一致性诊断 |
| Byte 5..6 | 保留，模拟阶段必须为 0 |
| Byte 7 | CRC-8/SAE-J1850 |

CRC 计算覆盖 CAN ID 的两个字节以及 Data[0..6]，参数固定为：

```text
polynomial = 0x1D
init       = 0xFF
xorout     = 0xFF
```

将 CAN ID 纳入 CRC 可以避免配置了错误 ID 时仍把 payload 当成有效报文。真实车辆接入
时，CRC、counter、端序、周期和有效位全部以对应 DBC/E2E 规范为准。

### 5.3 报文有效性检查

只有同时满足以下条件的帧才能改变倒车状态：

- CAN ID 和 DLC 正确；
- `payload_valid=1`；
- 保留位符合约定；
- CRC 正确；
- rolling counter 相对上一有效帧合理前进；
- 接收时间没有超过 freshness 阈值；
- 挡位枚举合法。

错误帧只增加计数并记录限频日志，不得直接把当前倒车画面关闭。

### 5.4 激活、去抖和信号丢失策略

建议第一版使用以下项目参数，后续实车标定时配置化：

| 参数 | 初始值 | 目的 |
| --- | ---: | --- |
| CAN 周期 | 20 ms | 模拟常见周期控制信号 |
| 激活确认 | 连续 2 帧有效 R | 20~40 ms 内激活并过滤单帧毛刺 |
| 退出确认 | 连续 5 帧有效非 R | 避免换挡触点瞬态关闭画面 |
| 退出保持 | 300 ms | 减少 R/N/D 快速切换闪屏 |
| 信号 stale | 100 ms | 连续丢失 5 个周期后报警 |
| 信号 lost | 500 ms | 认为控制链路已经中断 |

安全策略采用“激活后保持”：

- 从未确认过 R 挡且 CAN 丢失：保持待机，不自行显示相机；
- 已确认 R 挡后 CAN 丢失：继续显示实时倒车画面并进入
  `SignalLostWhileActive`，不能因为通信故障突然黑屏；
- 只有收到经过完整校验的非 R 挡，或收到有效 ignition off，才解除倒车锁存；
- 车速与挡位冲突只产生诊断，不应屏蔽 R 挡画面。

这是本原型的工程策略，不代替最终车辆安全需求。量产策略应由系统危害分析决定。

## 6. 倒车业务状态机

不要把车辆状态、相机状态和时间同步状态混成一个巨大枚举。建议使用一个主显示状态，
外加三个正交健康状态。

### 6.1 主显示状态

```text
Booting
   |
   | DRM、静态画面、V4L2 和 RGA 初始化完成
   v
Standby <-------------------------------+
   |                                    |
   | 连续 2 帧有效 R                     | 退出保持到期
   v                                    |
Arming                                  |
   |                                    |
   | 收到触发后的第一张新相机帧           |
   v                                    |
Active -------- 有效非R --------> ExitHold
   ^                                    |
   | CAN丢失但最后确认是R                 |
   +------------------------------------+

任意状态 --SIGTERM--> Stopping --> Stopped
DRM致命错误 ----------> DisplayFailed --> 退出40
```

各状态含义：

- `Booting`：资源尚未全部可用，显示启动提示或黑屏；
- `Standby`：摄像头保持 STREAMON，但屏幕扫描静态黑色 framebuffer；
- `Arming`：R 挡已经确认，等待一张接收时间不早于触发时间的新帧；
- `Active`：实时转换和翻页；
- `ExitHold`：收到非 R 后继续显示 300 ms，期间重新收到 R 立即回 Active；
- `Stopping`：停止接收新工作并按资源所有权顺序清理；
- `DisplayFailed`：禁止继续提交 DRM 操作，按 display 域退出。

### 6.2 正交健康状态

```text
VehicleSignalHealth = Valid / Stale / Lost / Invalid
VideoHealth         = Healthy / RecoveringL1 / RecoveringL2 / Unavailable
TimeSyncHealth      = FreeRunning / Locked / Holdover / Fault
```

这样可以表达“当前仍在 Active，但 CAN 已 Lost、相机仍 Healthy、gPTP 处于 Holdover”，
而不需要为所有组合创建几十个主状态。

## 7. 图像显示策略

### 7.1 为什么待机时仍保持摄像头运行

倒车影像更看重挂 R 挡到第一帧可见的延迟。若 Standby 时关闭 Sensor/ISP，激活时要重新
执行上电、曝光收敛、STREAMON 和 buffer 填充，延迟和首帧质量都不稳定。

第一版采用 warm standby：

- V4L2 始终 STREAMON；
- Standby 帧 DQBUF 后立即 QBUF，不执行 RGA；
- R 挡触发时记录单调时钟 `reverse_trigger_time`；
- 丢弃 timestamp 早于触发点的积压帧；
- 第一张新帧经 RGA 转换完成后，才从黑屏切到相机画面。

这会增加摄像头和 ISP 的常态功耗，但能换取确定的低激活延迟。低功耗冷启动可作为后续
独立模式，不应与第一版同时实现。

### 7.2 建议的 DRM framebuffer

除当前相机双缓冲 A/B 外，再准备两个只写一次的静态 framebuffer：

```text
Camera A    RGA 写入；Active 时与 Camera B 交替扫描
Camera B    RGA 写入；Active 时与 Camera A 交替扫描
Standby     纯黑或产品待机画面
Unavailable 相机不可用提示图
```

1080x1920 XRGB8888 单帧约 7.9 MiB，四帧约 31.6 MiB，不含驱动对齐。是否保留两个静态
buffer 应在板端查看 CMA/DDR 预算后决定。若内存不足，可让 Standby 和 Unavailable 共用
一帧并由 CPU 在非扫描期间重绘，但必须继续遵守 framebuffer 所有权。

第一版无需字体库。故障画面可由 CPU 在初始化阶段生成固定色块和简单像素图标，运行期
不再修改正在扫描的静态 buffer。

### 7.3 最新帧优先

倒车预览应控制延迟，而不是保证每帧都显示：

- Active 时只保留最新可用帧；
- 尚未进入 RGA 的旧帧直接 QBUF；
- 绝不排队积累数百毫秒的历史图像；
- 日志分别记录 capture sequence gap 和策略主动丢帧。

当前同步 pipeline 一次只有一个 RGA/flip 流程在途，第一阶段可以保持该模型。后续若
改为异步，才需要显式的 `Ready/PendingScanout/ScanningOut` 状态和 fence。

## 8. gPTP 在本项目中的正确位置

### 8.1 gPTP 能解决什么

当未来存在 MCU、域控制器、录像 ECU 或测试 PC 时，不同设备都有自己的振荡器。gPTP
按照 IEEE 802.1AS 的时间同步模型，通过以太网把它们约束到同一时间基准。

本项目可用统一时间回答：

- MCU 在什么全局时间确认 R 挡；
- RK3568 在什么时间收到 CAN 帧；
- ISP 在什么时间完成相机帧；
- RGA 和 page flip 分别何时完成；
- 从车辆事件到画面可见的端到端延迟是多少。

### 8.2 gPTP 不能解决什么

- 不传输 CAN 帧；
- 不传输摄像头像素；
- 不保证 RGA 或 DRM 一定在期限内完成；
- 不应成为显示 R 挡画面的前置条件；
- 单节点没有 Grandmaster 时，不能声称已经验证 gPTP 同步精度。

即使 `TimeSyncHealth=Fault`，收到有效 R 挡后仍应显示相机，只是跨节点绝对时间和延迟
报告被标记为不可信。

### 8.3 两类时钟必须分工

| 用途 | 时钟 | 原因 |
| --- | --- | --- |
| CAN freshness、去抖、恢复预算、超时 | `CLOCK_MONOTONIC` | 不受系统时间校准或跳变影响 |
| 跨 ECU 日志和端到端事件时间 | `CLOCK_TAI`/受控全局时间 | 可与其他 gPTP 节点比较 |
| V4L2/DRM 内核事件 | 保留原始 timestamp + 时钟域标签 | 避免误把不同 clock domain 直接相减 |

任何日志时间都必须带 clock domain。只有建立了单调时钟到 TAI 的有效映射后，才能将
V4L2 timestamp 和外部事件转换到同一时间线。

### 8.4 没有 gPTP 节点时怎样模拟

第一阶段实现 `TimeSource` 接口：

```text
MonotonicTimeSource
    本地单机测试；业务超时的正式基础

SimulatedGptpStatusSource
    只注入 Locked/Holdover/Fault、offset_ns、uncertainty_ns
    不修改系统 CLOCK_REALTIME/CLOCK_TAI

GptpTimeSource（未来）
    读取真实 PHC/TAI 和同步守护进程健康状态
```

模拟器用于验证“失锁不影响倒车显示，但日志被标记为不可信”。不要使用 `date -s` 或
周期修改系统时间来模拟 gPTP，这会破坏日志和其他服务。

真实接入前应确认以太网设备能力：

```sh
ethtool -T eth0
ls -l /dev/ptp*
```

随后再根据车载网络 profile 配置 `ptp4l`/`phc2sys` 或厂商时间同步栈。普通 PTP
配置不自动等价于完整 802.1AS/gPTP profile。

## 9. 推荐的软件目录与接口

不要继续扩张当前约 1500 行的 `camera_display_stream_main.cpp`。建议新增：

```text
inc/reverse_camera/
  reverse_view_controller.hpp   主显示状态机
  vehicle_signal.hpp            规范化 VehicleState
  vehicle_signal_source.hpp     信号源抽象
  can_frame_decoder.hpp         项目模拟帧解码/CRC/counter
  socketcan_signal_source.hpp   真实 can0/vcan0 后端
  unix_signal_source.hpp        无 vcan 时的本地后端
  time_source.hpp               monotonic/TAI 与同步健康
  camera_display_pipeline.hpp   现有硬件链路的业务封装
  health_snapshot.hpp           可观测状态

src/reverse_camera/
  对应实现文件
  reverse_camera_main.cpp
  reverse_signal_sim_main.cpp
  time_sync_sim_main.cpp

tests/
  can_frame_decoder_test.cpp
  reverse_view_controller_test.cpp
  time_source_test.cpp

config/
  reverse_camera.conf.example
```

建议生成三个程序：

| 程序 | 作用 |
| --- | --- |
| `reverse_camera_service` | 板端正式 worker，拥有 V4L2/RGA/DRM |
| `reverse_signal_sim` | 向 vcan 或 Unix socket 周期发送 P/R/N/D 和故障帧 |
| `time_sync_sim` | 注入 Locked/Holdover/Fault，不改系统时钟 |

公共业务类型不应暴露 `struct can_frame`、libdrm 或 V4L2 原始类型。例如：

```cpp
enum class GearPosition {
    kPark,
    kReverse,
    kNeutral,
    kDrive,
    kInvalid,
};

struct VehicleState {
    GearPosition gear;
    bool ignition_on;
    bool payload_valid;
    std::uint16_t speed_centikph;
    std::uint8_t alive_counter;
    std::uint64_t receive_monotonic_ns;
};
```

`VehicleSignalSource` 负责把不同输入统一成 `VehicleState`；状态机不应该知道数据来自
真实 CAN、vcan 还是 Unix socket。

## 10. 线程和事件模型

### 10.1 第一版建议

当前 V4L2 和 DRM 接口内部各自执行阻塞 `poll`，尚未暴露统一事件循环需要的全部异步
接口。为了不同时重写显示链路，第一版可以采用两个线程：

```text
Control thread
    阻塞接收 SocketCAN/Unix 报文
    校验 CRC/counter
    更新一个受 mutex 保护的最新 VehicleState
    不接触 V4L2、RGA、DRM

Pipeline/main thread
    唯一拥有全部图像和显示资源
    每次帧等待返回后读取 VehicleState 快照
    执行状态转换、RGA 和 DRM
```

所有 V4L2/RGA/DRM 操作只能在 pipeline thread 执行，避免跨线程争夺 fd 和 buffer。
Control thread 停止时使用 pipe/eventfd 或有限超时接收，确保 SIGTERM 能 join。

为了在相机断流时仍快速响应 R 挡，不能让 pipeline thread 永久阻塞 1000 ms。倒车服务
应把正常帧等待片段缩短到 20~50 ms；只有累计时间达到现有 capture timeout 阈值时，
才触发 L1 恢复。

### 10.2 后续统一事件循环

第二阶段可以把 V4L2、DRM、CAN、signal 和 timer 全部变成一个 `poll/epoll` 循环：

```text
V4L2 fd      新帧
DRM fd       flip-complete
CAN fd       新车辆信号
signalfd     SIGINT/SIGTERM
timerfd      freshness、hold、心跳和恢复退避
```

这一步需要把当前 `pageFlipAndWait()` 拆成“提交 flip”和“处理 DRM event”两个接口。
它不是第一里程碑的前置条件，也不意味着要做 DRM 自主恢复。

## 11. DRM 不自主恢复时的明确边界

“不做 DRM 域自主恢复”应落实成可以测试的行为，而不是简单忽略错误：

- DRM 初始化失败：不启动相机业务循环，退出 40；
- 首次 modeset 失败：退出 40；
- page flip 提交失败：停止提交新帧，退出 40；
- flip-complete 超时：相关 framebuffer 所有权不再推断为 free，退出 40；
- `restore()` 失败：报告清理错误并退出 40；
- 不在当前进程中重新 probe connector/CRTC；
- 不重新申请 framebuffer；
- 不循环抢占 DRM master；
- 不偷偷恢复 Weston。

进程外 supervisor 可以记录退出码，但第一版对 exit 40 建议进入熔断状态，不立即反复
重启 worker。恢复动作由人工、点火周期或产品级上层策略触发。这样既符合“不进行 DRM
域自主恢复”，也避免 DRM 永久故障造成重启风暴。

V4L2 L1/L2 恢复不受这一决定影响，因为它只重建采集域，不改变 DRM session。

## 12. 日志、指标和延迟

每次关键事件建议输出一行稳定的 `key=value` 日志，第一版无需引入重量级日志库：

```text
event=vehicle_signal gear=R valid=1 counter=7 mono_ns=...
event=reverse_state old=Standby new=Arming reason=confirmed-reverse
event=first_visible trigger_mono_ns=... frame_mono_ns=... flip_mono_ns=...
event=time_sync state=Holdover offset_ns=... uncertainty_ns=...
event=failure domain=display operation=page-flip exit_code=40
```

至少维护：

- 有效、CRC 错误、counter 错误和超时 CAN 帧数；
- R 挡激活次数；
- trigger 到第一张新 frame 的时间；
- trigger 到 flip-complete 的时间；
- 当前画面年龄；
- 主动丢弃旧帧数；
- V4L2 L1/L2 恢复次数和耗时；
- RGA 最短/最长耗时；
- DRM page flip 最长耗时；
- gPTP 状态、offset 和 uncertainty；
- 最后成功 frame/flip 的单调时间。

第一版工程目标建议设为：

| 指标 | 原型目标 |
| --- | ---: |
| warm standby 下 R 挡到首帧 flip-complete | 典型小于 150 ms，最坏小于 300 ms |
| 正常显示帧率 | 约 30 FPS，以 Sensor 实际输出为准 |
| Active 时画面年龄 | 正常小于 150 ms |
| SIGTERM 到资源释放完成 | 正常小于 2 s |
| DRM 错误后的新提交 | 0 次 |

这些是项目验收目标，不应表述为法规限值。

## 13. 模拟器使用场景

### 13.1 vcan 模式

如果目标内核支持 vcan：

```sh
modprobe vcan
ip link add dev vcan0 type vcan
ip link set vcan0 up
```

计划中的运行方式：

```sh
reverse_camera_service \
  --video /dev/video0 \
  --drm /dev/dri/card0 \
  --vehicle-source socketcan:vcan0 \
  --can-id 0x351 \
  --time-source monotonic \
  --confirm-desktop-stopped

reverse_signal_sim --interface vcan0 --gear R --rate-hz 50
reverse_signal_sim --interface vcan0 --gear D --rate-hz 50
```

若 `modprobe vcan`、`ip link add ... type vcan` 失败，说明 Buildroot 内核或用户态工具
没有该能力，不要因此阻塞业务开发，改用 Unix 模式。

### 13.2 Unix 本地模拟模式

计划中的运行方式：

```sh
reverse_camera_service \
  --vehicle-source unix:/tmp/reverse-camera-signal.sock \
  --time-source monotonic \
  --confirm-desktop-stopped

reverse_signal_sim \
  --socket /tmp/reverse-camera-signal.sock \
  --gear R --rate-hz 50
```

Unix 后端发送的仍是同一组规范化测试字段，并经过同一解码/状态验证规则。它只替代
SocketCAN 传输层，不绕过业务状态机。

### 13.3 必须支持的故障注入

`reverse_signal_sim` 应提供：

- `--gear P|R|N|D`；
- `--rate-hz`；
- `--bad-crc N`；
- `--freeze-counter`；
- `--drop-for-ms`；
- `--invalid-gear`；
- `--ignition-off`；
- `--speed-kph`。

`time_sync_sim` 应提供：

- `--state free-running|locked|holdover|fault`；
- `--offset-ns`；
- `--uncertainty-ns`；
- `--drop-for-ms`。

## 14. 分阶段开发路线

### 阶段 A：纯业务状态机，不碰硬件主循环

实现：

- `VehicleState`；
- 模拟 CAN frame codec、CRC 和 rolling counter；
- `ReverseViewController`；
- monotonic time source；
- 宿主机单元测试。

验收：

- 两帧 R 进入 Arming；
- 非 R 去抖和 300 ms hold 正确；
- CRC/counter 错误不改变挡位；
- Active 后信号丢失仍保持 active latch；
- 所有超时测试使用注入时钟，不依赖真实 sleep。

### 阶段 B：Unix 模拟信号接入真实屏幕

实现：

- 新建 `reverse_camera_service`；
- 提取并复用现有 camera/display 硬件组件；
- Standby、Arming、Active、ExitHold；
- 静态黑屏 framebuffer；
- 控制线程和受控退出。

实板验收：

- Weston/systemui 已按现有运维策略停用；
- 服务启动后黑屏但 V4L2 持续健康采集；
- Unix 模拟 R 后 300 ms 内出现新鲜相机画面；
- 模拟 D 后经过 hold 回到黑屏；
- 连续切换 1000 次无 fd、DRM client 或 video node 泄漏；
- SIGTERM 正常清理。

### 阶段 C：SocketCAN 和 vcan

实现：

- `SocketCanSignalSource`；
- `reverse_signal_sim` 的 vcan 后端；
- CAN socket timestamp、filter、overflow 统计；
- bus-off/error frame 诊断。

验收：

- vcan 与 Unix 后端跑同一组状态机用例；
- 错 ID、错 DLC、CRC 错误、counter freeze 和总线静默均符合策略；
- 没有 vcan 的板端仍能用 Unix 后端开发。

### 阶段 D：gPTP 时间接口和可观测性

实现：

- `TimeSyncHealth`；
- `time_sync_sim`；
- 单调时间与全局时间同时记录；
- trigger/frame/flip 的端到端延迟统计。

验收：

- Locked/Holdover/Fault 转换不影响倒车显示；
- 未锁定时不输出虚假的全局延迟；
- 单调超时不受模拟 offset 变化影响。

### 阶段 E：服务守护与故障画面

实现：

- BusyBox init 兼容的 supervisor；
- worker 心跳与限频日志；
- camera unavailable 静态提示；
- 退出码策略和重启预算；
- display 40 熔断，不执行 DRM 局部恢复。

验收：

- capture 20 可按预算重启整个 worker；
- transform 30 可按产品策略受控重启；
- display 40 不形成重启风暴；
- 断电重启后不依赖上一次 framebuffer/CRTC 状态；
- 板端所有可执行文件和脚本部署到 `/home/reynor` 下的项目子目录执行。

### 阶段 F：接入真实 MCU 和车辆网络

前置条件：

- 确认开发板 CAN 引脚和外部收发器；
- 正确的 CANH/CANL 终端和共地；
- MCU 周期发送并能够 ACK；
- 获得真实 DBC/E2E 定义；
- 确认 bit rate、sample point、CAN/CAN FD 模式；
- 完成 gPTP Grandmaster、网卡硬件时间戳和网络 profile 配置。

接入时只替换 `VehicleSignalSource` 的报文映射和 `TimeSource` 后端，不重写倒车状态机和
图像 pipeline。

## 15. 测试矩阵

| 类别 | 用例 | 预期 |
| --- | --- | --- |
| 正常 | P -> R -> D | 黑屏 -> 新鲜相机帧 -> hold 后黑屏 |
| 去抖 | 单个 R 毛刺 | 不进入 Active |
| 快速换挡 | R -> N -> R | ExitHold 中返回 Active，不闪黑 |
| CAN CRC | 连续坏 CRC | 不改变锁存状态，错误计数增加 |
| CAN counter | counter 冻结 | 标记 Invalid/Stale，不误退出 R |
| CAN 静默 | Active 时断 2 s | 保持倒车画面并报告 SignalLostWhileActive |
| 相机坏帧 | `V4L2_BUF_FLAG_ERROR` | 丢帧，不显示损坏帧 |
| 相机超时 | 现有 L1 注入 | 有预算恢复并继续 Active |
| 会话失败 | 现有 L2 注入 | 重建 V4L2，不重建 DRM |
| RGA 失败 | transform error | 退出 30，不复用所有权不明 buffer |
| DRM 失败 | flip 超时/提交失败 | 退出 40，零次 DRM 自主恢复 |
| gPTP 失锁 | Locked -> Fault | 显示继续，全局时间标记不可信 |
| 生命周期 | SIGTERM | 正常 STREAMOFF、关闭 CRTC、释放 buffer |
| 长稳 | Active/Standby 24 h | 无 fd/CMA/内存增长，延迟无持续累积 |

## 16. 推荐的下一次代码任务

下一次开发只做“阶段 A”，不要立即操作 DRM：

1. 新建 `VehicleState` 和 `ReverseViewController`；
2. 实现项目模拟 CAN 帧的纯内存 codec；
3. 使用可注入单调时钟完成去抖、freshness、active latch 和 exit hold；
4. 加宿主机单元测试；
5. 不改动 `camera_display_stream` 的实板数据路径。

阶段 A 通过后，再做 Unix 模拟器与真实屏幕的阶段 B。这样如果屏幕行为错误，可以先
确认是车辆状态机问题还是硬件 pipeline 问题，不会把两类故障混在一起。

