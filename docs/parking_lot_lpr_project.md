# RK3568 Linux 停车场车牌识别与 SQLite 入库项目设计

## 1. 文档状态与最终架构

本文把当前 RK3568 摄像头显示基线重新用于一个单车道停车场入口终端：摄像头持续
采集并实时显示，后台使用 HyperLPR3 识别中国车牌，经过多帧确认后把车辆入场事件
写入板端 SQLite 数据库。

本文中的“已具备”表示已经在当前开发板或现有项目中确认；“计划实现”表示下一阶段
设计，尚不能当成已完成代码。

推荐的最终进程结构是：

```text
parking_camera_service（C++11，实时数据面）
  V4L2 持续采集
  RGA 实时显示转换
  DRM/KMS 双缓冲显示
  RGA 低频生成识别帧
  DMA-BUF/Unix socket 把识别帧交给 Python
             |
             v
parking_lpr_service（Python 3.8，业务数据面）
  HyperLPR3 + ONNX Runtime
  多帧车牌共识、去重和入场事件状态机
  SQLite 单写者队列
  结果回传给 C++ 叠加显示
             |
             v
/userdata/parking-lot/db/parking.db
```

采用两个进程而不是把 Python 嵌入 C++，是为了让推理、Python 包或 SQLite 故障不阻塞
V4L2/RGA/DRM 的实时显示。识别进程退出时，相机预览应继续；supervisor 可以单独重启
识别进程。

## 2. 项目范围

### 2.1 第一版功能

- `/dev/video0` 以 1920x1080 NV12、约 30 FPS 持续采集；
- 屏幕持续显示入口摄像头画面；
- 从连续视频中按策略抽取 2~5 FPS 给 HyperLPR3；
- 识别车牌号、置信度和车牌类型；
- 使用连续多帧结果确认同一辆车，而不是单帧直接入库；
- 每辆车只产生一条入场事件；
- SQLite 至少保存车牌号和入库时间，并保留识别审计字段；
- 可选保存确认时的整帧快照和车牌裁剪图；
- 屏幕可叠加车牌号、置信度、识别状态和数据库状态；
- 识别、数据库、相机和显示故障相互隔离并可诊断；
- 所有板端程序、脚本和配置部署在 `/home/reynor/parking-lot/`；
- 持久数据库和图片放在空间更大的 `/userdata/parking-lot/`。

### 2.2 第一版不做什么

- 不做出口计费、支付、道闸控制和剩余车位统计；
- 不做多车道、多摄像头关联；
- 不把每一帧识别结果都写数据库；
- 不在采集线程中同步执行 HyperLPR3；
- 不要求 HyperLPR3 达到摄像头 30 FPS；
- 不在板端运行 HyperLPR3 的 FastAPI/REST demo；
- 不在运行时从互联网下载模型；
- 不假设 RK3568 NPU 能直接执行 HyperLPR3 的 ONNX 模型；
- 不继续开发 DRM 域的进程内自主恢复；
- 不把识别率宣传为固定值，必须使用真实安装角度和现场数据验收。

## 3. 当前开发板能力盘点

2026-08-23 通过 ADB 只读检查得到：

| 能力 | 当前板端事实 |
| --- | --- |
| 架构/系统 | RK3568 AArch64，Buildroot，glibc 2.35 |
| Python | Python 3.8.6 |
| pip | pip 23.1.2 |
| NumPy | 1.18.2，可正常 import |
| OpenCV Python | `cv2` 4.5.5，可正常 import |
| OpenCV C++ | 4.5.5 动态库已安装，交叉 sysroot 也有头文件和库 |
| SQLite CLI | 3.21.0 |
| Python sqlite3 | 可用，绑定 SQLite 3.21.0 |
| SQLite C++ 开发文件 | 交叉 sysroot 中存在 `sqlite3.h` 和 `libsqlite3.so` |
| ONNX Runtime | 当前未安装 |
| MNN | 当前未安装 |
| 内存 | 约 4 GiB |
| `/userdata` | 约 50 GiB 可用，适合持久数据库和受控图片留存 |

这个环境已经具备 Python HyperLPR3 路线的大部分基础，但仍必须先验证 ONNX Runtime
AArch64 wheel、模型加载和单图推理，不能直接跳到连续视频集成。

## 4. HyperLPR3 的部署路线

### 4.1 官方项目给出的两条路线

