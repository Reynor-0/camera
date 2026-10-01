# AIparking 迁移实施进度

## 1. 文档用途

本文记录 [AIparking 业务逻辑迁移方案](aiparking_logic_migration_plan.md) 的真实实施状态、
验证证据和下一步边界。设计文档描述目标架构；本文只有经过代码、主机测试和 RK3568
实板验证的能力才标为完成。

最后更新：2026-09-17。

## 2. 总体状态

| 阶段 | 状态 | 当前结果 |
| --- | --- | --- |
| 0. Camera 显示基线 | 已完成 | 沿用 0.13.0 的 V4L2/RGA/DRM 实板链路 |
| 1. C++11 业务领域模型 | 已完成 | 入场、出场、计费、幂等及内存 repository 单测 |
| 2. SQLite repository | 已完成 | 0.15.0 已实现持久 session、event journal 和重启恢复 |
| 3A. 手动模拟输入 | 已完成 | `parkingctl` 经 Unix socket 实板闭环 |
| 3B. 真实 RFID 输入 | 未开始 | 等待读卡器型号、串口和帧协议确认 |
| 4. Fake LPR IPC 闭环 | 未开始 | 当前 Fake LPR 仍在 `parkingd` 进程内 |
| 5. RecognitionFramePool | 未开始 | 尚未从 Camera 主路取识别帧 |
| 6. HyperLPR3 | 未开始 | 板端依赖和模型尚未部署 |

当前不是完整停车场产品。它已经是一个可重启、可审计的停车业务控制面基线，但车牌仍
是固定 Fake 结果，也没有真实 RFID、道闸、音频或 Camera 抓拍 IPC。

## 3. 当前可运行架构

```text
parkingctl
  -> PARKING/1 over Unix SOCK_SEQPACKET
  -> parkingd
       -> CardId / ParkingCoordinator / FeePolicy
       -> FakePlateRecognizer（固定结果）
       -> SQLiteParkingRepository（单 writer）
            -> parking_sessions
            -> access_events
            -> schema_meta
```

Camera 的 V4L2 -> DMA-BUF -> RGA -> DRM/KMS 显示链路没有被本阶段修改，停车控制面也
不会启动、停止或占用 Camera/DRM 设备。

## 4. 阶段 1：领域模型与状态机

已经实现：

- `CardId` 十六进制规范化，保留前导零；
- `CardPresentedEvent` 统一真实/模拟来源；
- 新卡入场、ACTIVE 卡重复入场、正常出场和无记录出场；
- event ID 重放幂等和同 ID 不同 payload 冲突；
- 固定结果 Fake LPR adapter；
- 整数分 `FeePolicy`；
- 测试用 `InMemoryParkingRepository`；
- repository 事务接口及回滚语义。

`parking_domain` 不依赖 V4L2、DRM、SQLite 或 Python。事件来源与业务规则分离，因此后续
串口 RFID 必须复用同一个 `ParkingCoordinator`，不能另写一套入出场逻辑。

## 5. 阶段 3A：手动模拟控制面

已经实现：

- `/run/parking-lot/control.sock` Unix `SOCK_SEQPACKET`；
- `PARKING/1` 版本化 tab-separated 单包协议；
- `SO_PEERCRED` 记录客户端 pid/uid/gid；
- 1024 字节消息上限和 2 秒无请求超时；
- ACK 表示请求格式与入口已接受，FINAL 表示最终业务结果；
- SIGINT、SIGTERM、SIGHUP 优雅退出和 socket 清理；
- `parkingd`、`parkingctl` 的 `--help`、`--version`；
- 交叉构建、白名单 ADB 部署和实板闭环。

手动输入不是测试旁路。它生成 `source=manual_simulator` 的正式事件，走与未来真实 RFID
相同的校验、幂等、状态机、事务和审计路径。

## 6. 阶段 2：SQLite 持久化

### 6.1 运行参数和文件

`parkingd` 默认数据库：

```text
/userdata/parking-lot/db/parking.db
```

也可用绝对路径隔离开发或验收数据库：

```sh
/home/reynor/camera-project/bin/parkingd \
    --database /userdata/parking-lot/db/parking-validation.db \
    --fake-plate TEST3568
```

程序创建父目录为 `0750`，数据库文件为 `0640`。构造 repository 时设置并验证：

```text
journal_mode=WAL
synchronous=FULL
foreign_keys=ON
busy_timeout=3000 ms
wal_autocheckpoint=100 pages
quick_check=ok
```

### 6.2 v1 Schema 的职责

`schema_meta` 保存 `schema_version=1`。

`parking_sessions` 保存一辆车的一次完整停车历史：

- `session_id` 和建立它的 `entry_event_id`；
- 卡号、车牌、识别置信度、入场来源和 UTC 时间；
- `active=1/0`；
- 出场 event、来源、UTC 时间、停车秒数和整数分费用。

部分唯一索引保证同一卡号最多只有一条 `active=1` 的 session。出场使用 UPDATE 关闭，
不 DELETE 历史。

`access_events` 保存每次已经形成最终结果的输入：

- 全局唯一 `event_id`；
- 不含接收时间的稳定 payload fingerprint；
- 卡号、方向、来源和两类接收时间；
- outcome、reason、session、车牌、时长和费用。

因此成功、业务拒绝和出场结果都能在重启后原样回放。使用同一 event ID 但更换卡号、
方向或来源时返回 `EVENT_ID_CONFLICT`，不会复用旧授权。

### 6.3 原子事务边界

入场识别不持有数据库写锁：

