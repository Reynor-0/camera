# RK3568 Linux 车载网关与倒车影像共存技术方案

## 1. 结论

在当前开发板上实现“Linux 应用层车载网关 + 独立倒车影像”是可行的，而且现有硬件
资源与已经完成的 Camera/RGA/DRM 基线很适合这个方向。

推荐的产品原型不是把全部功能塞进一个进程，而是两个独立业务域：

```text
通信域：gatewayd
  CAN / Ethernet 接入、校验、协议解析、状态缓存、规则路由和转发

视觉域：reverse_camera_service
  V4L2 持续采集、RGA 转换、DRM/KMS 显示和倒车策略

二者只通过 Unix Domain Socket 交换低带宽车辆状态，不传视频帧。
```

这样可以保证：

- 倒车影像失败时，CAN/Ethernet 网关继续运行；
- 网关或 ECU 离线时，Camera/RGA/DRM 链路仍能继续显示；
- 网关无法阻塞 V4L2 buffer 或占有 DRM master；
- 显示主循环阻塞时，不影响 CAN/Ethernet 接收和转发；
- 两个服务可以分别测试、退出和受控重启。

但必须明确当前边界：这块板现在适合做**应用层协议网关**，还不适合直接宣称是二层
Ethernet Switch、TSN Bridge、带防火墙/NAT 的安全 IP Router 或硬实时车规网关。
现有内核缺少 bridge、802.1Q VLAN、traffic control qdisc、netfilter filter/conntrack/
NAT 和 vcan；若以后需要这些能力，必须重配并重新构建 BSP 内核。

## 2. 与 `project1` 原架构的关系

参考文件 `/home/reynor/project1/docs/architecture.md` 的总体方向正确：网关与视觉应是
两个独立进程，统一车辆数据模型后只在业务层融合。但其中一些技术选择已经不符合当前
实板和本项目事实，需要修正：

| `project1` 中的设想 | 当前真实情况 | 本方案调整 |
| --- | --- | --- |
| C++17/20 | 当前项目强制最高 C++11 | 网关与视觉公共代码统一 C++11 |
| systemd | PID 1 是 BusyBox init | 使用 SysV `/etc/init.d/Sxx*` 和简单 supervisor |
| GStreamer/Qt/Weston | 已跑通直接 V4L2/RGA/DRM，产品模式停止 Weston | 继续复用当前 direct-display 基线 |
| vcan 模拟 | 当前内核 `CONFIG_CAN_VCAN` 未启用 | 第一版用 Unix/UDP 模拟；真实 CAN 条件具备后用 can0 |
| JSON IPC | 板端不需要引入 JSON 解析依赖 | 使用版本化 `SOCK_SEQPACKET` 二进制协议 |
| Camera Pipeline 自主恢复 | 当前只有 V4L2 L1/L2，用户已决定不做 DRM 域自主恢复 | 保留采集恢复；DRM 故障退出 40 并熔断 |
| 任意 Linux 网关能力 | bridge/VLAN/qdisc/filter/NAT 未启用 | 第一版做 CAN/Ethernet 应用协议转换，不开启内核透明转发 |
| gPTP | 当前仅运行普通 UDPv4 PTP slave 配置 | 先验证 PTP，再单独实施 802.1AS/gPTP profile |

本方案不是另起一套摄像头实现，而是把当前仓库 0.13.0 的成果作为视觉域基底。

## 3. 实板资源盘点

以下数据于 2026-08-23 通过 ADB 只读检查获得。

### 3.1 计算与存储

| 资源 | 实板状态 | 适用性判断 |
| --- | --- | --- |
| CPU | 4 个 ARMv8 Cortex-A55 级核心，408 MHz~1.992 GHz | 足以运行轻量 CAN/Ethernet 网关和硬件加速视频控制 |
| CPU 特性 | NEON/ASIMD、AES、SHA、CRC32、atomics | 有利于校验和和未来加密，但不等于已有 TLS 软件栈 |
| 调度内核 | 4.19.232，`PREEMPT_VOLUNTARY`，HZ=300 | 适合软实时原型，不是 PREEMPT_RT 或硬实时系统 |
| 内存 | 约 3.64 GiB 可用，无 swap | 空间充足，但所有队列必须有界，不能依赖 swap 兜底 |
| 根分区 | 5.9 GiB，总空闲约 4.5 GiB | 放系统和程序，不适合无界日志 |
| `/userdata` | 52 GiB，总空闲约 50 GiB | 放网关日志、诊断抓包和持久数据 |
| 温度 | 空闲时 SoC 约 47°C | 有余量，但必须做 Camera+Gateway 满载温升测试 |
| RTC | `/dev/rtc0` | 可保留断电后的粗略墙钟时间 |
| Watchdog | `/dev/watchdog0`，DW watchdog | 可由单一 health supervisor 管理 |

空闲内存很多不能证明满载性能。最终 CPU、热和延迟结论必须在倒车视频、CAN 和双网口
同时运行时测量。

### 3.2 通信接口

