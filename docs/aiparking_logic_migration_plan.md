# AIparking 业务逻辑迁移到 RK3568 Camera 项目的技术方案

## 1. 文档结论与状态

把旧 `AIparking` 的停车场业务迁移到当前 RK3568 Camera 项目是可行的，但正确做法是
迁移业务语义和状态机，而不是把旧 C 文件、32 位库和历史第三方目录直接加入当前构建。

当前项目已经完成并实板验证：

```text
Sensor/ISP
  -> V4L2 1920x1080 NV12 MMAP + DMA-BUF
  -> RGA 旋转和 NV12 到 XRGB8888
  -> DRM/KMS 双 framebuffer page flip
  -> VOP/DSI/LCD
```

因此新项目必须保留这条显示主路，并在它旁边增加低频、可丢弃、不会反压显示的车牌
识别支路。推荐目标为：

```text
parking_camera_service       C++11，实时 Camera/RGA/DRM 数据面
parkingd                     C++11，多路刷卡输入、业务状态机、计费、SQLite 单写者
parkingctl                   C++11，本地手动模拟刷卡和状态查询工具
parking_lpr_service          Python 3.8，HyperLPR3/ONNX Runtime
parking_audio_service        可选，异步语音/提示音，失败可降级
```

本文主要描述目标设计，不能据此判断每个模块已经实现。截至 2026-09-17，C++11 领域
模型、`parkingd`、`parkingctl`、手动 Unix socket 输入和 SQLite v1 repository 已经在
RK3568 实板通过，并验证了进程重启后的 ACTIVE 状态和 event 幂等恢复；真实 RFID 和
Camera/HyperLPR 闭环尚未实现。逐项证据以
[AIparking 迁移实施进度](aiparking_implementation_progress.md) 为准。Camera 链路仍以
0.13.0 实现和实板验收为基线。

与已有 [停车场车牌识别与 SQLite 入库设计](parking_lot_lpr_project.md) 的关系是：该文档
详细讨论连续识别、HyperLPR3 部署和数据库持久化；本文进一步基于旧 `AIparking` 源码，
恢复并重构“RFID 入场/出场、抓拍、识别、计费、播报”的完整业务闭环。

## 2. 本次实际分析的旧代码

旧项目位于：

```text
AIparking/开发板代码/
  main.c
  RFID.c
  SQLite.c
  Audio.c
  Video.c
  camera.c
  include/
  lib/
  HyperLPR-master/
  ttsSDK/
```

其中你自己编写的 6 个主要 C 文件约 1485 行。`AIparking` 整体约 205 MiB，大部分空间
来自旧 HyperLPR、OpenCV Android SDK、模型、编译缓存和语音 SDK，并不等于需要迁移
205 MiB 代码。

### 2.1 旧程序的进程关系

`main.c` 创建 FIFO 和 POSIX named semaphore，然后 fork/exec 四个模块：

```text
main
  +-- RFID
  +-- SQLite
  +-- Audio
  `-- Video
```

旧 IPC：

```text
/tmp/fifo1  RFID -> SQLite：入场卡号
/tmp/fifo2  RFID -> SQLite：出场卡号
/tmp/fifo3  SQLite -> Audio：播报文本
/tmp/fifo4  Video -> SQLite：识别后的车牌

sem1        子进程启动完成计数
sem2        SQLite 通知 Video 抓拍
```

### 2.2 旧入场流程

```text
RFID 读到卡
  -> 通过 fifo1 发送 int cardid
  -> SQLite 查询 info 表中是否已经存在该卡
  -> 不存在则 sem_post(SEM_TAKEPHOTO)
  -> Video 使用当前 gyuv 指针生成 a.jpg
  -> system("./alpr a.jpg")
  -> alpr 把车牌写入固定文件 license
  -> Video 读取 license 并通过 fifo4 返回车牌
  -> SQLite 插入 卡号/车牌/时间
  -> 通过 fifo3 请求“欢迎某车入场”语音
```

### 2.3 旧出场流程

```text
RFID 读到卡
  -> 通过 fifo2 发送 int cardid
  -> SQLite 查询活动记录
  -> 根据 time(NULL)-entry_time 计算演示费用
  -> DELETE 删除 info 表记录
  -> 通过 fifo3 请求收费播报
```

### 2.4 旧音频流程

```text
SQLite 文本
  -> Audio.c UDP 发送到 192.168.9.100:50001
  -> x86/x64 语音 SDK 合成 WAV
  -> UDP 返回 size 和数据块
  -> 保存固定文件 a.wav
  -> system("aplay a.wav")
```

### 2.5 旧 Video 流程

```text
/dev/video* YUYV single-planar
  -> CPU 查表 YUV->RGB
  -> mmap /dev/fb0 直接写 framebuffer

RFID 触发时：
  当前 gyuv 指针 -> CPU RGB -> libjpeg -> a.jpg -> 外部 alpr
