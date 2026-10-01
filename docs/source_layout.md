# 源码目录与命名规范

## 1. 目标

项目使用“目录说明职责、文件名保持简短”的规则。读者应先从目录判断代码属于业务、
存储、识别、控制还是程序入口，不需要从一个很长的文件名猜测层次。

类名仍应完整表达语义，例如 `SQLiteParkingRepository`；文件位于 `storage/` 后只需命名
为 `sqlite.cpp`，不重复写成 `sqlite_parking_repository.cpp`。

## 2. Camera 子系统

```text
inc/camera/
  capture/
    device.hpp         V4L2 设备、能力和格式
    buffer.hpp         MMAP 队列和 DMA-BUF
    session.hpp        capture session 与 L1/L2 恢复
  display/
    device.hpp         DRM/KMS 资源探测
    display.hpp        framebuffer、modeset、page flip
    transform.hpp      RGA 旋转和颜色转换

src/camera/
  capture/             device.cpp、buffer.cpp、session.cpp
  display/             device.cpp、display.cpp、transform.cpp
  apps/
    camera_demo.cpp
    drm_probe.cpp
    rga_drm_test.cpp
    display_once.cpp
    display_stream.cpp
```

Camera 的 target 也按照职责拆分为 `camera_capture`、`camera_drm` 和
`camera_transform`；apps 只组合这些库，不再重复编译同一组源文件。

## 3. Parking 子系统

```text
inc/parking/
  domain/
    card.hpp           RFID UID 值对象
    model.hpp          event、session、result 和稳定枚举
    fee.hpp            计费策略
    coordinator.hpp    入场/出场业务编排
  storage/
    repository.hpp     存储抽象接口
    memory.hpp         单元测试内存实现
    sqlite.hpp         板端 SQLite 实现
  recognition/
    recognizer.hpp     车牌识别抽象接口
    fake.hpp           固定结果模拟实现
  control/
    protocol.hpp       PARKING/1 消息格式
    server.hpp         Unix seqpacket 服务端

src/parking/
  domain/              与 domain 头文件同名的实现
  storage/             memory.cpp、sqlite.cpp
  recognition/         fake.cpp
  control/             protocol.cpp、server.cpp
  apps/                parkingd.cpp、parkingctl.cpp

tests/parking/
  domain_test.cpp
  control_test.cpp
  sqlite_test.cpp
```

## 4. 允许的依赖方向

```text
apps
  -> control
  -> domain
  -> storage implementation
  -> recognition implementation

domain/coordinator
  -> domain model
  -> storage interface
  -> recognition interface

storage implementation -> storage interface + domain model
recognition implementation -> recognition interface + domain model
control -> domain model
```

`domain` 不得 include SQLite、Unix socket、V4L2、DRM 或 Python 头文件。`sqlite.cpp` 不得
处理命令行参数；`parkingd.cpp` 不得拼接 SQL；`protocol.cpp` 不得直接修改停车状态。

Camera 中 `capture` 不依赖 DRM/RGA；`display` 不管理 V4L2 buffer；`apps` 才负责组合
两侧资源和确定退出顺序。

## 5. 文件命名规则

- 文件和目录全部使用小写 `snake_case`；
- 头文件使用 `.hpp`，实现文件使用 `.cpp`；
- 同一模块头文件和实现文件基本名一致；
- `apps/` 下文件名与最终程序名一致；
- 测试使用 `<被测模块>_test.cpp`；
- 避免 `common.cpp`、`utils.cpp`、`misc.cpp` 等无法表达职责的名字；
- 不把 `parking`、所属目录名或完整类名重复写进文件名。

## 6. 文件开头说明

每个实现文件在 include 之前写明用途和层次，例如：

```cpp
/*
 * 文件用途：实现 SQLite schema、事务、session 历史和 event 幂等日志。
 * 所属层次：storage，是 parkingd 当前使用的持久化 adapter。
 */
```

这段说明只描述文件边界。公共类和函数的参数、返回值、异常及所有权仍使用头文件中的
Doxygen 注释，不能用文件说明代替接口文档。

## 7. 下一阶段如何落目录

暂不接入真实 RFID。下一阶段的 Camera/Fake LPR IPC 建议新增：

```text
inc/parking/recognition/client.hpp
src/parking/recognition/client.cpp

inc/parking/control/camera_protocol.hpp
src/parking/control/camera_protocol.cpp

src/parking/apps/fake_lpr.cpp
```

如果 Camera IPC 后续增长，应再建立 `camera/` 子目录；不要把抓帧、模型调用或 socket
协议继续堆入 `parkingd.cpp`。