| 接口 | 实板状态 |
| --- | --- |
| CAN | `can0`，`rockchip_canfd` 驱动，200 MHz controller clock |
| CAN 内核 | CAN RAW、BCM、CAN Gateway 和 Rockchip CAN-FD 已启用 |
| CAN 工具 | `candump`、`cansend` 已安装 |
| Ethernet | `eth0`、`eth1`，均支持 10/100/1000BASE-T |
| Ethernet 队列 | 每口 8 TX + 8 RX queues |
| PTP | eth0 对应 `/dev/ptp0`，eth1 对应 `/dev/ptp1` |
| 时间戳 | 两个网口均声明 TX/RX hardware timestamp 能力 |
| LinuxPTP | `ptp4l`、`phc2sys` 2.0.1 已安装并由 S65 启动 |
| Wi-Fi | `wlan0`，RTL8852BS |
| UART | `/dev/ttyS3`、`ttyS4`、`ttyS8`，另有调试串口 ttyFIQ0 |
| I2C | i2c-0/1/3/4/5/6 |
| GPIO | gpiochip0..5 |
| SPI | 当前没有 `/dev/spidev*`，不能直接假设用户态 SPI 可用 |

当前 eth0/eth1 都是 `NO-CARRIER`，`ethtool` 在无链路时显示的 10 Mb/s half duplex 不是
真实协商结果。必须插入线缆并连接对端后重新确认 1000 Mb/s full duplex、误码和稳定性。

`can0` 存在只证明控制器、驱动和设备树已经创建网络接口。还必须确认板上是否有 CAN
transceiver、CANH/CANL 接线、共地、双端 120Ω 终端以及能够 ACK 的真实对端节点。

### 3.3 多媒体资源

| 资源 | 实板状态 |
| --- | --- |
| Camera | `/dev/video0`..`video8`，`/dev/video-camera0 -> video0` |
| Media Controller | `/dev/media0` |
| ISP | `rkisp_v5` 已验证输出 1920x1080 NV12，约 30 FPS |
| RGA | `/dev/rga`，现有项目已完成旋转和 NV12->XRGB 转换 |
| DRM Display | `/dev/dri/card0` + renderD128 |
| NPU | `/dev/dri/card1` + renderD129，对应 fde40000.npu |
| LCD | 现有项目已完成 legacy modeset/page flip 到 DSI 屏 |

现有连续链路已经完成 100 帧无错误采集、DMA-BUF 导出、RGA 双缓冲转换、DRM 动态翻页
以及 V4L2 L1/L2 恢复。它应作为视觉服务的唯一数据面，不再引入 GStreamer/Weston 与
当前 direct-display 路径争用同一个 Camera 或 DRM 资源。

### 3.4 当前内核的网关限制

实板 `/proc/config.gz` 显示：

```text
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_BCM=y
CONFIG_CAN_GW=y
CONFIG_CANFD_ROCKCHIP=y

# CONFIG_CAN_VCAN is not set
# CONFIG_BRIDGE is not set
# CONFIG_VLAN_8021Q is not set
# CONFIG_NET_SCHED is not set
# CONFIG_NF_CONNTRACK is not set
# CONFIG_IP_NF_FILTER is not set
```

结果是：

- 不能创建 `vcan0`；
- 不能把 eth0/eth1 建成 Linux Bridge；
- 不能配置 VLAN 子接口；
- 不能使用 mqprio/CBS/ETF/taprio 等 TSN qdisc；
- `iptables` 命令存在，但 filter table 不存在；
- 没有 conntrack/NAT；
- `net.ipv4.ip_forward` 当前为 0。

因此第一版禁止打开透明 IP forwarding，也不要向不可信网络暴露一个没有过滤能力的
路由器。南北向转发全部由 `gatewayd` 在应用层显式接收、校验、编码和发送。

## 4. 项目定位和非目标

### 4.1 第一版定位

这是一个单板 Linux 车载域控制器原型，包含：

- 一路真实 CAN/CAN-FD 控制器；
- 两路物理 Ethernet；
- 一个可替换的模拟 ECU；
- CAN 到 Ethernet 的单向遥测网关；
- Ethernet ECU 到统一车辆状态的只读接入；
- 车辆状态 freshness、quality、counter 和范围检查；
- 倒挡状态驱动的 Camera 到 LCD 显示；
- gateway/vision/time/watchdog 独立健康监控；
- 有界原始报文与故障日志。

### 4.2 第一版不做

- Ethernet 到 CAN 的任意远程控制；
- 内核透明桥接、NAT 或 Internet sharing；
- SOME/IP 全栈、DoIP/UDS 全栈；
- TSN 流量整形和带宽保证；
- 把普通 PTP 配置称为 gPTP；
- Qt/QML、Weston 或 GStreamer 桌面；
- Camera 视频经 IPC 传到 gatewayd；
- DRM 域局部自动重建；
- PREEMPT_RT、ASIL 或车规认证声明；
- OTA、Secure Boot 和量产密钥体系。

## 5. 物理拓扑

建议使用以下端口角色：