```

这部分已经被当前项目的 V4L2 multi-planar、DMA-BUF、RGA、DRM/KMS 路径完整替代。

## 3. 当前 RK3568 与旧产物的兼容性结论

以下事实于 2026-09-08 进行了实板和 ELF 检查。

### 3.1 可以直接利用的板端能力

| 能力 | 当前实板状态 |
| --- | --- |
| 用户态架构 | AArch64，glibc 2.35 |
| SQLite CLI | 3.21.0 |
| SQLite C 库 | `/usr/lib/libsqlite3.so`，交叉 sysroot 有头文件和库 |
| JPEG C 库 | 交叉 sysroot 有 `jpeglib.h` 和 AArch64 `libjpeg.so` |
| Python | 3.8.6 |
| OpenCV Python | 4.5.5 |
| NumPy | 1.18.2 |
| ALSA 播放 | `/usr/bin/aplay`，`/dev/snd` 存在 |
| 串口 | `/dev/ttyS3`、`ttyS4`、`ttyS8` |
| 持久存储 | `/userdata` 约 50 GiB 可用 |

### 3.2 不能直接搬运的旧二进制

| 旧文件 | ELF 架构 | 结论 |
| --- | --- | --- |
| `libsqlite3arm.so` | ELF32 ARM | 不能用于 AArch64；使用 sysroot 的 SQLite |
| `libjpeg.so.9` | ELF32 ARM | 不能用于 AArch64；使用 sysroot 的 libjpeg |
| 旧 `HyperLPR-master/alpr` | ELF32 ARM | 不能在当前 AArch64 用户态直接运行 |
| `libsqlite3x86.so` | x86-64 | 只能用于 x86 主机，不上板 |
| 语音 `libmsc.so` | x86/x86-64 | 不能在 RK3568 本地加载 |
| Android OpenCV/HyperLPR 库 | armeabi-v7a/Android | ABI和平台都不匹配 |

旧 Makefile 使用 `arm-none-linux-gnueabi-gcc`，目标是 32 位 ARM；当前项目必须继续使用：

```text
aarch64-buildroot-linux-gnu-
```

旧 HyperLPR CMake 还硬编码了 `/usr/local/arm/...`、`/home/gec/...` 和 OpenCV 3.4.15，
不能加入当前 CMake。

### 3.3 当前缺失能力

- Python 还没有安装 `hyperlpr3`；
- Python 还没有安装 `onnxruntime`；
- 板端没有旧代码使用的 `/dev/beep`；
- 只有 UART 设备节点，RFID 读卡器型号、接线和串口仍需实物确认；
- `aplay` 存在只证明 ALSA 播放路径可用，扬声器、音量和具体声卡仍需测试；
- 旧外部 TTS 服务地址和协议不能假定仍可用。

## 4. 哪些逻辑保留，哪些必须重写

### 4.1 保留业务语义

- RFID 卡作为一次入场/出场请求的身份输入；
- 重复入场卡拒绝；
- 未入场卡直接出场拒绝；
- 入场时请求相机取得新鲜图像并识别车牌；
- 入场持久化卡号、车牌和时间；
- 出场查询活动停车记录并计算停车时长/费用；
- 成功和失败产生语音或提示音；
- 各模块可独立诊断。

### 4.2 不迁移旧技术实现

- 不迁移 `/dev/fb0` CPU 刷屏；
- 不迁移旧 YUYV 单平面 Camera 代码；
- 不迁移 256x256x256 的巨大 RGB 查表；
- 不迁移 `gyuv` 全局裸指针；
- 不迁移 `a.jpg`、`license`、`a.wav` 固定临时文件协议；
- 不迁移 32 位 `alpr`、SQLite、JPEG 二进制；
- 不迁移 fork 后任何子进程退出就 SIGKILL 全部模块的策略；
- 不迁移按回车切换 IN/OUT 的生产逻辑；
- 不迁移拼接 SQL 字符串和 callback 全局变量；
- 不迁移无超时、无校验、无请求 ID 的 UDP TTS 协议。

### 4.3 旧实现中必须修复的具体问题

| 旧实现 | 风险 | 新设计 |
| --- | --- | --- |
| `gyuv` 指向当前 V4L2 mmap | QBUF 后驱动可覆盖，抓拍线程发生数据竞争 | QBUF 前复制/转换到独立 inference slot |
| `v4lbuf.index=i%nbuf` | 忽略 DQBUF 实际返回 index | 使用当前 `CapturedFrame::buffer_index` |
| ioctl 返回值基本不检查 | 失败后继续使用无效状态 | 保留现有异常、故障域和 L1/L2 恢复 |
| convert 线程创建后不 join | 显示可能读取未初始化查表 | 删除查表，继续使用同步 RGA |
| `get_bcc()` 的 `bcc` 未初始化 | RFID 校验值不确定 | 从明确初值计算并写协议单测 |
| card UID 填入 `int` | 字节序、符号和前导零丢失 | 保存规范化十六进制 byte string |
| FIFO 直接读写裸 `int`/字符串 | 无消息边界、版本、请求关联 | Unix `SOCK_SEQPACKET` 版本协议 |
| FIFO 以 O_RDWR 打开 | 对端消失可能无法感知 EOF | 心跳、断开检测和重连状态机 |
| `sem1` 表示所有模块 ready | 无法知道具体哪个模块或失败原因 | 每个服务独立 READY/DEGRADED/FAILED |
| signal handler 中 printf/kill/exit | 非 async-signal-safe，清理不可控 | handler 只置标志/signalfd，主循环清理 |
| 正常结束使用 SIGKILL | V4L2/DRM/SQLite/WAV 无法正常收尾 | SIGTERM、超时等待、最后才强杀 |
| SQLite 两线程共享全局状态 `t` | 查询结果串扰和数据竞争 | 单 writer connection + typed repository |
| SQL 字符串拼接 | 引号、UTF-8、注入和截断风险 | prepared statement + bind |
| 出场直接 DELETE | 历史、审计和计费记录丢失 | session 从 ACTIVE 更新为 EXITED |
| 车牌长度必须等于 9 字节 | 中文 UTF-8/新能源牌处理错误 | UTF-8 字符串+模型置信度+合法性策略 |
| UDP size 使用本机字节序 | 跨平台不确定且无法关联包 | 明确网络字节序、request ID、长度和 CRC |

## 5. 目标项目范围

### 5.1 第一版实现

- 单车道、单 Camera；真实 RFID reader 和手动模拟入口可独立启停并同时工作；
- Camera 持续采集并持续显示；
- RFID 事件触发 3~5 帧识别 burst；
- 多帧车牌共识，不再单帧直接入库；
- SQLite 保存活动停车和完整历史；
- 入场、重复入场、出场、无记录、识别失败状态；
- 可配置的演示计费策略，金额使用整数分；
- 异步语音/提示音；
- 屏幕叠加当前业务状态；
- 每个 IPC 请求具有 request ID 和超时；
- Camera、LPR、数据库、音频故障相互隔离；
- 支持手动模拟刷卡和 LPR 模拟器，在没有读卡器/模型时先完成完整业务测试；
- 每个刷卡事件保留 `serial_rfid` 或 `manual_simulator` 来源，便于审计和清理测试数据。

### 5.2 第一版明确不做

- 支付平台和真实扣款；
- 车辆道闸安全控制；
- 车位引导和空位检测；
- 多入口/多出口分布式一致性；
- 车辆类型和车型识别；
- 把 Python 推理嵌入 Camera 实时线程；
- 每帧执行 HyperLPR；
- 运行时联网下载模型；
- 使用旧 32 位二进制；
- 把识别成功率宣称为车规/商用保证；
- DRM 域进程内自主恢复。

若以后控制真实道闸，必须增加车辆存在传感器、防砸检测、开/关到位反馈、人工急停和
独立安全控制器。当前软件只能输出“建议授权”，不能直接承担人员/车辆安全功能。

## 6. 推荐总体架构

```text
真实 RFID reader ──UART──┐
                         v
                    ┌───────────────────────────┐
parkingctl ──UDS───>│ parkingd                  │
手动模拟刷卡         │                           │
                         │ Card validation           │
                         │ Access state machine      │
                         │ Fee policy                │
                         │ SQLite single writer      │
                         │ Idempotency/event journal │
                         └───────┬──────────┬────────┘
                                 │          │
                     CaptureBurst│          │AudioAnnouncement
                                 v          v
┌──────────────────────────────┐     ┌─────────────────────────┐
│ parking_camera_service       │     │ parking_audio_service   │
│                              │     │ bounded async queue     │
│ V4L2 CaptureSession          │     │ prerecorded/TTS client  │
│ RGA display transform        │     │ ALSA/aplay adapter      │
│ DRM double page flip         │     └─────────────────────────┘
│ RecognitionFramePool         │
│ OverlayRenderer              │
└───────────────┬──────────────┘
                │ FRAME_READY(slot/request/sequence)
                │ DMA-BUF fd 仅注册时 SCM_RIGHTS 传递
                v
       ┌────────────────────────────┐
       │ parking_lpr_service        │
       │ HyperLPR3 long-lived model │
       │ per-request consensus      │
       │ bounded latest-frame input │
       └─────────────┬──────────────┘
                     │ LPR_RESULT(request_id,...)
                     └──────────────> parkingd
```

服务之间只传业务消息和专用 inference buffer。Camera 的原始 V4L2 buffer 不跨进程长期
持有，SQLite 数据库也不由 Camera/LPR 直接写。

## 7. 服务职责和资源唯一所有者

### 7.1 `parking_camera_service`

由当前 `camera_display_stream` 演进，唯一拥有：

- `/dev/video0`；
- V4L2 MMAP buffers 和导出的 DMA-BUF fd；
- `/dev/rga` 作业；
- `/dev/dri/card0` DRM master、CRTC 和 framebuffers；
- recognition buffer pool；
- 屏幕 overlay。

它只负责实时图像和请求关联，不判断卡是否合法、不计算费用、不写数据库、不合成语音。

### 7.2 `parkingd`

唯一拥有：

- 0~N 个 RFID serial fd，以及可选的手动模拟控制 socket；
- 将真实刷卡和手动模拟输入归一化为同一种 `CardPresentedEvent`；
- 停车业务状态机；
- SQLite 写连接；
- 活动 session cache；
- 计费策略；
- request/event UUID；
- 对 Camera、LPR、Audio 的控制消息。

所有数据库写入都经一个 writer context，避免旧代码多个线程共享 callback/global `t`。

### 7.3 `parking_lpr_service`

唯一拥有：

- HyperLPR3 和 ONNX Runtime context；
- 模型文件映射；
- inference slots 的 CPU 映射；
- 每个 request 的候选列表和多帧共识；
- LPR 性能统计。

模型只加载一次。单帧异常只拒绝当前帧，不终止 Camera。

### 7.4 `parking_audio_service`

唯一拥有：

- 有界播报队列；
- WAV cache；
- 外部 TTS socket 或本地 TTS adapter；
- ALSA/aplay 播放进程；
- 音频超时、去重和失败统计。

音频服务失败不回滚已经成功提交的停车事务，也不能阻塞 Camera 和 RFID。

### 7.5 进程监督

当前系统 PID 1 是 BusyBox init，不使用 systemd。第一版可以使用独立 SysV init 脚本或
轻量 supervisor，但不要复用旧 `main.c` 的“任一 SIGCHLD 就 SIGKILL 全部”行为。

Camera display 域退出 40 时遵守当前决策：不在 Camera 进程内部重建 DRM。其他服务可
继续工作并记录显示故障；是否由产品 supervisor 重启整个视觉服务，应经过恢复预算和
明确策略，而不是无限忙重启。

## 8. 核心业务数据模型

以下类型是 C++11 领域模型，不暴露 SQLite row、FIFO 或 Linux tty 类型：

```cpp
enum class LaneDirection {
    kEntry,
    kExit,
};