```text
检查 ACTIVE
-> Fake/未来 LPR 识别（事务外）
-> BEGIN IMMEDIATE
-> 再检查 ACTIVE
-> INSERT parking_sessions
-> INSERT access_events
-> COMMIT
```

出场在一个事务中 UPDATE session 并 INSERT event。任一步失败都 ROLLBACK，因此不会出现
“车辆已经入场但 event 未记下”或“event 显示成功但 session 没有写入”的半状态。

session ID 使用 `session-<entry_event_id>`，不依赖进程内计数器，重启后不会与已有记录
碰撞。repository 继续采用单连接、单 writer 模型；当前 `parkingd` 单线程顺序处理请求。

### 6.4 构建依赖隔离

RK3568 交叉构建从 Buildroot sysroot 获取 `sqlite3.h` 和 AArch64 `libsqlite3.so`。当前 WSL
只有 x86_64 SQLite runtime，没有宿主机开发头文件，因此 CMake 只把单独的 `sqlite3.h`
复制到构建目录，不把整个 ARM sysroot include 目录加入宿主机构建。这样可避免 ARM 的
`sys/stat.h` 等 ABI 头污染 x86_64 编译。

## 7. 自动化测试

主机严格构建当前有 3 个 CTest：

```text
parking_domain
parking_control
parking_sqlite
```

SQLite 测试使用真实临时数据库，并验证：

- 首次入场写入一条 ACTIVE session；
- 销毁 repository、重新打开后 session/history 仍存在；
- 重放入场不再次调用 Fake LPR；
- 同 event ID 不同 payload 返回冲突；
- 新 event 的同卡入场返回 `DUPLICATE_ENTRY`；
- 出场关闭 ACTIVE 并持久化费用；
- 成功与拒绝结果都能跨 reopen 回放；
- 显式 ROLLBACK 不留下 session；
- 每次检查 `PRAGMA quick_check`。

2026-09-17 结果：GCC 13、ISO C++11、`-Werror` 构建通过，CTest 3/3 通过。主机真实
`parkingd/parkingctl` 还执行了断进程重启：第二次启动恢复 `sessions=1 active=1`，旧入场
返回原车牌且 `replayed=true`，随后出场恢复为 `active=0`。

## 8. RK3568 实板验收

部署位置遵守项目规范：

```text
/home/reynor/camera-project/bin/parkingd
/home/reynor/camera-project/bin/parkingctl
```

实板版本与依赖：

```text
parkingd 0.15.0 (AArch64/RK3568, protocol PARKING/1)
parkingctl 0.15.0 (AArch64/RK3568, protocol PARKING/1)
libsqlite3.so.0 => /usr/lib64/libsqlite3.so.0
SQLite runtime 3.21.0
```

2026-09-17 使用独立验收库
`/userdata/parking-lot/db/parking-v015-validation-20260917.db`，结果如下：

| 操作 | 结果 |
| --- | --- |
| `board-v015-entry-1` 首次入场 | `ENTRY_GRANTED`，车牌 `BOARD123` |
| 停止并重新启动 `parkingd` | 启动恢复 `sessions=1 active=1` |
| 重放 `board-v015-entry-1` | 原 session/车牌，`replayed=true` |
| 新 ID 同卡再次入场 | `REJECTED / DUPLICATE_ENTRY` |
| `board-v015-exit-1` 出场 | `EXIT_GRANTED`，原 session 被关闭 |
| SIGTERM | 打印 `stopping: sessions=1 active=0` 后退出 |

最终数据库检查：

```text
quick_check = ok
journal_mode = wal
synchronous = 2 (FULL)
parking_sessions = 1，active = 0
access_events = 3（成功入场、重复拒绝、成功出场）
```

板端验收结束后无残留 `parkingd` 进程。本次创建的是命名明确的验收数据库，没有覆盖
默认生产数据库 `parking.db`。

## 9. 使用方法

终端 A：

```sh
adb shell
/home/reynor/camera-project/bin/parkingd --fake-plate TEST3568
```

终端 B：

```sh
adb shell /home/reynor/camera-project/bin/parkingctl card-present \
    --direction entry --card 04A10B7F --event-id demo-entry-1

adb shell /home/reynor/camera-project/bin/parkingctl card-present \
    --direction exit --card 04A10B7F --event-id demo-exit-1
```

检查数据库：

```sh
adb shell sqlite3 /userdata/parking-lot/db/parking.db \
    'PRAGMA quick_check; SELECT session_id,card_uid,plate_number,active FROM parking_sessions;'
```

`event-id` 应由事件产生方保证唯一。为了测试重放，应显式重复使用同一个 ID；正常新事件
不得复用旧 ID。

## 10. 当前限制与下一阶段

- 没有数据库查询/备份专用 CLI；当前只能用板端 `sqlite3`；
- 没有磁盘空间水位、WAL 周期 checkpoint/backup 和长期增长策略；
- schema v1 是当前业务闭环的最小字段集，尚无卡片白名单、图片路径或时间质量字段；
- Fake LPR 仍在进程内固定返回一个车牌；
- 没有真实 UART RFID、Camera 抓拍、overlay、音频、道闸或 BusyBox 自启动服务；
- 没有在 COMMIT 指令附近做掉电/`SIGKILL` 故障注入和 24/72 小时长稳。

下一小阶段是阶段 3B：先实现独立、可单测的 RFID 串口帧 parser 和 adapter，再根据实际
读卡器确认 tty、波特率、帧长、校验/BCC 和拔插行为。手动模式必须继续保留，并与真实
RFID 同时复用当前 `ParkingCoordinator` 和 SQLite repository。Camera/LPR IPC 在真实输入
边界稳定后进入阶段 4。