```text
                         RK3568
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  Vehicle CAN ECU                                         │
│       |                                                  │
│   CANH/CANL -- Transceiver -- can0 --+                   │
│                                      |                   │
│  Vehicle Ethernet ECU/Switch         v                   │
│       |                            gatewayd               │
│      eth0 ----------------------------+                   │
│        | PTP/未来gPTP                 | UDS VehicleState  │
│        |                              v                   │
│  Diagnostics / Test PC             reverse_camera_service│
│       |                              |                   │
│      eth1 <----受控遥测/诊断          +--V4L2/RGA/DRM-->LCD│
│                                                          │
│  MIPI Sensor --> CSI-2/D-PHY --> ISP --> DDR buffers      │
│                                                          │
│  wlan0：开发维护；正式运行默认关闭或隔离                  │
└──────────────────────────────────────────────────────────┘
```

这是端口职责建议，不是板上固定接线。若实际板载 PHY/接口标识与丝印对应关系不同，必须
通过拔插线缆和 `ethtool` 重新标定 eth0/eth1，再冻结配置。

## 6. 软件进程架构

```text
BusyBox init
  |
  +-- time-sync policy
  |     +-- ptp4l
  |     `-- phc2sys 或 NTP，不能同时无协调地校时
  |
  +-- gateway-supervisor
  |     `-- gatewayd
  |           +-- CAN adapter
  |           +-- Ethernet adapters
  |           +-- decoders/encoders
  |           +-- state store
  |           +-- route/policy engine
  |           `-- IPC publisher
  |
  +-- reverse-camera supervisor
  |     `-- reverse_camera_service
  |           +-- CaptureSession / V4L2 L1/L2
  |           +-- RGA
  |           +-- DRM/KMS
  |           +-- gateway IPC client
  |           `-- reverse view policy/overlay
  |
  `-- healthd
        `-- sole owner of /dev/watchdog0
```

### 6.1 `gatewayd`

唯一拥有 CAN 和网关 Ethernet socket，职责为：

- 配置或验证 can0、eth0、eth1；
- 接收 CAN RAW 帧和 UDP/TCP ECU 报文；
- 使用 kernel receive timestamp；
- 校验 ID、DLC、CRC、rolling counter、周期和范围；
- 解析成协议无关 VehicleSignal；
- 更新 VehicleStateStore；
- 按白名单执行 CAN->Ethernet 单向转换；
- 向视觉服务发布状态快照；
- 监控 CAN error frame、bus-off、链路 carrier 和 ECU heartbeat；
- 输出统计和有界诊断日志。

`gatewayd` 不打开 `/dev/video0`、`/dev/rga` 或 `/dev/dri/card0`。

### 6.2 `reverse_camera_service`

由当前 `camera_display_stream` 演进而来，唯一拥有视觉硬件资源：

- Camera 始终 STREAMON，保持 Sensor/ISP 热启动；
- 非 R 挡时显示黑色/待机 framebuffer，同时持续 DQBUF/QBUF；
- 收到有效 R 挡后显示第一张触发后的新鲜帧；
- RGA 旋转 270°并执行 NV12->XRGB8888；
- DRM framebuffer A/B 做 legacy page flip；
- 在未扫描 framebuffer 上叠加挡位、车速和 Gateway health；
- 保留现有 V4L2 L1/L2 有界恢复；
- RGA 错误退出 transform 域；
- DRM page flip/modeset 错误退出 display 40，不在进程内重建 DRM。

视觉进程不打开 can0，也不向 Ethernet ECU 发控制报文。

### 6.3 `healthd`

只有一个进程可以喂 `/dev/watchdog0`。否则一个失效服务可能被另一个健康服务掩盖。

原型策略：

- gatewayd 是长期必需服务；
- reverse camera 在 R 挡激活期间是高优先级健康项；
- time sync 失锁不应直接触发板级重启；
- healthd 读取进程心跳和 monotonic progress counter；
- 只有整个系统进入无法恢复且符合产品策略的状态才停止喂狗；
- 第一版优先记录和有界重启进程，不轻易用板级 reboot 代替故障分析。

内核已有 `[watchdogd]` 线程，不等于用户态产品健康策略已经完成。使用
`/dev/watchdog0` 前必须确认现有 BSP watchdog ownership，禁止两个用户态进程同时打开。

## 7. Gateway 数据处理路径

每条输入都走明确阶段：

```text
Interface RX
  -> Ingress classification
  -> Source whitelist
  -> Length/version/basic validation
  -> CRC/counter/freshness validation
  -> Protocol decode
  -> Range/unit normalization
  -> VehicleStateStore update
  -> Route/policy match
  -> Rate limit
  -> Protocol encode
  -> Egress bounded queue
  -> Interface TX
```

任何阶段失败都只增加对应 reason counter，并按限频策略输出日志。不要统一记录成模糊的
`gateway error`。

### 7.1 Adapter 接口

```cpp
struct RawMessage {
    std::uint32_t source_id;
    std::uint32_t message_id;
    std::uint64_t receive_monotonic_ns;
    std::uint64_t receive_tai_ns;
    std::uint16_t payload_size;
    std::uint8_t payload[64];
};