enum class AccessStatus {
    kCreated,
    kValidatingCard,
    kAwaitingCapture,
    kAwaitingRecognition,
    kCommitting,
    kGranted,
    kRejected,
    kTimedOut,
    kCancelled,
};

struct CardId {
    std::string normalized_hex;
};

struct AccessRequest {
    std::string request_id;
    LaneDirection direction;
    CardId card;
    std::uint64_t created_monotonic_ns;
    std::int64_t created_utc_ms;
    AccessStatus status;
};

struct PlateCandidate {
    std::string plate_utf8;
    std::int32_t confidence_millionths;
    std::uint32_t frame_sequence;
    std::uint64_t capture_monotonic_ns;
};

struct ParkingSession {
    std::string session_id;
    CardId card;
    std::string plate_utf8;
    std::int64_t entry_utc_ms;
    std::int64_t exit_utc_ms;
    std::int64_t fee_cent;
};
```

在线协议和数据库金额不能使用 `float`。金额统一用 `int64` 分，置信度可以用 0~1000000
定点整数，避免不同语言浮点编码差异。

Card UID 不再转成宿主机 `int`，而是按读卡器返回的原始字节顺序转换为大写十六进制，
例如 `04A10B7F`，保留前导零。

## 9. 入场状态机

### 9.1 推荐流程

```text
Idle
  |
  | CardPresented(direction=ENTRY)
  v
ValidateCard
  |
  +-- disabled/invalid card ----------------> Rejected
  |
  +-- card already has ACTIVE session ------> RejectedDuplicateEntry
  |
  `-- valid
       v
CreateAccessRequest(request_id)
       |
       | CAPTURE_BURST(request_id, count=5)
       v
AwaitRecognition
       |
       +-- timeout/no consensus ------------> RecognitionFailed
       |
       `-- confirmed plate
              v
        BEGIN IMMEDIATE
        insert ACTIVE parking_session
        insert access_event ENTRY_GRANTED
        COMMIT
              |
              +--> Camera overlay: ENTRY GRANTED
              +--> Audio: 欢迎入场
              `--> Future BarrierAdapter: access recommendation
```

### 9.2 新鲜帧要求

Capture request 必须携带 `created_monotonic_ns`。Camera 只接受：

```text
frame.capture_monotonic_ns >= request.created_monotonic_ns
```

避免卡片触发后使用触发前缓存的历史画面。

### 9.3 Burst 和共识

第一版建议在 1~2 秒内采集 3~5 个识别样本：

- 同一规范车牌至少出现 3 次；
- 单帧置信度和平均置信度均达到配置；
- 每个 sample 的 request ID 一致；
- 超时后所有迟到结果丢弃；
- 没有共识时不得伪造车牌或写入空字符串。

旧代码只有一张 `a.jpg`，任何一次误识别都会成为正式记录。多帧共识是必须补上的业务
可靠性，不是额外 UI 功能。

### 9.4 数据库提交与授权顺序

只有 SQLite 事务成功后才能报告 `ENTRY_GRANTED`。如果数据库返回 FULL/IOERR/CORRUPT，
应进入 `StorageDegraded` 并拒绝宣称已经入场。

真实道闸项目还需要 passage sensor 区分“已授权”和“车辆确实通过”。当前没有该传感器，
数据库只能记录软件授权事件，不能把它等价为物理车辆已经进入。

## 10. 出场状态机

第一版保留旧项目的“刷卡出场，按卡查活动记录”，但不再 DELETE：

```text
CardPresented(direction=EXIT)
  -> validate card
  -> query ACTIVE session by card UID
  -> not found: EXIT_NO_ACTIVE_SESSION
  -> found:
       calculate duration and fee
       BEGIN IMMEDIATE
       update session ACTIVE -> EXITED
       write exit_time/duration/fee
       insert access_event EXIT_GRANTED
       COMMIT
       publish overlay and audio
```

可选增强：出口也请求 Camera 识别，并比较当前 plate 与入场 plate。结果不一致时进入
`ManualReview`，不能简单自动覆盖入场车牌。

同一个 session 的退出请求必须幂等。IPC 重试、RFID 卡未及时移开或进程重连不能产生
两次计费。

## 11. RFID 接入设计

### 11.1 要实现的“双输入”是什么意思

真实刷卡和手动模拟不是两个互斥的程序模式，而是两个可独立启用的**事件来源**：

```text
SerialRfidSource（可开/关） ─┐
                             ├─> CardPresentedEvent -> 同一个入/出场状态机
ManualCardSource（可开/关） ─┘
```

因此可以形成三种部署状态：

| 使用场景 | 串口源 | 手动源 | 行为 |
| --- | --- | --- | --- |
| 当前无硬件开发 | 关 | 开 | 通过命令模拟车辆入场/出场 |
| 联调读卡器 | 开 | 开 | 真实刷卡可用，同时保留人工故障注入入口 |
| 生产锁定 | 开 | 默认关 | 只接受真实读卡器；维护窗口才临时开启手动源 |

手动事件只替代“卡号是怎样进入系统的”这一小段。它不能绕过卡号合法性、重复入场检查、
车牌识别、数据库事务、计费和幂等判断。默认实验配置下，手动事件会真实修改测试数据库，
效果就像刷了一张卡；数据库必须记录它是模拟事件，不能伪装成硬件刷卡。

### 11.2 统一事件与 Adapter 接口

```cpp
enum class CardEventSource {
    kSerialRfid,
    kManualSimulator,
};

struct CardPresentedEvent {
    std::string event_id;
    CardId card;
    LaneDirection direction;
    CardEventSource source;
    std::string source_instance;
    std::uint64_t receive_monotonic_ns;
    std::int64_t receive_utc_ms;
};

class CardEventSourceAdapter {
public:
    virtual ~CardEventSourceAdapter() {}
    virtual int fileDescriptor() const noexcept = 0;
    virtual bool readEvent(CardPresentedEvent* event) = 0;
};
```

`parkingd` 用 `poll()` 同时等待所有启用的 adapter，不为每一种来源复制业务状态机：

```text
SerialRfidSource     真实 UART/9600 8N1 协议；方向来自该 reader 的固定配置
ManualCardSource     本地 Unix socket；方向和卡号来自每次 parkingctl 请求
```

`source_instance` 用于区分 `entry-reader-0`、`exit-reader-0`、`local-cli` 等实例。事件来源
只描述事实，是否允许驱动未来的物理道闸由 `parkingd` 的策略决定，不能由请求方自行声明。

### 11.3 为什么不用进程 stdin、signal 或裸 FIFO