[HyperLPR3 官方仓库](https://github.com/szad670401/HyperLPR)说明：

- Python 实现依赖 OpenCV 和 ONNX Runtime；
- C++ 实现与 Python 实现相互独立；
- C++ 实现要求 OpenCV 4 和 MNN 2；
- 官方声明 Linux 支持 Armv8，但当前仓库 CMake 中 `BUILD_LINUX_ARM64` 分支仍标有
  `TODO: Not implement`；
- Python `LicensePlateCatcher` 接收 OpenCV/NumPy 图像并返回识别结果；
- 支持单行蓝牌、单行黄牌、新能源和教练车，部分特殊/双层车牌支持有限。

因此本板第一版选择 Python/ONNX Runtime，C++/MNN 只保留为后续性能优化路线。

### 4.2 固定版本而不是安装“最新版”

建议兼容性闸门使用：

```text
Python             3.8.6（板端现有）
HyperLPR3          0.1.3
ONNX Runtime       1.14.0，CPython 3.8，AArch64 manylinux2014 wheel
OpenCV Python      4.5.5（板端现有）
```

HyperLPR3 0.1.3 的官方依赖清单固定了 ONNX Runtime 1.14.0 和 OpenCV 4.7，但在当前
板端不能让 pip 无条件替换系统 OpenCV。应先单独验证兼容组合，并用版本/哈希清单冻结
实际通过的 wheel。

不要执行无版本约束的：

```sh
pip install hyperlpr3
```

它可能升级 NumPy、安装另一套 `opencv-python`，或选择已经不支持 Python 3.8 的新
ONNX Runtime，破坏板端现有 OpenCV 环境。

### 4.3 安装隔离策略

不修改 Buildroot 系统 Python 目录。推荐按以下优先级：

1. 如果板端 `venv` 完整可用，创建
   `/home/reynor/parking-lot/venv`，并显式决定是否继承系统 `cv2`；
2. 若 Buildroot 缺少 venv 模块，使用 `pip --target`
   安装到 `/home/reynor/parking-lot/python-packages`；
3. 启动脚本设置固定 `PYTHONPATH`，不写 `/usr/lib/python3.8/site-packages`；
4. 依赖 wheel 在 WSL 下载和校验后通过 ADB 部署，板端不联网安装。

HyperLPR3 的 Python 包会在默认模型目录不存在时尝试联网下载模型。正式部署必须：

- 把 `20230229` 版本模型预先放入版本化目录；
- 保存每个 ONNX 模型的 SHA-256；
- 启动前检查模型存在、大小和哈希；
- 将模型目录显式传给识别器；
- 禁止运行期自动下载和模型静默替换。

建议模型目录：

```text
/home/reynor/parking-lot/models/hyperlpr3/20230229/onnx/
  y5fu_320x_sim.onnx
  y5fu_640x_sim.onnx
  rpv3_mdict_160_r3.onnx
  litemodel_cls_96x_r1.onnx
  SHA256SUMS
```

HyperLPR3 0.1.3 在 import 时会检查 `$HOME/.hyperlpr3/20230229`。启动脚本应使用专用
HOME，并让默认目录指向已经校验的离线模型，避免触发下载：

```text
HOME=/home/reynor/parking-lot/runtime-home
/home/reynor/parking-lot/runtime-home/.hyperlpr3
    -> /home/reynor/parking-lot/models/hyperlpr3
```

传给 `LicensePlateCatcher(folder=...)` 的是包含 `20230229` 子目录的
`/home/reynor/parking-lot/models/hyperlpr3`，不是更深一层的版本目录。

第一版使用低检测级别的 320x320 detector；只有 320 模型识别率不足且 CPU 预算允许
时才评估 640 detector。

### 4.4 上板兼容性闸门

进入实时开发前必须依次通过：

```text
G1  import numpy、cv2、onnxruntime、hyperlpr3
G2  创建 LicensePlateCatcher，全部模型离线加载成功
G3  对官方样例图识别，输出内容、UTF-8 和置信度正确
G4  对 100 张本地图片循环推理，无内存持续增长
G5  记录 320 detector 在 RK3568 上的 P50/P95/P99 推理耗时
G6  使用实际摄像头保存的图片验证方向、颜色和车牌像素尺寸
```

只要 G1~G3 不通过，就不修改当前实时 pipeline。先解决依赖和模型问题。

## 5. 图像数据路径

### 5.1 实时显示主路

主路沿用当前已经验证的实现：

```text
Sensor -> ISP -> V4L2 NV12 DMA-BUF
       -> RGA 旋转/颜色转换
       -> XRGB8888 DRM framebuffer A/B
       -> legacy page flip -> VOP -> DSI -> LCD
```

显示主路始终优先，目标约 30 FPS。HyperLPR3 再慢也不得让采集队列、RGA 显示转换或
DRM page flip 等待它。

### 5.2 低频识别支路

```text
选中的 V4L2 frame（仍归应用）
       |
       | 第二次同步 RGA 作业，裁剪/旋转/缩小
       v
Inference buffer，BGRX8888，CPU 可 mmap
       |
       | Unix socket 通知 slot ready
       v
Python mmap -> NumPy view -> 连续 BGR 输入 -> HyperLPR3
```

选择一张帧做识别时，必须在 `VIDIOC_QBUF` 之前完成显示 RGA 和识别 RGA。两个 RGA
作业返回后，硬件才不再读取源 V4L2 buffer，随后才能归还给 ISP。

识别支路不能直接长期持有 V4L2 DMA-BUF，否则会减少 ISP 可用 buffer，最终令采集
断流。也不能把当前 DRM 扫描 framebuffer 直接交给 Python 后立即复用，因为 Python
尚未完成读取时 RGA 可能覆盖它。

### 5.3 为什么使用专用 inference buffer

准备 3 个较小的 inference buffer：

```text
Free -> RgaWriting -> ReadyForLpr -> LprReading -> Free
```

- C++ 通过 DRM dumb/GEM 创建可 mmap 的线性 buffer；
- 导出 DMA-BUF fd 给 RGA；
- 启动 IPC 时使用 `SCM_RIGHTS` 把 fd 副本发送给 Python 一次；
- 后续只发送 slot、generation、尺寸、stride、timestamp 等小消息；
- Python 完成推理后返回 `FRAME_DONE`，C++ 才能复用该 slot；
- IPC 断开时所有 `LprReading` slot 隔离，重新建立 worker 后再重置整个推理 pool。

初始识别分辨率由实拍标定决定。建议先测试正确方向下的 960x540；如果当前物理安装
必须旋转 270°，则保持相同比例改成 540x960。不要为了匹配数字强行拉伸图像。

BGRX 到 HyperLPR3 所需连续 BGR 可能产生一次识别侧 CPU copy。以 3 FPS 和缩小后的
ROI 计算，这个开销可接受，并且不会影响 30 FPS 显示。优化前必须先测量，不能为了
追求“零拷贝”引入错误的 stride 或跨进程 buffer 所有权。

### 5.4 ROI 与摄像头安装

停车场识别效果首先由光学和安装决定：

- 摄像头固定，避免自动旋转方向不确定；
- 车牌进入图像时应有足够像素宽度；
- 避免车灯直射、逆光和强反光；
- 入口单车道设置识别 ROI，排除邻道、广告牌和远处车辆；
- 保留完整原始图验证曝光、白平衡和运动模糊；
- ROI 使用归一化坐标配置，不硬编码到算法中。

HyperLPR3 返回的 box 位于 inference buffer 坐标系。叠加到屏幕前必须依次逆变换：

```text
HyperLPR box
  -> 加回 ROI offset
  -> 逆缩放
  -> 应用/逆应用旋转关系
  -> 映射到 DRM 显示坐标
```

## 6. 采样和背压策略

### 6.1 不对每一帧推理

当前 RK3568 性能必须通过实测得到，不能引用桌面 Intel 的官方 benchmark。第一版采用
自适应采样：

| 状态 | 建议推理频率 | 目的 |
| --- | ---: | --- |
| 没有候选车牌 | 2~3 FPS | 降低 CPU 占用 |
| 正在确认候选 | 5 FPS | 尽快取得多帧共识 |
| 已经确认、车辆仍未离开 | 1 FPS | 只用于判断目标消失 |
| inference worker 忙 | 0 FPS 新提交 | 跳过旧帧，不排队 |

### 6.2 latest-frame-wins

识别队列深度固定为 1 个待处理任务，inference pool 建议 3 个 slot：

- 有空闲 slot 且到达采样时间时才生成识别帧；
- 没有空闲 slot 就增加 `lpr_frames_skipped_busy`，显示主路继续；
- 已经有一个 Ready 帧尚未开始推理时，可用更新帧替换旧 Ready 帧；
- 不允许排队积累历史视频；
- 数据库写入也使用有界队列，不能无限占用内存。

这种策略牺牲无必要的中间帧，确保识别结果对应较新的现场画面。

## 7. 车牌确认和防重复入库

### 7.1 为什么单帧结果不能直接入库

单帧可能因运动模糊、反光或汉字相似而识别错误。如果每帧直接 `INSERT`：

- 同一辆车会生成几十条记录；
- 错一个字符就会被当成另一辆车；
- 停在入口的车辆会不断重复入库。

因此数据库接收的是“确认后的车辆入场事件”，而不是原始模型输出。

### 7.2 单车道事件状态机

```text
Searching
    |
    | ROI 内出现合法候选
    v
Tracking
    |
    | 2 秒窗口内同一号码至少 3 次，平均置信度达标
    v
Confirmed ---- 写 SQLite 一次 ----> Suppressed
    ^                                  |
    | 新结果继续属于同一目标            | 连续 3 秒没有目标
    +----------------------------------+
                                       v
                                    Searching
```

建议初始参数：

| 参数 | 初始值 |
| --- | ---: |
| 共识窗口 | 2 s |
| 最少一致识别次数 | 3 |
| 单帧文字置信度下限 | 0.80 |
| 确认平均置信度下限 | 0.85 |
| 目标消失重新布防 | 3 s |
| 同车牌最小再次入场间隔 | 60 s，可配置 |

这些数值必须用现场数据标定。

### 7.3 候选归一化

每个候选保留：

- `plate_raw`：HyperLPR3 原始 UTF-8 文本；
- `plate_number`：去除无意义空白、统一字母大小写后的规范文本；
- 车牌类型；
- 文本置信度和 detector 置信度；
- box、frame sequence、capture timestamp；
- 模型版本。

不要用过于严格的正则表达式提前丢弃新能源或特殊车牌。第一版可以把格式可疑但高
置信度的结果标记为 `review`，而不是伪装成完全可靠的确认结果。

多车牌同时出现时，单车道版本只选择：box 中心位于入口 ROI、面积较大且综合置信度
最高的候选。多车道必须使用独立 ROI/tracker，不能复用这个简化策略。

### 7.4 幂等性

`Confirmed` 状态只生成一次不可变 `event_uuid`。数据库对 `event_uuid` 建 UNIQUE
约束，即使 IPC 重传或数据库 worker 重试，也只会保留一条入场事件。

事件 UUID 应由设备 ID、启动 ID、单调计数器和随机熵组成，不能只用车牌号；同一辆车
以后合法再次入场必须能生成新事件。

## 8. SQLite 数据库设计

### 8.1 数据存放位置

```text
/userdata/parking-lot/
  db/
    parking.db
    parking.db-wal
    parking.db-shm
  images/
    2026/08/23/<event_uuid>.jpg
    2026/08/23/<event_uuid>-plate.jpg
  backups/
  logs/
```

数据库不能放 `/tmp`，因为它是 tmpfs；也不建议持续写只有约 5.9 GiB 的根分区。

### 8.2 表结构

SQLite 使用 UTF-8 TEXT，可以直接保存中文省份字符。建议第一版 schema：

```sql
CREATE TABLE IF NOT EXISTS schema_meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS entry_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_uuid TEXT NOT NULL UNIQUE,

    plate_number TEXT NOT NULL,
    plate_raw TEXT NOT NULL,
    plate_type TEXT NOT NULL DEFAULT 'unknown',

    entry_time_utc_ms INTEGER NOT NULL,
    timezone_offset_minutes INTEGER NOT NULL,
    time_quality TEXT NOT NULL
        CHECK (time_quality IN ('synchronized', 'holdover', 'unsynchronized')),
    capture_monotonic_ns INTEGER NOT NULL,

    text_confidence REAL NOT NULL
        CHECK (text_confidence >= 0.0 AND text_confidence <= 1.0),
    detection_confidence REAL,
    confirmation_count INTEGER NOT NULL CHECK (confirmation_count > 0),
    confirmation_window_ms INTEGER NOT NULL CHECK (confirmation_window_ms >= 0),

    camera_id TEXT NOT NULL,
    lane_id TEXT NOT NULL,
    frame_sequence INTEGER NOT NULL,
    image_path TEXT,
    plate_image_path TEXT,

    model_name TEXT NOT NULL,
    model_version TEXT NOT NULL,
    recognition_status TEXT NOT NULL
        CHECK (recognition_status IN ('confirmed', 'review', 'manual')),

    created_at_utc_ms INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_entry_events_time
    ON entry_events(entry_time_utc_ms DESC);

CREATE INDEX IF NOT EXISTS idx_entry_events_plate_time
    ON entry_events(plate_number, entry_time_utc_ms DESC);
```

其中真正满足用户需求的核心字段是：

- `plate_number`：规范化车牌号；
- `entry_time_utc_ms`：入库时间的唯一规范值。

其他字段用于排查误识别、模型升级、图片关联和时间可信度。不要同时维护多个互相可能
矛盾的“标准时间字符串”；界面查询时再把 UTC 毫秒转换为本地 ISO-8601 文本。

### 8.3 时间处理

停车场业务时间和内部超时使用不同的时钟：

| 用途 | 时钟 |
| --- | --- |
| 入库时间 | `CLOCK_REALTIME` 的 UTC 毫秒 |
| 识别窗口、目标消失、重试退避 | `CLOCK_MONOTONIC` |

板端未完成 NTP/gPTP/RTC 同步时仍可记录事件，但 `time_quality` 必须是
`unsynchronized`。不能把 1970 年或明显跳变的时间默认为可信入库时间。

### 8.4 SQLite 运行参数

数据库启动后验证每条 PRAGMA 的返回值：

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=3000;
PRAGMA wal_autocheckpoint=100;
```

选择 WAL 是为了让查询工具读取历史记录时不阻塞单一写者；SQLite WAL 仍然只允许一个
writer。入场频率很低，使用 `synchronous=FULL` 的提交开销可以接受，并比 NORMAL 更
重视突然断电时的事务持久性。

数据库只允许 `parking_lpr_service` 拥有写连接。CLI、导出和 UI 使用短只读事务，不能
保持长事务导致 checkpoint 饥饿。

### 8.5 写入事务

数据库 writer 使用准备好的参数化语句，不能拼接车牌字符串：

```sql
BEGIN IMMEDIATE;

INSERT OR IGNORE INTO entry_events (
    event_uuid,
    plate_number,
    plate_raw,
    plate_type,
    entry_time_utc_ms,
    timezone_offset_minutes,
    time_quality,
    capture_monotonic_ns,
    text_confidence,
    detection_confidence,
    confirmation_count,
    confirmation_window_ms,
    camera_id,
    lane_id,
    frame_sequence,
    image_path,
    plate_image_path,
    model_name,
    model_version,
    recognition_status,
    created_at_utc_ms
) VALUES (
    ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
);

COMMIT;
```

提交后必须检查 SQLite result code 和 `sqlite3_changes()`：

- changes=1：首次持久化成功；
- changes=0：相同 `event_uuid` 已存在，属于幂等重试；
- `SQLITE_BUSY`：在 busy timeout 后进入有界重试；
- `SQLITE_FULL/IOERR/CORRUPT`：进入 StorageDegraded，不能宣称已经入库。

### 8.6 图片与数据库的一致性

只为确认事件保存图片，不保存每次推理帧：

1. 在 `/userdata/parking-lot/images/...` 写临时文件；
2. `fsync` 文件并原子 rename 到最终事件路径；
3. 数据库事务插入最终相对路径；
4. 数据库插入失败时保留短期 orphan 清理标记；
5. 定期删除数据库未引用、超过宽限期的孤立图片。

如果产品不需要图片审计，可把保存策略设为 `none`，显著减少隐私风险和 eMMC 写入。

### 8.7 备份和恢复

WAL 模式下不能在数据库打开时只复制 `parking.db`，否则可能漏掉仍在 `-wal` 中的已提交
事务。使用 SQLite backup API 或在受控停止/完成 checkpoint 后备份。

启动自检包括：

```sql
PRAGMA quick_check;
```

失败时把数据库标为损坏并报警，不应自动删除原库重新创建。自动删除会让现场数据无法
恢复。

## 9. 进程、线程和 IPC

### 9.1 C++ 相机进程

`parking_camera_service` 唯一拥有：

- V4L2 fd、CaptureSession 和 capture buffers；
- RGA 显示/识别作业；
- DRM fd、CRTC 和 display framebuffers；
- inference DMA-BUF pool；
- 与识别进程的 Unix socket。

它不链接 Python、ONNX Runtime 或 SQLite，不执行数据库写入。

### 9.2 Python 识别进程

`parking_lpr_service` 包含：

- IPC receiver 和 inference slot 映射；
- 单个长期存在的 HyperLPR3 catcher，模型只加载一次；
- plate tracker/consensus 状态机；
- 有界数据库写队列；
- 单一 SQLite writer thread；
- 结果回传和健康心跳。

不要每帧创建 `LicensePlateCatcher`，否则会反复加载多个模型并造成巨大延迟和内存抖动。

### 9.3 IPC 协议

使用 Unix `SOCK_SEQPACKET`：

- 消息边界由内核保留；
- 初始化时可用 `SCM_RIGHTS` 传 DMA-BUF fd；
- 协议有 magic、version、message type 和 payload length；
- 所有整数明确 little-endian；
- UTF-8 字符串使用长度前缀；
- 不直接发送带编译器 padding 的 C/C++ struct。

消息至少包括：

```text
HELLO / HELLO_ACK
REGISTER_BUFFER(slot, fd, width, height, stride, format)
FRAME_READY(slot, generation, sequence, monotonic_ns, realtime_utc_ms)
FRAME_DONE(slot, generation)
LPR_RESULT(sequence, plate, confidence, box, state)
ENTRY_COMMITTED(event_uuid, database_id)
HEARTBEAT
SHUTDOWN
```

每个 slot 带 generation，识别 worker 重启后迟到的旧 `FRAME_DONE` 不能释放新一代
buffer。

## 10. 屏幕显示和叠加

相机画面继续全帧显示。识别结果是低频的，叠加层使用“最新有效结果”：

- box 和文字带结果对应的 frame sequence；
- 结果年龄超过 1 秒则不再画 box，避免框住已经离开的车辆；
- Tracking 显示黄色；
- Confirmed 且数据库提交成功显示绿色和记录 ID；
- 识别可用但数据库失败显示红色 `DB ERROR`；
- LPR worker 离线显示 `LPR OFFLINE`，相机画面继续；
- DRM 失败按现有 display 域退出 40，不进行进程内 DRM 重建。

可使用板端已有 OpenCV/freetype 在未扫描的 framebuffer 上绘制。写入顺序必须是：

```text
RGA 完成目标 framebuffer
  -> CPU/OpenCV 绘制 overlay
  -> DRM page flip
```

CPU 只能修改当前不被 VOP 扫描的 framebuffer。

## 11. 故障隔离

| 故障 | 系统行为 |
| --- | --- |
| 单个坏相机帧 | 丢帧并继续 |
| V4L2 连续超时 | 使用已有 L1/L2 有界恢复 |
| HyperLPR3 单次异常 | 放弃该帧，计数并继续 |
| LPR worker 崩溃 | C++ 继续显示，隔离推理 slot，supervisor 有界重启 worker |
| 模型缺失/哈希错误 | LPR 不启动，禁止联网下载，显示 LPR OFFLINE |
| SQLite BUSY | 有界退避重试，不阻塞 inference/display |
| SQLite FULL/IOERR | StorageDegraded，报警，不伪报入库成功 |
| 数据库损坏 | 隔离原库，停止写入，保留现场文件 |
| 图片保存失败 | 按配置决定事件可无图入库或标记 review |
| RGA 失败 | transform 域退出或由产品 supervisor 受控重启 camera worker |
| DRM flip 失败 | 退出 40，零次 DRM 自主恢复 |

数据库队列例如固定 64 个确认事件。队列满时停止接受新的“已持久化”承诺并报警；不能
无限占内存，也不能静默丢掉事件。

## 12. 配置建议

配置文件建议放在：

```text
/home/reynor/parking-lot/config/parking-lot.conf
```

至少包含：

```ini
[camera]
device=/dev/video0
width=1920
height=1080
format=NV12
rotation=270
color_mode=bt709-limited

[display]
device=/dev/dri/card0
confirm_desktop_stopped=true

[lpr]
enabled=true
model_dir=/home/reynor/parking-lot/models/hyperlpr3
model_version=20230229
detector_level=low
idle_fps=3
tracking_fps=5
confirmed_fps=1
text_confidence_min=0.80
consensus_confidence_min=0.85
consensus_count=3
consensus_window_ms=2000
target_lost_ms=3000
minimum_reentry_ms=60000
roi_x=0.10
roi_y=0.25
roi_width=0.80
roi_height=0.55

[site]
camera_id=entrance-camera-01
lane_id=entrance-lane-01

[storage]
database=/userdata/parking-lot/db/parking.db
save_snapshot=confirmed
image_root=/userdata/parking-lot/images
image_retention_days=30
event_retention_days=180
minimum_free_mib=1024
```

程序启动时解析并严格校验配置，之后打印最终生效值。运行期间不隐式热更新模型、ROI
和 schema；配置变更通过受控重启生效。

## 13. 可观测性与查询

### 13.1 指标

至少记录：

- capture/display FPS；
- V4L2 timeout、L1/L2 恢复次数；
- inference submitted/completed/skipped-busy；
- HyperLPR P50/P95/P99 耗时；
- 当前 inference queue 深度；
- candidate/confirmed/rejected 数；
- database committed/duplicate/retry/failure 数；
- 最后一条成功入库时间；
- 数据库、WAL、图片目录大小和剩余磁盘；
- LPR worker 心跳和模型版本；
- 最后一次 DRM flip 时间。

### 13.2 查询示例

最近 20 辆车：

```sql
SELECT
    id,
    plate_number,
    datetime(entry_time_utc_ms / 1000, 'unixepoch', 'localtime') AS entry_time,
    text_confidence,
    recognition_status
FROM entry_events
ORDER BY entry_time_utc_ms DESC
LIMIT 20;
```

查询指定车牌：

```sql
SELECT id, plate_number, entry_time_utc_ms, image_path
FROM entry_events
WHERE plate_number = ?
ORDER BY entry_time_utc_ms DESC;
```

实际程序必须使用绑定参数；上面的 `?` 不能通过字符串替换实现。

## 14. 隐私、存储和运维

车牌号和车辆图片属于需要谨慎处理的现场数据：

- 数据库文件权限建议 0600，目录 0700；
- 不默认开放 HyperLPR3 REST 服务；
- 不把数据库或图片写入项目 Git；
- 日志尽量对车牌脱敏，例如只显示前两位和后两位；
- 图片和事件分别配置保留期限；
- 清理任务只删除数据库明确允许过期的数据；
- 磁盘不足阈值提前报警，不等到 SQLite 返回 FULL；
- 导出数据库需要身份验证和审计；
- 模型、schema 和配置都有版本记录。

每日低峰期执行 retention 和 WAL checkpoint。清理必须使用事务，并限制单批删除量，
避免一次删除大量记录造成长写事务和巨大的 WAL。

## 15. 推荐目录

仓库内建议：

```text
inc/parking/
  inference_buffer_pool.hpp
  lpr_ipc_protocol.hpp
  lpr_result.hpp
  overlay_renderer.hpp

src/parking/
  parking_camera_main.cpp
  inference_buffer_pool.cpp
  lpr_ipc_protocol.cpp
  overlay_renderer.cpp

python/parking_lpr/
  service.py
  hyperlpr_adapter.py
  plate_tracker.py
  database.py
  ipc.py
  time_source.py

sql/
  001_initial.sql

tests/
  parking/

tools/parking/
  deploy_parking_lot_rk3568.sh
  run_parking_lot_rk3568.sh
  query_entries_rk3568.sh
  backup_parking_db_rk3568.sh
```

板端建议：

```text
/home/reynor/parking-lot/
  bin/
  python-packages/
  python/
  models/
  config/
  scripts/
  logs -> /userdata/parking-lot/logs
```

所有脚本开头必须写清楚用途、用法、前置条件、是否修改系统状态以及恢复方法，继续遵守
现有 [开发规范](development_guidelines.md)。

## 16. 分阶段实施

### 阶段 0：HyperLPR3 板端兼容性验证

实现和验收：

1. 在隔离目录准备固定 wheel 和模型；
2. 通过 G1~G6；
3. 输出依赖版本、模型 SHA-256、单图结果和推理耗时；
4. 使用从当前摄像头保存的 20~100 张图片验证方向和 ROI；
5. 不修改实时显示代码。

这是下一次最应该先做的任务。

### 阶段 1：SQLite repository

实现：

- schema migration；
- SQLite RAII/封装或 Python repository；
- prepared statement 和幂等插入；
- WAL/FULL/busy timeout；
- 查询 CLI；
- 突然终止后的 quick_check 回归。

验收：重复写同一 UUID 只有一条记录，中文车牌往返无损，断电风险测试后数据库一致。

### 阶段 2：离线多帧车牌共识

实现：

- 输入一个图片目录模拟连续帧；
- Tracking/Confirmed/Suppressed 状态机；
- 置信度阈值、目标消失和最小再次入场间隔；
- 确认后 SQLite 入库一次。

验收：同车 30 张图只生成一条记录，单帧误识别不入库，两辆不同车生成两条记录。

### 阶段 3：C++ inference buffer 与 IPC

实现：

- 三槽 inference DMA-BUF pool；
- RGA 裁剪/旋转/缩放；
- `SOCK_SEQPACKET + SCM_RIGHTS`；
- generation 和 worker 重连；
- latest-frame-wins。

验收：Python 可稳定读取正确颜色/方向的帧，worker 人为变慢不降低显示 FPS。

### 阶段 4：连续识别与入库

实现：

- HyperLPR3 长期 context；
- 自适应 3/5/1 FPS；
- tracker 与数据库 writer；
- 结果回传。

验收：真实车辆/打印测试牌连续通过入口，确认事件唯一，识别 worker 崩溃不影响显示。

### 阶段 5：屏幕叠加和故障显示

实现：

- box 坐标变换；
- UTF-8 车牌和状态 overlay；
- LPR OFFLINE、DB ERROR、StorageDegraded；
- 结果年龄限制。

验收：overlay 不修改正在扫描的 framebuffer，不出现撕裂或历史框长期残留。

### 阶段 6：守护、留存和长稳

实现：

- BusyBox init/supervisor；
- camera 和 LPR worker 分别有界重启；
- database backup、retention、checkpoint；
- 磁盘和心跳监控；
- 24/72 小时长稳测试。

验收：无 fd/内存/WAL 无界增长，数据库可备份恢复，异常退出不会产生重复入场事件。

## 17. 完整测试矩阵

| 类别 | 用例 | 预期 |
| --- | --- | --- |
| 模型 | 官方样例图 | 输出预期车牌和类型 |
| 方向 | 0/90/180/270 | 只在正确配置下进入正式运行 |
| 颜色 | NV12 转 BGRX/BGR | 无明显偏色，识别结果稳定 |
| 共识 | 2 正确 + 1 错误 | 未达到策略则不入库 |
| 共识 | 3+ 一致高置信度 | 入库一次 |
| 停留 | 同车停留 30 s | 不重复入库 |
| 重现 | 车辆离开后 60 s 再进入 | 产生新事件 |
| 多候选 | ROI 内外各一辆车 | 只跟踪入口 ROI 目标 |
| 推理慢 | worker sleep 1 s | 显示继续，识别帧被有界丢弃 |
| worker 崩溃 | SIGKILL Python | 显示继续，重启后 generation 安全 |
| SQLite 重复 | 重放同一 UUID | 数据库仍只有一条 |
| SQLite busy | 人为持有写锁 | 有界重试，不阻塞显示 |
| 磁盘满 | 限制测试分区 | StorageDegraded，不伪报成功 |
| 数据库恢复 | 写入期间杀进程 | 启动 quick_check 通过或明确隔离 |
| V4L2 L1/L2 | 现有注入 | 识别暂停后继续，DRM 不重建 |
| DRM 失败 | page flip 失败 | camera worker 退出 40，不自主恢复 |
| 生命周期 | SIGTERM | IPC、SQLite、V4L2、DRM 按序清理 |
| 长稳 | 24/72 h | 资源稳定、事件唯一、WAL 有界 |

## 18. 下一步建议

下一次不要先改实时 pipeline，而是完成一个最小的 `hyperlpr_board_probe.py`：

```text
输入：一张板端 JPEG/PNG
输出：依赖版本、模型版本、车牌、类型、置信度、box、推理耗时
退出码：成功/无车牌/依赖错误/模型错误/推理错误
```

把官方样例和真实摄像头图片都跑通并记录 RK3568 性能后，才能合理确定识别分辨率、
采样 FPS、线程数和 detector level。完成这个兼容性闸门，再开发 SQLite repository，
最后接入持续采集，是风险最低且最容易定位问题的实施顺序。