class MessageSource {
public:
    virtual ~MessageSource() {}
    virtual int fileDescriptor() const noexcept = 0;
    virtual bool receive(RawMessage* message) = 0;
};
```

CAN adapter 内部使用 `struct can_frame/canfd_frame`；Ethernet adapter 使用 UDP/TCP。
Linux UAPI 类型不能泄漏到 VehicleState 和 HMI 层。

`receive_tai_ns` 只有在 PTP/PHC 映射已验证且时间质量有效时才填写；否则置 0，并通过
独立的 time-quality 字段标记不可用。任何 timeout、freshness 和恢复预算仍只使用
`receive_monotonic_ns`，避免系统校时造成业务状态跳变。

### 7.2 单线程事件循环优先

第一版 `gatewayd` 使用一个 `poll/epoll` I/O 线程：

```text
can0 socket
eth0 socket
eth1 socket
IPC listen/client sockets
signalfd
timerfd：freshness、heartbeat、rate limiter 和退避
```

协议解码和路由计算应有确定上限。只有出现明显 CPU 密集任务时才增加 worker，不为每个
接口建立随意共享状态的线程。

所有发送队列固定上限。队列满时按路由优先级丢弃低优先级遥测，并记录
`dropped_backpressure`；绝不无限积压旧车辆状态。

## 8. 统一车辆状态

```cpp
enum class SignalQuality {
    kValid,
    kStale,
    kInvalid,
    kTimeout,
};

enum class GearPosition {
    kPark,
    kReverse,
    kNeutral,
    kDrive,
    kInvalid,
};

template <typename T>
struct VehicleSignal {
    T value;
    SignalQuality quality;
    std::uint32_t source_id;
    std::uint64_t source_timestamp_ns;
    std::uint64_t receive_monotonic_ns;
    std::uint64_t version;
};

struct VehicleState {
    std::uint64_t snapshot_version;
    VehicleSignal<GearPosition> gear;
    VehicleSignal<std::int32_t> speed_millikph;
    VehicleSignal<std::int32_t> steering_millidegree;
    VehicleSignal<std::int32_t> temperature_millicelsius;
    bool can_online;
    bool vehicle_ethernet_online;
};
```

公共数据模型避免 `double` 作为线协议字段，优先使用带单位的定点整数，保证跨平台编码
确定。UI 可以在显示阶段转换成小数。

每个 signal 必须携带 quality 和 receive monotonic time。ECU 不再发送时，timerfd
驱动 freshness 更新：Valid -> Stale -> Timeout，不能永久保留最后值为 Valid。

## 9. CAN 方案

### 9.1 实际能力

当前 can0 使用 Rockchip CAN-FD 驱动，但网络 MTU 为 16，说明当前处于 Classic CAN
接口形态。是否启用 CAN FD 必须根据收发器、布线、ECU 和 bit timing 一起验证，不能只
因为驱动名字含 `canfd` 就直接发送 FD frame。

### 9.2 初始配置示例

真实总线参数确认后，由独立板端脚本配置，例如 Classic CAN 500 kbit/s：

```sh
ip link set can0 down
ip link set can0 type can bitrate 500000 restart-ms 100
ip link set can0 up
ip -details -statistics link show can0
```

脚本必须说明用法、真实 bitrate 前置条件和恢复命令。`restart-ms` 初始建议 100 ms 并
配置恢复预算；当前显示的 1 ms 自动恢复过于激进，不应未经总线测试直接作为产品参数。

### 9.3 Bus-Off 和错误策略

- 打开 CAN error frame subscription；
- 记录 error-active/passive/bus-off 转换；
- bus-off 时停止应用发送；
- 由内核 restart 或显式 down/up 有界恢复；
- 60 秒内超过预算后进入 CAN Degraded；
- 即使 CAN Degraded，Ethernet 遥测和倒车 Camera 进程仍存活；
- 已经确认 R 挡后 CAN 丢失，视觉域保持相机画面并显示 signal lost，而不是突然黑屏。

### 9.4 无 MCU 时怎样模拟

当前内核没有 vcan。第一阶段使用：

```text
ecu_simulator --unix /run/gateway/ecu-sim.sock
或
ecu_simulator --udp 127.0.0.1:15000
```

模拟器生成与真实 decoder 输入等价的 frame envelope，并支持错误 CRC、counter freeze、
周期抖动和报文丢失。它只替代 transport，不绕过 gateway validation。

如果后续希望使用标准 SocketCAN vcan 测试，需要在 BSP 启用：

```text
CONFIG_CAN_VCAN=y 或 m
```

## 10. Ethernet 网关方案

### 10.1 端口角色

建议初始分工：

```text
eth0：车辆南向网络
  ECU telemetry、PTP/未来 gPTP
  静态地址或车辆网络指定配置

eth1：北向诊断/测试网络
  向开发 PC 输出经过过滤的状态
  不透明转发到 eth0

wlan0：开发维护
  正式演示时默认关闭，避免第三个不受控入口