常驻进程由 BusyBox init/supervisor 启动后通常没有可靠的交互终端；多个 ADB shell 也无法
安全共享它的 stdin。`SIGUSR1` 带不了完整卡号和入/出口方向，裸 FIFO 则缺少身份、消息
边界、版本和确认回执。因此主方案使用本机 Unix `SOCK_SEQPACKET`：

```text
/home/reynor/camera-project/bin/parkingctl
        -> /run/parking-lot/control.sock
        -> parkingd/ManualCardSource
```

开发者仍然是在“相关进程中手动触发”，只是通过一个专门的控制客户端触发，而不是要求
守护进程占住某个终端。这样从任意 ADB shell 都能操作，也不会影响进程常驻和自动重启。

### 11.4 手动模拟命令

在板端 shell 直接执行：

```sh
/home/reynor/camera-project/bin/parkingctl card-present \
    --direction entry --card 04A10B7F --wait-ms 5000

/home/reynor/camera-project/bin/parkingctl card-present \
    --direction exit --card 04A10B7F --wait-ms 5000
```

也可以从 WSL 一步触发：

```sh
adb shell /home/reynor/camera-project/bin/parkingctl card-present \
    --direction entry --card 04A10B7F --wait-ms 5000
```

协议至少返回两层结果：

```text
ACK   request_id=... accepted=true source=manual_simulator
FINAL request_id=... outcome=ENTRY_GRANTED session_id=...
```

`ACK` 只表示请求格式合法并已进入有界队列，不等于车辆已经入场。使用 `--wait-ms` 时，
CLI 等待相同 request ID 的最终业务结果；超时也不会擅自重发并制造第二次入场。建议退出码：

- `0`：收到最终结果（具体 granted/rejected 仍打印在结果中）；
- `2`：命令或参数错误；
- `3`：控制 socket 不可用；
- `4`：入口校验拒绝或队列已满；
- `5`：等待最终结果超时。

卡号必须经过与真实读卡器完全相同的 `CardId` 规范化。手动请求不允许指定数据库 SQL、
最终识别结果或“强制放行”，防止测试接口变成绕过业务规则的后门。

### 11.5 手动控制面的安全边界

- socket 只监听 Unix path，不监听 TCP；
- 默认权限 `0660`，owner/group 由部署脚本明确创建；板端尚无专用用户组时先限制为 root；
- 服务端用 `SO_PEERCRED` 记录调用进程的 pid/uid/gid；
- 请求包含协议版本、request ID、长度上限，非法包直接拒绝并计数；
- 设置队列容量、每秒速率限制和单卡去抖，避免命令洪泛；
- `manual_input.enabled=false` 时 socket 不创建，不能只在 UI 上隐藏；
- 模拟事件默认禁止驱动未来的真实 `BarrierAdapter`，除非维护配置显式授权；
- 生产数据库如需验证手动入口，必须在审计记录中保留来源；日常测试优先使用单独数据库。

### 11.6 Serial reader 修正

- 使用明确 BCC 初值；
- 严格检查帧长、命令、状态、BCC 和结束字节；
- 处理 partial read/write、EINTR、EAGAIN；
- 使用 poll/定时器，不使用 10 ms 后盲读固定长度；
- 记录协议 timeout、bad checksum 和 malformed frame；
- 卡片在场去抖使用 monotonic timer，不在 signal handler 中改共享 bool；
- 串口恢复具有次数预算和退避；
- 启动时打印真实 tty、baud、data/parity/stop bits；
- 关闭时恢复 termios 或明确由进程独占。

### 11.7 入/出口方向来源

旧代码通过 stdin 回车在 IN/OUT 间切换，只适合课堂演示。新设计优先级：

1. 生产：入口和出口各自固定 reader/lane 配置；
2. 单通道原型：配置文件固定 `direction=entry` 或 `exit`；
3. 手动模拟：每条 `parkingctl card-present` 命令显式携带 `entry` 或 `exit`，不改变真实
   reader 的配置；
4. 自动方向：以后接地感线圈/GPIO/CAN 状态，不能仅靠软件猜测。

## 12. Camera 显示主路如何接入识别

### 12.1 当前真实 buffer 生命周期

当前主循环已经正确执行：

```text
waitForFrame
-> tryDequeue(CapturedFrame)
-> validate metadata
-> RGA 读取 V4L2 DMA-BUF，写未扫描 DRM framebuffer
-> 同步 RGA 返回
-> requeue(frame.buffer_index)
-> DRM show/pageFlipAndWait
```

`CapturedFrame::data` 和 `dma_buf_fd` 都是借用值。调用 `requeue()` 后，buffer 所有权回到
ISP，任何其他线程或进程都不能继续读取该帧。

### 12.2 正确的识别分支插入点

```text
DQBUF：应用拥有源帧
  |
  +-- RGA display transform -> 未扫描 DRM framebuffer
  |
  `-- 若有有效 CaptureBurst 请求且 inference slot 空闲
        -> RGA crop/rotate/scale/color convert
        -> 独立 inference slot
        -> 标记 ReadyForLpr
  |
  v
所有同步读源操作完成
  -> QBUF：源帧归还 ISP
  -> page flip
  -> 通知 LPR FRAME_READY
```

绝不能这样做：

```text
全局指针 = CapturedFrame::data
QBUF
另一个线程稍后读取全局指针
```

这正是旧 `gyuv` 设计的核心数据竞争。

### 12.3 `RecognitionFramePool`

建议准备 3 个较小的专用 buffer：

```text
Free
  -> RgaWriting
  -> ReadyForLpr
  -> LprReading
  -> Free
```

每个 slot 包含：

- slot index；
- pool generation；
- DMA-BUF fd；
- CPU mmap 地址；
- width/height/stride/fourcc；
- request ID；
- V4L2 frame sequence；
- capture monotonic/realtime timestamp。

当前项目的 `rotateNv12ToBgrx8888()` 只实现了经过实板验证的固定 270°转换，并没有
通用 crop/resize 接口。阶段 5 应新增独立的 RGA inference transform，先用离线图像验证
ROI、旋转、缩放、stride 和颜色，再接进实时循环；不要为了识别功能改变现有显示函数的
已验证语义。

初始 allocator 可以复用 DRM dumb/GEM 的线性内存机制，但应抽出不调用
`drmModeAddFB2()` 的普通 `RgaLinearBuffer`，避免把“可供 RGA 写的内存”错误建模成
“正在用于扫描的 DRM framebuffer”。该 buffer 创建 GEM、mmap 并导出 PRIME DMA-BUF，
只由 inference pool 管理，不提交给 VOP。

初始化 IPC 时使用 `SCM_RIGHTS` 向 LPR 进程发送每个 fd 的副本一次。之后只发送 slot 和
metadata。LPR 返回 `FRAME_DONE(slot,generation)` 后才能复用。

当前 RGA 显示路径输出 XRGB8888；在 AArch64 little-endian 内存中通常表现为 B、G、R、X
字节顺序。Python 必须依据协议中的 fourcc/stride 创建四通道 NumPy view，再显式生成
HyperLPR3 要求的连续 BGR 三通道输入。这个低频 CPU copy 可以先接受并测量，不能把
带 4 字节 pixel stride 的切片误当成连续三通道图像。

### 12.4 背压

Camera 主路永远优先：

- 没有 Free slot 就跳过这张识别样本；
- 不阻塞 V4L2 requeue；
- 不排队历史帧；
- request burst 超时就返回失败；
- LPR 崩溃时隔离旧 generation，再整体重置 inference pool；
- 显示仍按约 30 FPS 运行。

由于识别由 RFID 事件触发而不是每帧触发，初始方案可以接受每次请求 3~5 次额外 RGA
作业。若实测 page flip P99 被影响，再降低 inference 分辨率或把 burst 间隔拉开。

## 13. HyperLPR 迁移路线

### 13.1 不继续使用旧 `alpr`

旧 `alpr` 存在以下问题：

- ELF32 ARM，不能直接在当前 AArch64 运行；
- 基于旧 OpenCV 3.4/Caffe 模型；
- 每次 `system()` 都重新创建进程和加载模型；
- 固定输入 `a.jpg` 和输出 `license`；
- 没有 request ID；
- 没有可靠的错误码、候选列表和结构化置信度；
- 固定文件造成并发覆盖和迟到结果串单。

### 13.2 推荐 HyperLPR3 独立服务

沿用已有设计：Python 3.8 + HyperLPR3 + ONNX Runtime，模型离线部署，进程长期持有
模型。当前板端还没有 `hyperlpr3` 和 `onnxruntime`，所以第一步必须是单图兼容性探针，
不能先修改实时主循环。

兼容性闸门：

```text
G1  Python/NumPy/OpenCV/ONNX Runtime/HyperLPR3 import
G2  离线模型哈希检查和加载
G3  官方样例识别
G4  当前 Camera 保存图片的方向、颜色和 ROI 验证
G5  100 张循环推理内存稳定
G6  RK3568 P50/P95/P99 推理时间
```

版本、wheel 和模型以真正上板通过的组合冻结，不在运行期联网升级。详细部署约束见
[停车场车牌识别与 SQLite 入库设计](parking_lot_lpr_project.md)。

### 13.3 模拟器先行

在 HyperLPR3 尚未安装时提供：

```text
fake_lpr_service
  input: request ID + frame metadata
  output: configurable plate/confidence/delay/error