```

ConnMan 当前运行并可能管理 eth0/eth1/wlan0。产品配置必须明确唯一网络所有者：要么
配置 ConnMan 服务策略，要么让自有 init 脚本管理网关端口。禁止 ConnMan、ifupdown 和
gatewayd 同时修改同一个接口地址。

### 10.2 第一版协议

第一版南北向使用 UDP 固定版本二进制 envelope：

```text
magic
protocol_version
message_type
source_id
sequence
source_timestamp_ns
payload_length
payload
crc32
```

使用 network byte order，接收端严格校验长度、版本、source、sequence、CRC 和速率。
UDP 适合周期状态；需要可靠请求/响应时单独使用 TCP，不在 UDP 上自行拼装一个不完整的
可靠传输协议。

### 10.3 初始路由方向

只开放：

```text
CAN -> decoder -> VehicleState -> UDP telemetry on eth1
eth0 ECU telemetry -> decoder -> VehicleState -> local HMI/filtered eth1 telemetry
```

默认禁止：

```text
eth1 arbitrary packet -> can0
eth1 -> eth0 transparent forwarding
Internet/Wi-Fi -> vehicle CAN control
```

Ethernet 到 CAN 的控制必须等命令模型、认证、重放保护、速率限制和安全状态定义完成后，
逐条白名单开放。

### 10.4 当前安全边界

当前内核没有 netfilter filter/conntrack，板端也没有 `openssl` 命令。第一版安全措施只能
先落在应用层和物理网络隔离：

- socket 只绑定指定接口和地址；
- source IP/MAC/port/message ID 白名单；
- 所有输入严格长度与版本校验；
- 单源和单 message rate limit；
- 默认只读、单向转发；
- 北向网络不直接拥有 CAN 发送能力；
- 配置和模型不接受网络热更新；
- Wi-Fi 正式运行默认关闭。

如果要接不可信网络或 Internet，必须先补齐内核 firewall、TLS 库/证书、身份认证、密钥
保护和安全更新机制，不能把当前原型直接暴露出去。

## 11. PTP 与 gPTP

### 11.1 当前到底具备什么

当前实板具备：

- eth0/eth1 hardware TX/RX timestamp 声明；
- `/dev/ptp0` 和 `/dev/ptp1`；
- linuxptp 2.0.1；
- S65linuxptp 启动 `ptp4l` 和 `phc2sys`。

但 `/etc/linuxptp.cfg` 当前配置是：

```text
slaveOnly 1
network_transport UDPv4
time_stamping hardware
[eth0]
```

这属于普通 UDPv4 PTP slave 配置，不是已经完成的 IEEE 802.1AS/gPTP。当前网口又没有
carrier，因此不能声称时间已经同步。

### 11.2 当前还存在时间源冲突风险

板端同时运行：

- `ntpd`；
- `ptp4l`；
- `phc2sys -s eth0 -c CLOCK_REALTIME`。

当 PTP 和 NTP 都试图校准 `CLOCK_REALTIME` 时可能互相干扰。正式方案必须有一个
TimeAuthorityManager：

```text
PTP Locked       -> phc2sys 驱动系统时钟，NTP 不参与 discipline
PTP Holdover     -> 保留最近频率估计，标记 holdover
PTP Unavailable  -> 可切换到 NTP fallback，但记录 time quality
```

切换不能只看进程是否存在，必须读取 PTP port state、offset 和 freshness。

### 11.3 时间戳分工

| 用途 | 时钟 |
| --- | --- |
| freshness、timeout、恢复预算、限速 | `CLOCK_MONOTONIC` |
| 单板业务日志 | `CLOCK_REALTIME` + time quality |
| 跨 ECU 可比时间 | gPTP 锁定后的 `CLOCK_TAI`/PHC 映射 |
| CAN/UDP 接收 | kernel timestamp + clock domain 标签 |
| Camera/V4L2 | 保留原始 timestamp 和其 clock domain |

网关和倒车显示绝不能依赖 gPTP Locked 才开始工作。失锁只降低跨节点时间可信度，不应
阻断倒挡画面。

### 11.4 真正实施 gPTP 的前置条件

- 对端 ECU/Switch 支持相同 802.1AS profile；
- 确认 eth0 PHY/MAC 硬件时间戳在真实链路上可调且稳定；
- 解决当前 PHC `max_adjust` 报告为 0 的可疑点；
- 选择 L2 transport、P2P delay 和 transportSpecific 等 profile 参数；
- 确认 Grandmaster/BMCA 策略；
- 测量 offset、path delay、holdover 和断链恢复；
- 如果需要跨 eth0/eth1 转发时间，单独设计 boundary clock，不要把两个独立 PHC 当成
  天然同一个时钟。

由于当前没有 `CONFIG_NET_SCHED`，即使完成 gPTP，也没有 taprio/CBS/ETF 流量整形。
时间同步和 TSN 调度是两件不同的能力。

## 12. Gateway 与倒车影像 IPC

使用 Unix `SOCK_SEQPACKET`：

```text
/run/gateway/vehicle-state.sock
```

协议要求：

- magic 和 protocol version；
- 固定 little-endian 序列化；
- 不发送原始 C++ struct padding；
- 每条消息有 payload length 和 sequence；
- 连接建立后先 HELLO/HELLO_ACK 协商版本；
- gateway 周期发布完整 snapshot，例如 20 Hz；
- ECU online/offline、bus-off 等作为事件附加发布；
- HMI 不通过 IPC 请求 gateway 打开任意 CAN 发送通道。

消息类型：

```text
HELLO
VEHICLE_STATE_SNAPSHOT
ECU_HEALTH_EVENT
CAN_STATE_EVENT
ETHERNET_STATE_EVENT
TIME_SYNC_STATUS
GATEWAY_HEARTBEAT
SHUTDOWN
```

视觉服务保存最后 snapshot 的 receive monotonic time。超过 100 ms 标记 Stale，超过
500 ms 标记 Timeout。已经确认 R 挡后 IPC 断开时继续显示实时相机，并叠加 Gateway
Offline；只有收到完整校验的有效非 R 状态才退出倒车画面。

IPC 不发送 Camera frame 或 DMA-BUF，网关不需要理解视频 buffer 生命周期。

## 13. 倒车显示状态机

```text
Starting
   |
   | Camera/RGA/DRM 初始化成功
   v
Standby（Camera持续采集，LCD黑屏）
   |
   | 连续有效 gear=R
   v
Arming（等待触发后第一张新鲜帧）
   |
   v
Active（实时Camera + 状态叠加）
   |
   | 有效非R，去抖和hold
   v
ExitHold ----------------------> Standby

Active + Gateway/IPC失联 -> SignalLostActive，保持Camera
任意状态 + SIGTERM       -> Stopping -> Stopped
DRM错误                  -> DisplayFailed -> exit 40
```

视觉服务使用当前同步 RGA + legacy page flip 双缓冲。网关项目不要求先迁移 Atomic KMS，
也不重新启动 Weston。

## 14. 资源与调度隔离

### 14.1 所有权隔离

| 资源 | 唯一所有者 |
| --- | --- |
| can0 socket/config | gatewayd 或其启动脚本 |
| eth0/eth1 gateway sockets | gatewayd |
| `/dev/video0` | reverse_camera_service |
| `/dev/rga` jobs | reverse_camera_service |
| `/dev/dri/card0` master | reverse_camera_service |
| `/dev/ptp0`/clock policy | time sync service |
| `/dev/watchdog0` | healthd |

### 14.2 初始资源预算

以下是验收预算，不是当前测量结果：

| 项目 | 原型预算 |
| --- | ---: |
| gatewayd RSS | < 100 MiB |
| reverse camera RSS，不含内核/CMA | < 200 MiB |
| gateway bounded queues 总和 | < 32 MiB |
| 空闲系统可用内存警戒线 | > 512 MiB |
| CAN RX -> VehicleState P99 | < 5 ms，目标负载下 |
| CAN -> Ethernet 转发 P99 | < 10 ms，目标负载下 |
| gear=R -> 首帧 flip-complete | warm standby 最坏 < 300 ms |
| HMI snapshot freshness | 正常 < 100 ms |

CPU affinity 和实时优先级只在测量后配置。不能简单把所有业务设置为 `SCHED_FIFO`；
错误的实时线程可能饿死 ksoftirqd、存储和 DRM event。第一版使用普通调度、短临界区、
有界工作量，再依据 `top`、IRQ 和 P99 延迟决定是否用 `taskset/chrt`。

### 14.3 过载策略

过载时优先级：

```text
1. CAN RX 和安全相关状态 freshness
2. 倒车 Camera capture/display
3. 高优先级网关路由
4. 普通 telemetry
5. 原始报文日志和调试输出
```

日志和北向遥测允许有界丢弃；CAN 接收和倒车画面不能因为写日志而阻塞。

## 15. 故障与恢复边界

| 故障 | 局部动作 | 升级动作 | 不影响的域 |
| --- | --- | --- | --- |
| CAN 单帧非法 | 丢帧、计数 | 持续异常标记 ECU Invalid | Camera/DRM |
| CAN timeout | signal Stale/Timeout | 重建 CAN socket/有界接口恢复 | Camera |
| CAN bus-off | 停止发送 | 有预算 restart；耗尽后 Degraded | Camera、Ethernet只读 |
| eth0 link down | 标记 ECU offline | 周期探测 carrier | CAN、Camera |
| eth1 link down | 丢弃北向 telemetry | 等待链路恢复 | CAN、本地HMI |
| gatewayd 崩溃 | HMI 标记 Gateway Offline | supervisor 有界重启 | Camera/RGA/DRM |
| V4L2 timeout | 现有 L1 | L1失败升级现有L2 | gatewayd |
| V4L2预算耗尽 | exit 20 | supervisor 受控重启视觉进程 | gatewayd |
| RGA失败 | exit 30 | 有界重启视觉进程 | gatewayd |
| DRM失败 | exit 40 | 熔断，不做DRM局部自主恢复 | gatewayd |
| PTP失锁 | time quality 降级 | NTP fallback 或 holdover | gateway和Camera功能 |
| 日志盘满 | 停止低优先级日志 | StorageDegraded | 数据转发和Camera |

所有恢复都有滑动窗口预算、退避和结构化原因。不要用无限 `while restart` 隐藏永久硬件
错误。

## 16. BusyBox init 与启动顺序

当前 PID 1 是 BusyBox init，不使用 systemd。建议脚本：

```text
/etc/init.d/S35gateway-interfaces
/etc/init.d/S65linuxptp        现有脚本需重构时间源策略
/etc/init.d/S66gatewayd
/etc/init.d/S67reverse-camera
/etc/init.d/S68vehicle-healthd
```

这里的 S65 是保留并重构 BSP 现有 `S65linuxptp`，不是再创建一个同名脚本；部署前还要
检查其他 BSP 脚本是否占用 S66~S68。项目脚本仍存放在
`/home/reynor/gateway-platform/scripts/`，由安装动作显式链接/复制到 `/etc/init.d`，并
提供恢复脚本。

启动顺序：

```text
syslog/udev/network
-> 配置端口 ownership
-> time service
-> gatewayd
-> reverse_camera_service
-> healthd 最后接管 watchdog
```

停止顺序：

```text
healthd 停止 watchdog policy
-> reverse camera 完成 STREAMOFF/DRM 清理
-> gatewayd 停止新转发并关闭 sockets
-> time/network service
```

每个脚本文件开头必须写清用途、用法、前置条件、修改内容和恢复方法，并遵守
[项目开发规范](development_guidelines.md)。

## 17. 配置与目录

### 17.1 仓库建议

不建议把全部 gateway 代码继续写入 `camera_display_stream_main.cpp`。可在当前仓库逐步
形成：

```text
apps/
  gatewayd/
  reverse-camera/
  ecu-simulator/
  healthd/