```

它用于先完成 RFID、状态机、SQLite、IPC 超时和屏幕状态，不伪装成真实模型验收。

## 14. SQLite 数据库设计

### 14.1 数据位置和运行模式

```text
/userdata/parking-lot/db/parking.db
/userdata/parking-lot/images/
/userdata/parking-lot/logs/
/userdata/parking-lot/backups/
```

启动时验证：

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=3000;
PRAGMA wal_autocheckpoint=100;
```

数据库只有一个 writer connection。状态查询工具使用只读、短事务。

### 14.2 目标 Schema

以下是完整项目的目标 schema，包含卡片白名单、图片、时间质量和未来迁移字段。0.15.0
已经落地的是其最小可运行 v1 子集：`schema_meta`、`parking_sessions`、ACTIVE 卡部分唯一
索引和 `access_events`。实际字段、事务边界与实板证据见
[AIparking 迁移实施进度](aiparking_implementation_progress.md)，不能把下面尚未实现的
`cards`、图片路径等字段当作当前数据库事实。

```sql
CREATE TABLE IF NOT EXISTS schema_meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS cards (
    card_uid TEXT PRIMARY KEY,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    owner_label TEXT,
    registered_plate TEXT,
    created_utc_ms INTEGER NOT NULL,
    updated_utc_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS parking_sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_uuid TEXT NOT NULL UNIQUE,
    entry_request_uuid TEXT NOT NULL UNIQUE,
    exit_request_uuid TEXT UNIQUE,

    card_uid TEXT NOT NULL,
    plate_number TEXT NOT NULL,
    entry_source TEXT NOT NULL
        CHECK (entry_source IN ('serial_rfid', 'manual_simulator', 'migration')),
    entry_source_instance TEXT NOT NULL,
    exit_source TEXT
        CHECK (exit_source IS NULL OR
               exit_source IN ('serial_rfid', 'manual_simulator', 'migration')),
    exit_source_instance TEXT,
    status TEXT NOT NULL
        CHECK (status IN ('ACTIVE', 'EXITED', 'EXCEPTION', 'LEGACY_UNKNOWN')),

    entry_time_utc_ms INTEGER NOT NULL,
    entry_time_quality TEXT NOT NULL,
    entry_capture_monotonic_ns INTEGER NOT NULL,
    entry_frame_sequence INTEGER NOT NULL,
    entry_confidence_millionths INTEGER NOT NULL,
    entry_image_path TEXT,

    exit_time_utc_ms INTEGER,
    exit_time_quality TEXT,
    parking_duration_seconds INTEGER,
    fee_cent INTEGER,

    created_utc_ms INTEGER NOT NULL,
    updated_utc_ms INTEGER NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_active_card
ON parking_sessions(card_uid)
WHERE status = 'ACTIVE';

CREATE INDEX IF NOT EXISTS idx_sessions_plate_time
ON parking_sessions(plate_number, entry_time_utc_ms DESC);

CREATE TABLE IF NOT EXISTS access_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_uuid TEXT NOT NULL UNIQUE,
    request_uuid TEXT NOT NULL,
    session_uuid TEXT,
    event_type TEXT NOT NULL,
    source TEXT NOT NULL
        CHECK (source IN ('serial_rfid', 'manual_simulator', 'migration', 'system')),
    source_instance TEXT NOT NULL,
    outcome TEXT NOT NULL,
    reason_code TEXT NOT NULL,
    event_time_utc_ms INTEGER NOT NULL,
    event_monotonic_ns INTEGER NOT NULL,
    detail TEXT,
    UNIQUE(request_uuid, event_type)
);
```

如果第一版不做卡片预注册，`cards` 表可以为空，策略允许未知卡作为临时卡；如果做白名单，
则必须先查 `cards.enabled`。两种模式由配置决定，不能隐藏在代码中。

`LEGACY_UNKNOWN` 只供旧数据库迁移使用，正常入场流程不得生成该状态，也不能把它计入
当前在场数量。人工核对后才能将其修正为 ACTIVE、EXITED 或 EXCEPTION。

### 14.3 为什么不再 DELETE

旧 `info` 表只保存当前在场车辆，出场后直接删除，导致：

- 无法审计车辆历史；
- 无法复核计费；
- 无法统计占用和流量；
- 无法区分合法退出、重复退出和数据库故障。

新表将 `ACTIVE` 更新为 `EXITED`，完整保留 entry/exit/duration/fee。

### 14.4 Prepared statement 和事务

车牌、卡号和 UUID 全部用绑定参数，不通过 `snprintf` 拼 SQL。查询和写入检查每一个
SQLite result code、`sqlite3_step()` 和 `sqlite3_changes()`。

业务线程等待 LPR 和 Audio 时不得保持 SQLite transaction。事务只覆盖最终状态检查和
写入：

```text
外部耗时工作完成
-> BEGIN IMMEDIATE
-> 再次验证 active session 不变量
-> INSERT/UPDATE session
-> INSERT access_event
-> COMMIT
```

request UUID 和 UNIQUE constraint 保证消息重放不会重复入场或计费。

真实与模拟来源必须进入相同事务。`parking_sessions.entry_source/exit_source` 方便区分一条
停车记录由什么触发，`access_events.source/source_instance` 则保存每次尝试的审计来源。
测试结束后可以按来源查询或清理实验数据，但不能在正常业务路径中偷偷省略模拟记录。

### 14.5 数据库损坏与断电

- 启动运行 `PRAGMA quick_check`；
- WAL 模式备份使用 SQLite backup API，不直接只复制主文件；
- FULL/IOERR/CORRUPT 时停止写入并保存现场；
- 不自动删除损坏数据库；
- 磁盘不足提前进入 StorageDegraded；
- 图片先写临时文件、fsync、rename，再在事务中记录最终路径；
- 数据库失败不能对外报告“入场成功”。

## 15. 时间和计费

### 15.1 两套时钟

| 用途 | 时钟 |
| --- | --- |
| IPC timeout、burst、去抖、恢复预算 | `CLOCK_MONOTONIC` |
| 入场/出场时间、跨重启计费 | `CLOCK_REALTIME` UTC ms + time quality |

不能用 monotonic 保存跨重启账单，也不能用可能被 NTP/PTP 调整的 realtime 做 2 秒识别
timeout。

### 15.2 FeePolicy

旧代码把 1 秒直接当 1 元，30 秒后固定 100 元，只能作为教学演示。新接口：

```cpp
struct FeeResult {
    std::int64_t duration_seconds;
    std::int64_t fee_cent;
    std::string policy_version;
};

class FeePolicy {
public:
    virtual ~FeePolicy() {}
    virtual FeeResult calculate(std::int64_t entry_utc_ms,
                                std::int64_t exit_utc_ms) const = 0;
};
```

初始配置至少包括：

- 免费时长；
- 计费粒度；
- 每单位金额；
- 每日上限；
- 跨日规则；
- 时钟无效/倒退策略；
- policy version。

金额全部使用整数分，禁止 `float`。当 `exit < entry` 或时间质量不可接受时进入人工复核，
不能得到负费用。

## 16. 音频、蜂鸣器和未来道闸

### 16.1 音频策略

优先级建议：

1. 第一版使用预录 WAV 验证 ALSA/aplay；
2. 动态车牌/金额使用独立外部 TTS service；
3. 找到许可和 AArch64 支持后再评估本地 TTS；
4. 不把旧 x86 `libmsc.so` 链到 RK3568。

新的外部 TTS 协议至少有：

```text
magic/version
request_id
UTF-8 text length
payload length
network byte order
response status
CRC/hash
timeout/retry budget
```

使用 TCP 最容易获得完整消息边界；若继续 UDP，必须处理丢包、重复、乱序、来源验证和
分片。旧协议只有 size 和数据块，无法可靠关联并发请求。

### 16.2 播报时机

```text
数据库 COMMIT 成功
  -> enqueue AudioAnnouncement
  -> parkingd 立即继续处理
  -> audio service 异步播放
```

音频失败只产生 `AUDIO_FAILED` 事件，不回滚停车 session。

### 16.3 蜂鸣器

当前板端没有 `/dev/beep`。先实现：

```text
BuzzerAdapter
  +-- NullBuzzer：记录日志
  +-- AudioToneBuzzer：播放短 WAV
  `-- GpioBuzzer：确认硬件和极性后再实现
```

不要在未确认 GPIO 复用、电气电平和驱动方式前直接操作随机 gpiochip line。

### 16.4 道闸

旧项目实际没有道闸闭环。未来 `BarrierAdapter` 必须是独立可测试模块，并至少包含：

- open/close command；
- open/closed position feedback；
- obstacle/anti-crush input；
- command timeout；
- manual emergency mode；
- hardware fail-safe。

在这些输入不存在时，只输出 `ACCESS_GRANTED_RECOMMENDATION`，不宣称已经安全控制道闸。

## 17. IPC 协议

### 17.1 Transport

本机进程使用 Unix `SOCK_SEQPACKET`：

- 保留消息边界；
- 支持检测连接关闭；
- 支持 `SCM_RIGHTS` 注册 DMA-BUF；
- 无需开放 TCP 端口；
- 可以按 socket 文件权限限制访问。

不要直接发送 C++ struct 内存，因为 padding、enum size 和字节序不是稳定协议。

### 17.2 Envelope

```text
magic
protocol_version
message_type
header_length
payload_length
sequence
request_id[16]
sender_instance_id[16]
monotonic_ns
payload
crc32（需要时）
```

### 17.3 消息类型

```text
HELLO / HELLO_ACK
SERVICE_READY
HEARTBEAT

CARD_PRESENTED
ACCESS_REQUEST_STATE
CAPTURE_BURST_REQUEST
CAPTURE_BURST_CANCEL

REGISTER_INFERENCE_BUFFER
FRAME_READY
FRAME_DONE
LPR_CANDIDATE
LPR_CONFIRMED
LPR_FAILED

PARKING_SESSION_COMMITTED
AUDIO_ANNOUNCEMENT
AUDIO_RESULT
OVERLAY_STATE

SHUTDOWN
```

### 17.4 Request 生命周期

- `request_id` 全局唯一；
- 每个状态转换带单调 sequence；
- 接收方记住最近完成 request，重复消息返回原结果；
- 超时后迟到结果只能计数，不能影响新 request；
- 服务重启后使用新的 `sender_instance_id`；
- inference slot 使用 generation，旧 `FRAME_DONE` 不能释放新 slot；
- 队列全部有固定上限和明确 overflow policy。

## 18. 屏幕叠加

Camera 画面保持全速。业务状态以低频 snapshot 叠加在当前未被 VOP 扫描的 framebuffer：

```text
RGA 完成 XRGB8888 目标
-> OverlayRenderer 写未扫描 framebuffer
-> DRM page flip
```

禁止修改正在扫描的 framebuffer。

建议状态：

| 状态 | 显示 |
| --- | --- |
| Idle | 当前在场数量、入口就绪 |
| Card detected | 正在验证卡片 |
| Capturing | 正在取得新鲜帧 |
| Recognizing | 正在识别车牌 |
| Entry granted | 车牌、入场时间、绿色状态 |
| Duplicate entry | 卡已在场，黄色警告 |
| Exit granted | 停车时长、费用 |
| LPR offline | 识别服务离线，Camera 继续 |
| DB error | 红色 Storage/DB fault，不宣称成功 |
| RFID offline | 串口/reader 离线 |

显示结果带最大年龄。旧请求的车牌和 box 超时后清除，不能长期留在实时画面上。

若需要中文，必须明确字体授权、UTF-8 解码和 glyph rasterizer。第一阶段可以先用 ASCII
状态码，避免 UI 阻塞核心闭环。

## 19. 启动、停止和故障隔离

### 19.1 启动

```text
准备 /userdata 目录与权限
-> 数据库 migration + quick_check
-> audio service（失败允许降级）
-> LPR service（失败允许 Camera-only）
-> parkingd
-> camera service
-> 各连接通过 HELLO 自主重连
```

不再使用一个共享 semaphore 等待四次。每个服务独立发布 READY/DEGRADED/FAILED 和具体
reason。

### 19.2 停止

```text
parkingd 停止接受新的真实或手动刷卡请求
-> cancel/finish 有限时间内的 access request
-> flush/close SQLite writer
-> audio 停止接受并清理播放子进程
-> LPR 释放模型和 inference slots
-> Camera 停止新帧、STREAMOFF、DRM restore、释放 buffer
```

正常路径使用 SIGTERM。超过明确 grace period 后，supervisor 才能升级强制终止。

### 19.3 故障矩阵

| 故障 | 处理 | 不应影响 |
| --- | --- | --- |
| RFID checksum/timeout | 丢帧、计数、有界重试 | Camera 显示 |
| RFID 断开 | 标记 offline、等待重连 | Camera/LPR/DB 历史 |
| 手动控制 socket 异常 | 拒绝该命令、记录计数、重建监听 socket | 真实 RFID/Camera/DB 历史 |
| LPR 单帧失败 | 当前 sample rejected | Camera/其他 request |
| LPR 进程崩溃 | request 超时、slot generation 隔离、受控重启 | Camera 显示 |
| LPR 模型缺失 | LPR offline，禁止联网下载 | Camera/DB 查询 |
| SQLite BUSY | busy timeout + 有界重试 | Camera |
| SQLite FULL/IOERR | StorageDegraded、拒绝伪成功 | Camera/RFID诊断 |
| Audio 超时/崩溃 | 丢弃或重试播报，记录结果 | 已提交 session |
| V4L2 timeout | 使用现有 L1/L2 | parkingd/SQLite |
| RGA 失败 | Camera transform 30 | parkingd/SQLite |
| DRM 失败 | Camera display 40，不进程内自主恢复 | parkingd/SQLite |
| IPC 断开 | stale/offline、重连、旧 request 隔离 | 各服务本地核心功能 |

## 20. 推荐代码结构

```text
inc/parking/
  access_request.hpp
  access_model.hpp
  card_id.hpp
  card_event_source.hpp
  fee_policy.hpp
  manual_card_server.hpp
  manual_control_protocol.hpp
  manual_card_source.hpp
  parking_coordinator.hpp
  parking_repository.hpp
  parking_session.hpp
  serial_rfid_source.hpp
  ipc_protocol.hpp
  rga_linear_buffer.hpp
  recognition_frame_pool.hpp
  overlay_renderer.hpp