libs/
  gateway-core/
  can-adapter/
  ethernet-adapter/
  vehicle-model/
  gateway-ipc/
  common/

config/
  interfaces.ini
  routes.ini
  signals.ini
  reverse-camera.ini
  time-sync.ini

tools/gateway/
tests/gateway/
docs/
```

如果保持当前扁平 CMake，也至少把公共类从现有 main 文件抽离，不能让 gateway 依赖
camera CLI 的匿名 namespace 类型。

### 17.2 板端建议

```text
/home/reynor/gateway-platform/
  bin/
  config/
  scripts/
  share/

/userdata/gateway-platform/
  logs/
  captures/
  state/
```

交叉编译得到的所有可执行文件仍通过 ADB 放到 `/home/reynor` 下的项目子目录运行。

### 17.3 配置格式

第一版使用严格 INI 或自有小型 key/value parser，避免为了 YAML/JSON 引入目标板没有的
运行库。配置启动时一次加载并验证：

```ini
[can0]
enabled=true
bitrate=500000
fd=false
restart_ms=100

[ethernet]
southbound=eth0
northbound=eth1
ip_forward=false

[gateway]
vehicle_state_publish_hz=20
max_tx_queue=1024
raw_log_enabled=false

[reverse_camera]
video=/dev/video0
drm=/dev/dri/card0
width=1920
height=1080
rotation=270
color_mode=bt709-limited
```

DBC 可在开发机用工具生成 C++ decoder table，第一版不强制在 Buildroot 上部署完整 DBC
解析器。真实 ECU 接入后再冻结 CAN ID、factor、offset、endianness、counter 和 CRC。

## 18. 日志与可观测性

### 18.1 Gateway 指标

- CAN RX/TX frames、error frames、bus-off、restart；
- 每个 CAN ID 周期 P50/P95/P99 和 timeout；
- Ethernet RX/TX packets、sequence gap、CRC error、link state；
- 每条 route accepted/dropped/rate-limited/backpressure；
- VehicleState snapshot version 和 signal quality；
- IPC client、publish rate、last heartbeat；
- 事件循环最长一次处理时间；
- RSS、CPU、fd count、queue high-water mark。

### 18.2 Visual 指标

- capture/display FPS；
- frame age、RGA min/max、page flip max；
- V4L2 timeouts、L1/L2 attempts；
- Gear trigger 到第一张新帧和 flip-complete 延迟；
- Gateway snapshot age；
- display exit domain/code。

### 18.3 存储

使用 syslog 的同时把高价值结构化事件写入 `/userdata` 的固定大小轮转文件。当前没有
`logrotate` 命令，因此需要项目自己的按大小/数量轮转，或将 logrotate 加入 Buildroot。

原始 CAN/Ethernet 报文默认不长期记录。诊断抓包显式启用，设最大文件、最大总量和超时，
避免 50 GiB `/userdata` 被持续流量填满。

## 19. 分阶段实施路线

### 阶段 A：冻结和回归当前视觉基线

- 保留 `camera_display_stream` 作为诊断程序；
- 回归 1920x1080 NV12、RGA 双缓冲和 lifecycle；
- 回归 V4L2 L1/L2；
- 明确 DRM 不自主恢复边界；
- 记录空闲/Active CPU、内存、温度和 FPS。

验收：现有全部实板测试通过，网关开发尚未改动视觉路径。

### 阶段 B：Gateway Core 与无硬件模拟

- VehicleSignal/VehicleState；
- validation、freshness、state store、route engine；
- Unix/UDP ECU simulator；
- signalfd/timerfd 事件循环；
- 单元测试和故障注入。

验收：挡位、车速、counter、CRC、timeout 和 route 在宿主机可确定复现。

### 阶段 C：Ethernet 最小闭环

- 固定 eth0/eth1 ownership；
- UDP southbound/northbound adapter；
- source/message whitelist；
- 有界 queue/rate limit；
- 断链恢复和统计。

验收：不启用 ip_forward，PC 模拟 ECU -> gatewayd -> 过滤后的另一端 telemetry 成功。

### 阶段 D：真实 CAN

- 核对 transceiver 和终端；
- can0 bitrate/bit timing；
- listen-only 抓取；
- decoder 和 CAN->UDP；
- error frame/bus-off 恢复预算。

验收：真实对端连续收发、断线/短时 bus-off 可诊断，Camera 不受影响。

### 阶段 E：Gateway IPC

- `SOCK_SEQPACKET` 版本协议；
- 20 Hz VehicleState snapshot；
- heartbeat 和 quality；
- gatewayd/HMI 独立 kill 测试。

验收：停止任一进程，另一个保持核心功能；重连后 snapshot generation 正确。

### 阶段 F：倒车策略与叠加

- warm standby；
- Standby/Arming/Active/ExitHold；
- Gear R 触发；
- stale/lost 策略；
- 车速/挡位/Gateway health overlay。

验收：gear=R 到可见新帧 <300 ms，gateway 失联不突然黑屏。

### 阶段 G：PTP 时间策略

- 真实链路 `ethtool -T`/PHC 验证；
- 解决 ntpd 与 phc2sys ownership；
- PTP lock/offset/holdover 监控；
- timestamp domain 映射；
- 再评估 802.1AS/gPTP profile。

验收：断开/恢复 PTP 不影响网关功能，time quality 和 offset 可观测。

### 阶段 H：Supervisor、Watchdog 和长稳

- BusyBox init；
- 进程 restart budget/backoff/circuit breaker；
- healthd sole watchdog owner；
- 日志轮转和磁盘保护；
- 24/72 小时 Camera+CAN+双网口并发测试。

验收：无 fd/内存/日志无界增长，任何单域故障不会无故拖垮另一域。

### 阶段 I：可选 BSP 增强

确有需求时启用并逐项验证：

```text
CONFIG_CAN_VCAN
CONFIG_BRIDGE
CONFIG_VLAN_8021Q
CONFIG_NET_SCHED
CONFIG_NET_SCH_MQPRIO/CBS/ETF/TAPRIO
CONFIG_NF_CONNTRACK
CONFIG_IP_NF_FILTER
CONFIG_NF_NAT
```

这是一项 BSP 变更，不属于普通应用开发。启用后仍需检查驱动、用户态工具、性能和安全
策略，不能因 config=y 就宣布具备 TSN 或安全路由能力。

## 20. 验收矩阵

| 类别 | 用例 | 预期 |
| --- | --- | --- |
| 隔离 | kill reverse camera | gatewayd 持续接收和转发 |
| 隔离 | kill gatewayd | Camera 继续，状态显示 offline |
| CAN | 正常 500 kbit/s 流量 | 周期、counter、状态解析正确 |
| CAN | 错 DLC/CRC/counter | 丢弃并按原因计数 |
| CAN | bus-off | 有界恢复或 Degraded，不影响 Camera |
| Ethernet | eth0 断线 | ECU offline，CAN/HMI 继续 |
| Ethernet | eth1 断线 | 北向停发，南向处理继续 |
| 安全 | eth1 未授权消息 | 不转发到 can0 |
| 背压 | 北向消费者变慢 | telemetry 有界丢弃，CAN RX 不阻塞 |
| IPC | snapshot 延迟/断开 | quality 变 stale/timeout |
| Reverse | gear R | 300 ms 内显示新鲜帧 |
| Reverse | Active 时 Gateway 崩溃 | 继续 Camera，显示 Gateway Offline |
| Capture | L1/L2 注入 | 视觉恢复，gateway 不受影响 |
| DRM | page flip 失败 | 视觉退出 40，gateway 继续，DRM 不局部恢复 |
| PTP | master 消失 | time quality 降级，业务超时仍由 monotonic 正常驱动 |
| Watchdog | gateway 心跳停止 | 按策略重启/报警，不被其他服务假喂狗掩盖 |
| 性能 | Camera+CAN+双ETH满目标负载 | 达到资源和P99预算，无持续升温失控 |
| 长稳 | 24/72 h | 无资源泄漏、重启风暴和日志占满 |

## 21. 推荐的下一步

下一次代码阶段不应立刻把 CAN、Ethernet 和 Camera 全部拼在一起。推荐先完成
`gatewayd` 的纯软件最小闭环：

1. `VehicleState`、quality 和 freshness；
2. Unix/UDP 模拟 ECU；
3. 固定版本 Ethernet envelope；
4. 单向 route：模拟 CAN/ECU -> VehicleState -> UDP telemetry；
5. 有界 queue、rate limit、heartbeat 和统计；
6. host 单元测试；
7. 交叉编译并部署到 `/home/reynor/gateway-platform/bin`；
8. 板端通过 eth0/eth1 与 PC 模拟器完成闭环。

这个阶段完全不打开 Camera/DRM。网关闭环稳定后再加入 UDS VehicleState IPC，最后让
倒车视觉服务订阅 Gear 状态。这样能清楚区分通信、业务状态和视频问题，也能持续保留
当前已经稳定的摄像头显示基线。