src/parking/
  parkingd_main.cpp
  parkingctl_main.cpp
  parking_coordinator.cpp
  sqlite_parking_repository.cpp
  serial_rfid_source.cpp
  manual_card_server.cpp
  manual_control_protocol.cpp
  manual_card_source.cpp
  fee_policy.cpp
  ipc_protocol.cpp

src/parking_camera/
  parking_camera_main.cpp
  rga_linear_buffer.cpp
  recognition_frame_pool.cpp
  overlay_renderer.cpp

python/parking_lpr/
  service.py
  hyperlpr_adapter.py
  fake_lpr_service.py
  consensus.py
  ipc.py

python/parking_audio/
  service.py
  prerecorded_adapter.py
  remote_tts_adapter.py

config/parking/
  parking.ini
  fee.ini
  rfid.ini
  lpr.ini
  audio.ini

sql/
  001_parking_initial.sql

tests/parking/
  test_card_id.cpp
  test_fee_policy.cpp
  test_parking_coordinator.cpp
  test_sqlite_repository.cpp
  test_ipc_protocol.cpp
  scenarios/

tools/parking/
  cross_build_parking_rk3568.sh
  deploy_parking_rk3568.sh
  run_parking_rk3568.sh
  simulate_rfid_rk3568.sh
  query_parking_db_rk3568.sh
  backup_parking_db_rk3568.sh
```

所有 C++ 保持 ISO C++11，并遵守 [项目开发规范](development_guidelines.md)。脚本开头
写明用途、用法、前置条件、系统修改和恢复方法。

板端目录：

```text
/home/reynor/camera-project/
  bin/
  python/
  python-packages/
  models/
  config/
  scripts/
  share/

/userdata/parking-lot/
  db/
  images/
  logs/
  backups/
  state/
```

交叉编译产物继续通过 ADB 部署到 `/home/reynor` 下运行，不在板端直接编译。

### 20.1 配置示例

```ini
[site]
device_id=rk3568-parking-01
lane_id=lane-01
lane_direction=entry

[rfid.serial.entry0]
enabled=false
device=/dev/ttyS3
baud=9600
direction=entry
source_instance=entry-reader-0
card_present_debounce_ms=1000
response_timeout_ms=200

[manual_input]
enabled=true
socket=/run/parking-lot/control.sock
socket_mode=0660
allow_entry=true
allow_exit=true
allow_barrier_actuation=false
queue_capacity=16
rate_limit_per_second=5

[camera]
device=/dev/video0
drm_device=/dev/dri/card0
width=1920
height=1080
format=NV12
rotation=270
color_mode=bt709-limited

[lpr]
mode=fake
model_dir=/home/reynor/camera-project/models/hyperlpr3
burst_count=5
burst_interval_ms=200
request_timeout_ms=3000
minimum_votes=3
minimum_confidence_millionths=850000
roi_x=0.10
roi_y=0.25
roi_width=0.80
roi_height=0.55

[parking]
allow_unregistered_card=true
maximum_pending_requests=8

[fee]
policy_version=demo-v1
free_seconds=600
billing_unit_seconds=3600
unit_fee_cent=500
daily_cap_cent=5000

[audio]
enabled=true
mode=prerecorded
queue_capacity=16
play_timeout_ms=10000

[storage]
database=/userdata/parking-lot/db/parking.db
image_root=/userdata/parking-lot/images
log_root=/userdata/parking-lot/logs
minimum_free_mib=1024
```

程序启动时必须打印最终生效配置并严格验证范围。每个 RFID 串口源与手动输入源分别用
`enabled` 控制，允许同时为 `true`，不能再使用单个 `mode=serial|simulator` 让二者互斥。
LPR、Audio 仍可用 `mode` 选择对应 adapter。测试程序不能通过隐藏编译宏改变产品行为。
配置变更第一版通过受控重启生效，不在运行时热换模型、数据库和 fee policy。

当前没有 RFID 硬件时，保持上述 `serial.entry0.enabled=false`、
`manual_input.enabled=true`。读卡器到位后把串口源改为 `true` 即可；手动入口可以继续留作
联调，也可以在生产配置中关闭，业务代码不需要切换或重新编译。

## 21. 旧文件到新模块的迁移表

| 旧文件/模块 | 迁移结果 | 目标模块 |
| --- | --- | --- |
| `main.c` | 不复制进程实现，只保留服务编排语义 | BusyBox init/supervisor + service health |
| `RFID.c` | 重写协议解析、去抖和方向输入 | `SerialRfidSource` + `ManualCardSource` |
| `SQLite.c::carIn` | 重写为入场状态机和事务 | `ParkingCoordinator` + repository |
| `SQLite.c::carOut` | 重写为关闭 session，不 DELETE | `ParkingCoordinator` + repository |
| `SQLite.c::payment` | 保留“按时长计费”概念 | 版本化 `FeePolicy` |
| `SQLite.c::beep` | `/dev/beep` 不存在 | Null/Audio/GPIO BuzzerAdapter |
| `Audio.c` | 保留异步播报语义，废弃裸 UDP 文件协议 | `parking_audio_service` |
| `Video.c::display` | 完全废弃 | 当前 RGA + DRM 双缓冲 |
| `camera.c` | 完全废弃旧 V4L2/YUYV/CPU 转换 | `CaptureSession`/`V4L2BufferQueue` |
| `Video.c::takePhoto` | 重写为 request 驱动 burst | `RecognitionFramePool` |
| `system("./alpr")` | 完全废弃 | 长驻 HyperLPR3 adapter |
| `/tmp/fifo*`/`sem*` | 废弃 | `SOCK_SEQPACKET` + heartbeat |
| `info` 表 | 不直接复用 | versioned schema migration |
| 旧 `.so`/Android SDK/build cache | 不加入新构建 | 使用 AArch64 sysroot/离线依赖 |

## 22. 分阶段实施计划

### 阶段 0：冻结 Camera 基线

- 回归现有 `camera_display_stream`；
- 记录 1920x1080 NV12、FPS、RGA、page flip、L1/L2；
- 保证 AIparking 迁移前现有测试全部通过；
- 不改动 buffer 生命周期。

验收：现有实板 lifecycle 和 capture recovery 测试通过。

### 阶段 1：纯业务领域模型

- `CardId`；
- `AccessRequest`；
- Entry/Exit 状态机；
- `FeePolicy`；
- Fake repository；
- CardPresented/LPR/Audio 全部使用模拟事件。

验收：无需开发板即可确定性测试重复入场、非法出场、识别失败、计费和超时。

该阶段已经完成并通过主机测试和 RK3568 实板验证。

### 阶段 2：SQLite repository

- schema migration；
- prepared statements；
- ACTIVE/EXITED session；
- access event journal；
- request UUID 幂等；
- WAL/FULL/busy timeout/quick_check；
- 查询和 backup 工具。

验收：中文车牌无损；相同 request 重放一次；出场保留历史；异常中止后数据库可检查。

状态：0.15.0 已完成最小 v1 repository、prepared statements、ACTIVE 唯一约束、event
journal、WAL/FULL/busy timeout/quick_check，以及断进程重启恢复。查询/backup 专用工具、
中文真实 LPR 结果和 COMMIT 附近掉电注入仍属于后续工业化验收。

### 阶段 3：手动模拟与真实 RFID 双输入

- 实现 `parkingctl` 和 Unix `SOCK_SEQPACKET` 手动事件源；
- 再实现串口帧解析单元测试；
- 上板确认实际 reader、tty、协议、波特率、BCC；
- 完成卡在场去抖和拔插恢复；
- 用同一个 `poll()` loop 同时启用两个来源，并保留事件来源审计；
- 模拟事件默认不允许控制未来的真实道闸。

验收：手动源单独启用、串口源单独启用、二者同时启用均能工作；相同 event ID 重放不
产生第二条业务结果；真实和模拟的同卡并发事件受相同幂等/状态规则约束；同一张卡持续
放置只产生一次事件；坏 BCC 不进入业务状态机。

### 阶段 4：Fake LPR 闭环

- `parkingd`、Camera、fake LPR 的版本化 IPC；
- request ID、timeout、cancel；
- Camera 只在触发后选择新帧；
- 先用伪结果完成数据库和 overlay。

验收：LPR 延迟、错误、断开和迟到结果均不会串到下一个 request。

### 阶段 5：RecognitionFramePool

- 3 个独立 inference slots；
- RGA crop/rotate/scale/color conversion；
- fd 注册和 generation；
- no-free-slot 跳帧；
- 不改变现有 page flip 正常路径。

验收：LPR worker sleep/kill 时显示 FPS 不受明显影响，V4L2 buffer 不被长期占用。

### 阶段 6：HyperLPR3 实板兼容性

- 离线 wheel/model；
- 单图 probe；
- Camera 实拍图方向/颜色/ROI；
- 性能与内存；
- 多帧 consensus。

验收：通过 G1~G6 后替换 fake LPR，不同时改业务状态机。

### 阶段 7：完整入场

- RFID Entry；
- fresh burst；
- LPR consensus；
- durable DB commit；
- overlay；
- audio event。

验收：一次刷卡只建立一个 ACTIVE session；识别/数据库失败不伪报成功。

### 阶段 8：完整出场和计费

- Exit 查 active session；
- versioned FeePolicy；
- durable close；
- 费用 overlay/audio；
- 可选出口车牌核验。

验收：重复出场不二次收费；历史 session 不被删除；异常时间进入人工复核。

### 阶段 9：监督、运维和长稳

- BusyBox init/supervisor；
- restart budget/backoff/circuit breaker；
- SIGTERM 清理；
- 日志、WAL、图片轮转；
- 数据库 backup/restore；
- 24/72 小时 Camera+RFID+LPR+SQLite+Audio 长稳。

验收：无 fd/内存/存储无界增长，单域崩溃不拖垮 Camera 主路。

## 23. 测试矩阵

| 类别 | 用例 | 预期 |
| --- | --- | --- |
| Card | UID 有前导零 | 数据库规范值不丢失 |
| RFID | bad length/BCC/end marker | 丢弃并按原因计数 |
| RFID | 卡持续放置 10 秒 | 只产生一次 CardPresented |
| Manual input | 手动模拟入场/出场 | 进入与真实刷卡相同状态机并返回 ACK/FINAL |
| Manual input | 禁用后执行 parkingctl | socket 不存在，请求明确失败且 DB 不变 |
| Manual input | 非法卡号/方向/超长包 | 入口拒绝，进程继续运行 |
| Dual input | 真实与模拟同时触发同一卡 | 不建立两个 ACTIVE session，来源均可审计 |
| Dual input | 模拟事件请求物理放行 | 默认策略拒绝驱动 BarrierAdapter |
| Entry | 新卡+有效共识 | 建立一个 ACTIVE session |
| Entry | ACTIVE 卡再次刷入 | 拒绝，不请求/不提交第二次入场 |
| Entry | LPR 无结果 | 不建立 ACTIVE session |
| Entry | LPR 迟到 | 旧 request 结果被丢弃 |
| Entry | DB FULL/IOERR | 不报告 granted |
| Exit | 存在 ACTIVE session | 更新 EXITED 并保存费用 |
| Exit | 无 ACTIVE session | 拒绝且不创建假记录 |
| Exit | 重放同一 request | 不重复计费 |
| Fee | 免费/边界/跨日/上限 | 整数分结果符合 policy version |
| Frame | QBUF 后访问检测 | 设计上不存在 borrowed pointer 外逃 |
| Frame | inference slot 全忙 | 跳过识别帧，显示继续 |
| LPR | 3 正确+2 错误 | 正确候选达到共识 |
| LPR | worker SIGKILL | Camera 继续，generation 安全恢复 |
| Audio | TTS 超时 | DB session 不回滚，记录 AUDIO_FAILED |
| SQLite | 进程在 COMMIT 附近终止 | quick_check 通过或明确故障 |
| Camera | 现有 L1/L2 注入 | parkingd 保持运行 |
| DRM | display 40 | DB/RFID 继续，Camera 不局部重建 DRM |
| IPC | 重复/乱序/版本错误 | 幂等或拒绝，不串 request |
| Shutdown | SIGTERM | DB、socket、V4L2、DRM 按所有权清理 |
| Long run | 24/72 h | RSS/fd/WAL/图片/日志有界 |

## 24. 旧数据库数据迁移

如果仍有旧 `parking.db`，不要直接把旧 `info` 表改名为新表。先只读导出：

```text
旧字段：卡号、车牌、时间
  -> 验证卡号格式
  -> 验证 UTF-8 车牌
  -> 把时间解析为 UTC ms，并标记 time quality
  -> 为每行生成 legacy session UUID
  -> 导入为 ACTIVE 或 LEGACY_UNKNOWN 状态
  -> 保存 migration report
```

旧表没有出场历史，也没有 timezone/time quality，无法凭空恢复。迁移工具必须输出每行
成功、跳过或失败原因，原数据库保持只读备份。

如果没有需要保留的旧数据，仍使用 schema migration 创建新库，不在程序中散落
`CREATE TABLE` 和隐式版本判断。

## 25. 第三方代码、许可证和仓库整理

旧 HyperLPR 目录带 Apache-2.0 LICENSE；修改或分发时需要保留许可证和归属说明。语音
SDK 带独立商业/授权条款，不能因为文件在旧目录中就默认允许随产品重新分发。

当前 `AIparking/` 是未跟踪目录，并包含：

- 32 位 ARM 和 x86/x64 二进制；
- Android OpenCV SDK；
- 编译输出和 CMake cache；
- 第三方模型和字体；
- 语音 SDK 日志和资源。

不要直接执行 `git add -A` 把整个 205 MiB 历史目录提交到当前项目。推荐后续动作：

1. 把旧项目作为只读归档保留在仓库外或单独 legacy repository；
2. 当前仓库只提交重新实现的源码、schema、配置模板和必要许可证；
3. 模型/wheel/大文件使用明确的制品清单和哈希，不混入普通源码历史；
4. 删除/忽略第三方 build cache 前先确认旧资料已经备份；
5. 每项第三方依赖建立名称、版本、来源、许可证、SHA-256 清单。

## 26. 下一步开发建议

阶段 1、手动输入 3A 和 SQLite v1 已完成。下一步仍不要先改 Camera 主循环，也不要先
安装 HyperLPR3；应实现“阶段 3B：真实 RFID 输入边界”：

```text
serial frame parser + checksum tests
termios UART adapter
reader reconnect/backoff
card-present debounce
SerialRfidSource -> existing CardPresentedEvent
manual + serial dual-source tests
```

先在主机完成与硬件无关的 parser、坏帧、拆包/粘包和去抖测试，再根据真实读卡器确认
`tty`、波特率和校验。`SerialRfidSource` 与现有手动源复用同一个状态机和 SQLite 事务；
两种输入稳定后，再实现 Camera/Fake LPR IPC 和 `RecognitionFramePool`。这样不会一次性
混入 Camera、Python、模型、数据库、串口和音频而无法定位问题。
