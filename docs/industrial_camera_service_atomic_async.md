# RK3568 摄像头显示工业化：退出、故障恢复、Atomic KMS 与异步流水线

## 1. 这篇文档要解决什么问题

当前项目已经证明下面这条真实硬件链路可以工作：

```text
Sensor / ISP
    -> V4L2 采集 buffer
    -> DMA-BUF fd
    -> RGA 旋转 + NV12 转 XRGB8888
    -> DRM framebuffer A/B
    -> legacy page flip
    -> VOP -> DSI -> LCD
```

这非常重要，但“链路能跑通”和“设备可以长期无人值守运行”是两个阶段。工业现场还会
遇到摄像头暂时不出帧、驱动报错、RGA 超时、显示提交卡住、进程崩溃、频繁重启、日志
占满磁盘等问题。

本文从零解释四件事：

1. 当前 demo 已经具备哪些退出保护，还缺什么；
2. 工业服务怎样检测、隔离和恢复故障；
3. Atomic KMS 是什么，与当前 legacy KMS 有什么区别；
4. 异步流水线是什么，为什么它与 Atomic KMS 相关但不是同一个概念。

先给出最重要的结论：

- 当前实现是**同步 RGA + legacy KMS + 双 DRM buffer + 单线程串行控制**；
- 当前没有 CPU `memcpy` 整帧图像，但 RGA 会把 NV12 转换并写入另一块 DRM 内存；
- 当前已经能响应 `SIGINT/SIGTERM`、执行 `STREAMOFF`、等待 page-flip event 并释放
  DRM/V4L2 资源；
- 当前已能有预算地重启 V4L2 capture stream；stream 本身恢复失败、其他故障域或预算
  耗尽时仍会退出，尚无设备 session 重建、健康检查或进程守护；
- Atomic KMS 的核心是“把一组显示状态作为一个事务验证和提交”，它不等于异步；
- 异步流水线的核心是“允许不同硬件阶段并行处理不同帧”，它也不等于多线程；
- 下一步不应直接把所有功能同时异步化，应该先把同步 demo 改造成生命周期清楚、能够
  分级恢复的长期服务，再迁移 Atomic KMS，最后引入 fence 和多帧在途。

## 2. 先准确理解当前程序

### 2.1 当前有两组不同的 buffer

第一组是 4 个 V4L2 MMAP capture buffer，由 RKISP 驱动分配：

```text
ISP 写入 -> DQBUF 交给应用 -> RGA 读取 -> QBUF 归还 ISP
```

程序对每个 capture buffer 执行 `VIDIOC_EXPBUF`，得到 DMA-BUF fd。RGA 通过 fd
直接读取这块内存，用户态没有执行整帧 `memcpy`。

第二组是 2 个 DRM dumb framebuffer，由 DRM 驱动分配：

```text
RGA 写入 A，VOP 扫描 A
RGA 写入 B，翻页后 VOP 扫描 B
随后 A 才能重新写入
```

所以当前不是“同一块 V4L2 buffer 被 VOP 直接扫描”，而是：

```text
V4L2 DMA-BUF --RGA硬件读写/格式转换--> DRM GEM buffer
```

这不是 CPU 拷贝，但确实有一次由 RGA 完成的内存读写。它同时完成了 NV12 到
XRGB8888、1920x1080 到 1080x1920 的旋转和格式转换。

### 2.2 当前每一帧的真实执行顺序

`camera_display_stream` 的主循环可以简化成：

```text
等待 V4L2 fd 最多 1000 ms
    |
    v
VIDIOC_DQBUF
    |
    v
同步 RGA 转换（函数返回时 RGA 已经完成）
    |
    v
VIDIOC_QBUF：立即把源 capture buffer 归还 ISP
    |
    v
第一帧：drmModeSetCrtc
后续帧：drmModePageFlip + 等待 flip-complete，最长 2000 ms
    |
    v
切换 A/B buffer，再处理下一帧
```

这里有一个容易误解的地方：摄像头硬件、RGA 和 VOP 本身都是独立硬件，VOP 在程序
等待时仍会不断扫描当前 framebuffer，ISP 也会继续填充其他已排队的 V4L2 buffer。
因此硬件层面已经存在一定并行。

但是从应用调度角度看，主线程必须等 RGA 完成，再等 DRM 翻页完成，才会处理下一帧。
程序没有同时维护多个处于 RGA、待显示和扫描状态的帧，所以本文把它称为**同步流水
线**或**单帧在途控制**。

### 2.3 当前已经具备的安全基础

当前代码并不是完全没有退出和错误处理：

- `SIGINT`、`SIGTERM` handler 只设置 `sig_atomic_t` 标志，没有在 signal handler
  内执行不安全的 ioctl 或内存释放；
- V4L2 `poll()` 被信号中断时会返回主循环检查退出标志；
- 连续 3 次采集超时会尝试一次有预算的 V4L2 STREAMOFF/QBUF/STREAMON 恢复；
- V4L2 `POLLERR/POLLHUP/POLLNVAL` 会被报告；
- `V4L2_BUF_FLAG_ERROR` 帧会被丢弃并重新 QBUF；
- page flip 同一时刻只允许一个请求在途，并设置 2000 ms 超时；
- 正常退出执行 `VIDIOC_STREAMOFF`、关闭 CRTC、删除 framebuffer 和释放内存；
- C++ 对象的析构函数提供不抛异常的兜底清理，部分初始化失败也能逆序回滚；
- 板端 shell 脚本使用 `trap`，测试结束后按当前开机策略决定是否恢复桌面。

这些能力足以支撑受控测试，却还不能称为工业服务。

### 2.4 当前与工业服务之间的差距

| 维度 | 当前 demo | 工业目标 |
| --- | --- | --- |
| 运行时间 | 命令行限定 1..3600 秒 | 可持续运行，直到收到停止命令 |
| 故障策略 | capture 超时已有 L1 恢复；其他域仍退出 | 先逐域恢复，超过预算才重启进程 |
| 进程守护 | 无 | worker 崩溃后受控拉起 |
| 重试方式 | 基本无重试 | 有上限、退避和熔断 |
| 健康状态 | 结束时打印统计 | 持续输出心跳、帧率、超时和恢复次数 |
| 日志 | 标准输出文本 | 分级、限量、可轮转、带故障原因码 |
| 配置 | 固定 1920x1080/NV12/路径 | 配置文件 + 严格校验 + 实际值记录 |
| 显示接口 | legacy SetCrtc/PageFlip | Atomic TEST_ONLY + Atomic commit |
| RGA 调度 | `IM_SYNC` | 验证后可用异步 job/fence |
| DRM buffer | 双缓冲 | 异步阶段通常使用三缓冲 |
| 故障验证 | 10/60 秒功能测试 | 长稳、断流、信号、驱动错误、反复重启测试 |

## 3. “工业化”到底意味着什么

工业化不是简单地“加一个 while(true)”。至少要同时保证：

### 3.1 正确性

- ISP、RGA、VOP 不会同时以冲突方式使用同一块 buffer；
- 只有 RGA 完成读取后，V4L2 源 buffer 才能 QBUF；
- 只有 VOP 不再扫描后，DRM 目标 buffer 才能重新写；
- 颜色空间、量化范围、stride、offset 和旋转参数明确；
- 出错时不会把所有权不明的 buffer 重新投入使用。

### 3.2 可用性

偶发坏帧或短暂超时不应立刻让整个服务永久停止；真正的设备故障也不应让程序无限
忙循环，占满一个 CPU。

### 3.3 可恢复性

恢复动作应从小到大逐级升级：先丢一帧，再重启 stream，再重开设备，最后才重启
worker。不要一遇到问题就重启整块开发板。

### 3.4 可观测性

现场工程师至少要回答：

- 最后一帧是什么时候到达的；
- 采集、转换和显示各自是否还在前进；
- 当前处于正常、降级还是恢复状态；
- 最近一次恢复是因为什么；
- 过去一小时丢了多少帧、重启了多少次；
- 是否发生 fd、内存或 buffer 泄漏。

### 3.5 有界延迟

实时预览通常更重视“看到最新画面”，而不是“每一帧都必须显示”。如果下游变慢，
无限保存旧帧会让画面延迟越来越大。工业预览常用 `latest-frame-wins`：丢掉尚未显示
的旧 Ready 帧，只保留最新帧。

## 4. 正常退出应该怎样设计

### 4.1 不要在 signal handler 里直接清理

很多 C/C++ 和 libdrm 函数都不是异步信号安全的。正确做法是：

```text
SIGTERM/SIGINT
    -> handler 只设置退出标志，或通过 signalfd 唤醒事件循环
    -> 主控制流停止接收新工作
    -> 按所有权顺序清理资源
```

当前代码采用“只设置标志”的方式，这是正确基础。长期服务进一步推荐 Linux
`signalfd`，把信号也作为 `poll/epoll` 事件处理，避免全局变量和 EINTR 分支散落。

### 4.2 推荐的正常停止顺序

同步版本建议使用下面的停止事务：

```text
1. 状态切换为 Stopping，拒绝新的 DQBUF/RGA/DRM 提交
2. 如果有同步 RGA 调用，等待它返回
3. 如果有 page flip pending，等待完成事件或达到退出专用短超时
4. VIDIOC_STREAMOFF，让驱动收回全部 capture buffer
5. Atomic 禁用 plane/CRTC；当前 legacy 版本则安全关闭 CRTC
6. 删除 DRM framebuffer ID
7. 关闭 DRM 导出的 DMA-BUF fd、munmap、销毁 GEM dumb buffer
8. 关闭 V4L2 DMA-BUF fd、munmap、REQBUFS(count=0)
9. 关闭 DRM/V4L2 fd
10. 输出最终统计和退出原因
```

异步版本还要先处理 RGA fence 和 KMS out-fence，不能只看 C++ 对象是否存在。

### 4.3 `SIGKILL` 和掉电不能优雅清理

`SIGKILL` 无法被捕获。进程 fd 关闭后，内核通常能回收其 GEM、DMA-BUF 和 V4L2
对象，但应用没有机会执行自己的恢复流程、写最终日志或主动关闭显示。

突然掉电更不可能执行退出流程。因此工业设计必须满足：

- 下次启动能够从未知显示状态重新 modeset；
- 不依赖上一次运行留下的 framebuffer ID 或 plane ID；
- 配置和关键状态采用原子写入，不能把半写文件当成有效配置；
- 文件系统和日志策略能承受非正常断电。

### 4.4 退出码也应成为接口

建议给 worker 定义稳定退出码，而不是所有错误都返回 1：

| 退出码示例 | 含义 | supervisor 策略 |
| --- | --- | --- |
| 0 | 收到正常停止命令 | 不重启 |
| 10 | 配置错误/格式不支持 | 不反复重启，进入 Fatal |
| 20 | 摄像头暂时不可用 | 退避后重启 worker |
| 21 | 摄像头持续断流 | 退避后重启，达到预算后报警 |
| 30 | RGA 初始化或运行失败 | 重建 RGA；失败则重启 worker |
| 40 | DRM/KMS 暂时失败 | 重建显示会话；失败则重启 worker |
| 50 | 内部状态机不变量破坏 | 立即退出，由 supervisor 拉起并报警 |

具体数字不是标准，关键是把“重试有意义”和“重试不会解决”区分开。

## 5. 故障检测与分级恢复

### 5.1 先按故障域分类

不要把所有异常都归类为“camera error”。推荐至少区分：

```text
Capture 域：Sensor / CSI / ISP / V4L2
Transform 域：RGA / IOMMU / fence
Display 域：DRM master / plane / CRTC / VOP / DSI
Control 域：配置、信号、内部状态机、资源耗尽
System 域：内存不足、文件系统只读、温度、电源
```

只有先知道故障属于哪一域，才能只重建必要部分。

### 5.2 推荐故障表

| 现象 | 检测方式 | 第一动作 | 升级动作 |
| --- | --- | --- | --- |
| 单个 V4L2 ERROR 帧 | `V4L2_BUF_FLAG_ERROR` | 计数、丢帧、QBUF | 短时间超过阈值则重启 stream |
| 偶发采集超时 | V4L2 `poll` 超时 | 记录一次，继续等待 | 连续 N 次执行 STREAMOFF/ON |
| V4L2 `EIO/EPIPE` | ioctl errno | 停止采集会话 | 关闭并重开 video fd |
| video node 消失 | `ENODEV`/设备路径消失 | 进入 WaitingForDevice | 带退避等待设备恢复 |
| sequence 跳变 | buffer sequence | 统计，不一定重启 | 跳变持续且 FPS 异常时重建 capture |
| RGA job 失败 | RGA 返回值/fence 错误 | 丢当前帧，保护两端 buffer | 重建 RGA context，失败则重启 worker |
| RGA 超时 | fence deadline | 不复用源/目标 buffer | 重建 RGA；必要时 worker 重启 |
| page flip 超时 | DRM event deadline | 停止新提交 | 重建 DRM session 并重新 modeset |
| DRM master 丢失 | commit 返回 `EACCES` 等 | 关闭显示会话 | 等待所有权或退出报警 |
| connector 断开 | connector/property 变化 | 进入 NoDisplay | 周期探测，连接后重新 TEST_ONLY |
| 内存/fd 耗尽 | `ENOMEM/EMFILE`、指标 | 停止新工作 | worker 重启并报告泄漏嫌疑 |
| 内部非法 buffer 状态 | 状态机断言/检查 | 禁止继续复用 buffer | worker 立即失败退出 |

这里的阈值必须配置化。例如“连续 3 次 1 秒采集超时”适合当前 demo，但工业设备应
结合摄像头帧率和允许恢复时间确定。

必须特别注意：**超时不等于任务已经取消**。例如 page-flip event 超时后，请求可能
仍在内核中；RGA fence 超时后，RGA 也可能仍在访问源/目标内存。此时不能简单地把
相关 buffer 标记为 FREE。应先把它们放入隔离状态，停止新提交，然后通过完成 fence、
明确的设备会话重建或 worker 退出重新建立所有权。否则恢复代码本身可能制造 ISP、
RGA、VOP 同时访问同一内存的竞争。

### 5.3 推荐恢复阶梯

```text
L0  Frame recovery
    丢掉单个错误帧，保持 stream 运行

L1  Stream recovery
    STREAMOFF -> 重置队列状态 -> QBUF all -> STREAMON

L2  Device-session recovery
    销毁 V4L2 会话 -> close -> 重新 open/协商格式/申请 buffer

L3  Pipeline-domain recovery
    单独重建 RGA 或 DRM/KMS 会话，不动仍健康的另一端

L4  Worker restart
    当前进程完整退出，由 supervisor 重新启动

L5  Board-level recovery
    只有反复 worker 重启仍失败、且有明确产品策略时才考虑重启系统
```

每一级都应有验收条件和最大耗时。恢复成功后不要立刻把历史错误清零，应保留累计
计数，便于判断设备是否正在劣化。

### 5.4 为什么必须退避和熔断

如果 `/dev/video0` 不存在，下面这种循环是错误的：

```text
open失败 -> 立即重试 -> 立即失败 -> ...
```

它会占用 CPU、刷爆日志，也可能阻碍驱动恢复。建议使用：

```text
第1次失败等待 0.5 s
第2次失败等待 1 s
第3次失败等待 2 s
第4次失败等待 4 s
之后上限 10 s，并加入少量随机抖动
```

再增加时间窗口预算，例如 60 秒内最多完整重建 5 次。超过预算后进入 `Degraded` 或
`Fatal`，降低日志频率并等待人工/外部控制器处理。这就是“熔断”，目的是避免一个
永久硬件故障造成无限重启风暴。

## 6. 推荐的服务状态机

状态机比散落的 `if (error) restart()` 更容易证明正确：

```text
             +------------------+
             |      Starting    |
             +---------+--------+
                       |
                       v
             +---------+--------+
       +---->|       Running    |<----------------+
       |     +----+--------+----+                 |
       |          |        |                      |
恢复成功| 采集故障  |        | 显示/RGA故障          | 恢复成功
       |          v        v                      |
       |  +-------+--+  +--+----------------+     |
       +--+Recovering|  | RecoveringDisplay +-----+
          | Capture  |  | / Transform       |
          +-----+----+  +---------+----------+
                |                 |
                +--------+--------+
                         | 超过恢复预算
                         v
                  +------+------+
                  |  Degraded   |
                  +------+------+
                         | supervisor策略
                         v
                  +------+------+
                  |    Fatal    |
                  +-------------+

任意状态 --SIGTERM--> Stopping --> Stopped
```

建议把每次状态转换写入一条结构化日志：旧状态、新状态、原因码、累计尝试次数和耗时。

## 7. 进程内恢复与进程外守护

### 7.1 为什么需要两层

进程内控制器最了解 buffer 所有权，适合执行精细恢复；但如果程序段错误、死锁或内部
状态已经损坏，它不能自救。因此还需要一个逻辑尽量简单的进程外 supervisor。

```text
BusyBox init
    -> camera-display-supervisor
         -> camera-display-worker
              -> V4L2 / RGA / DRM
```

职责建议：

- worker：所有图像处理、buffer 状态机和局部恢复；
- supervisor：启动/停止 worker、限制重启频率、检查心跳、保存最终退出原因；
- BusyBox init 脚本：只管理 supervisor，不直接承载复杂恢复逻辑。

当前系统不是 systemd，而是 BusyBox init + `/etc/init.d/rcS`。后续可以增加独立的
`Sxxcamera-display` 启动脚本，但启动顺序必须晚于设备节点、文件系统和 `S40rkaiq_3A`。
Weston/systemui 已单独禁用，不应把桌面启停逻辑继续塞进正式 camera worker。

### 7.2 心跳不能只代表“进程还活着”

进程存在不等于图像链路正常。建议心跳同时包含三个前进指标：

```text
last_capture_timestamp
last_rga_complete_timestamp
last_flip_complete_timestamp
```

如果 PID 存在但 `last_capture` 已经 5 秒不更新，supervisor 应判断为业务失活，而不是
继续认为服务健康。

### 7.3 看门狗策略

可以分三层：

1. worker 内部阶段 deadline：采集、RGA、DRM 各有超时；
2. supervisor 心跳 deadline：worker 卡死时先发 SIGTERM，等待宽限期，再发 SIGKILL；
3. 硬件 watchdog：只有整个用户空间失去响应时才复位开发板。

硬件 watchdog 不应该代替应用故障恢复，否则每次摄像头断流都可能导致整板重启。

## 8. KMS 是什么

KMS 是 Kernel Mode Setting，即 Linux DRM 子系统中负责显示模式和扫描输出的部分。
可以先把主要对象理解成：

```text
Framebuffer：一张可被显示硬件读取的图像描述
Plane：      一层图像，可设置来源区域、屏幕位置、缩放、旋转等
CRTC：       产生显示时序，并把各 plane 合成后逐行扫描
Connector：  输出接口，例如 DSI-1、HDMI-A-1
Mode：       1080x1920、刷新率和完整时序参数
```

一条典型关系是：

```text
Framebuffer -> Plane -> CRTC -> Connector -> DSI panel
```

VOP 是 RK3568 内部真正执行 plane 合成和扫描的硬件。KMS 对象是 Linux 内核向
用户态暴露的控制模型，不是额外的物理芯片。

## 9. 当前使用的 legacy KMS

“legacy”表示较早的一组 KMS 接口，不表示它完全不能用。当前项目使用：

```text
首次显示：drmModeSetCrtc(crtc, framebuffer, connector, mode)
后续翻页：drmModePageFlip(crtc, next_framebuffer, EVENT)
完成通知：poll(DRM fd) -> drmHandleEvent -> page-flip handler
```

它的优点是接口简单，适合证明 modeset 和双缓冲所有权。局限是：

- `drmModeSetCrtc` 主要围绕 CRTC 和 primary framebuffer，plane 控制不统一；
- mode、connector、plane、缩放和旋转等状态可能需要多个调用分别修改；
- 很难在真正修改硬件前验证“一整套状态组合是否可行”；
- 多 plane 同步切换时，某些设置可能已成功、另一些失败，回滚复杂；
- 显式同步 fence 与现代显示属性不容易统一接入。

当前 `drmModePageFlip` 本身已经是“提交后通过事件告知完成”的接口，但项目的
`pageFlipAndWait()` 立即在内部等待该事件，所以从上层主循环看仍是同步调用。

## 10. Atomic KMS 到底是什么

### 10.1 “Atomic”不是 CPU 原子指令

Atomic KMS 把 connector、CRTC 和 plane 的属性修改收集成一个请求：

```text
Atomic request
  Connector.CRTC_ID = 115
  CRTC.MODE_ID       = <1080x1920 mode blob>
  CRTC.ACTIVE        = 1
  Plane.FB_ID        = 200
  Plane.CRTC_ID      = 115
  Plane.SRC_X/Y/W/H  = ...
  Plane.CRTC_X/Y/W/H = ...
  Plane.rotation     = ...
```

上面的 `115`、`200` 只是帮助理解的例子。DRM object ID 和 property ID 都由运行时
枚举获得，不能硬编码进正式程序。

然后一次性验证或提交：

```text
drmModeAtomicCommit(fd, request, flags, user_data)
```

“原子”的含义是：这组显示状态作为一个整体生效，或者整个请求失败，不让用户态看到
一半新状态、一半旧状态。

可以把它类比成数据库事务：

```text
legacy：逐条修改表A、表B、表C，中间失败时自己回滚
atomic：把修改组成一个事务，内核整体检查后一次提交
```

### 10.2 `TEST_ONLY` 为什么重要

Atomic KMS 可以先做只验证、不改变屏幕的提交：

```text
DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET
```

内核会检查：

- plane 是否能连接目标 CRTC；
- framebuffer 格式和 modifier 是否支持；
- source/destination 尺寸和缩放比例是否合法；
- mode、connector 路由和带宽是否可接受；
- 旋转等属性组合是否受支持。

只有 TEST_ONLY 成功，才执行真实 commit。它不能保证硬件永远不会在运行时故障，但能
把很多配置错误挡在改变屏幕之前。

### 10.3 Atomic commit 可以同步也可以异步

下面两种都属于 Atomic KMS：

```text
阻塞 atomic commit：调用直到提交完成才返回
NONBLOCK atomic commit：立即返回，稍后收到 page-flip event/out-fence
```

所以：

```text
Atomic != 异步
Atomic = 状态事务语义
NONBLOCK/event/fence = 调度和完成通知语义
```

### 10.4 当前实现与 Atomic KMS 的对比

| 项目 | 当前实现 | Atomic KMS 目标 |
| --- | --- | --- |
| 初始 modeset | `drmModeSetCrtc` | connector/CRTC/plane 一次 atomic commit |
| 翻页 | `drmModePageFlip` | 修改 plane `FB_ID` 后 atomic commit |
| plane | 大多由 legacy 接口隐式处理 primary plane | 显式选择 plane 并设置全部属性 |
| 预检查 | 无完整事务预检查 | `TEST_ONLY` |
| 多对象更新 | 多个旧接口 | 一个 request 整体提交 |
| 旋转/缩放 | 当前交给 RGA，KMS 未显式配置 | 可查询并选择性使用 plane 属性 |
| 完成通知 | page-flip event | event 和可选 out-fence |
| 输入同步 | RGA 同步返回后再 flip | 可选 `IN_FENCE_FD` |
| 失败回滚 | 应用按步骤处理 | 未生效的 atomic request 整体失败 |

### 10.5 Atomic KMS 不会自动带来什么

Atomic KMS 不会自动：

- 消除 RGA 转换；
- 把 NV12 变成 RGB；
- 实现零拷贝；
- 自动选择正确 plane；
- 自动解决 BT.601/BT.709；
- 自动异步运行；
- 自动恢复摄像头或显示故障；
- 保证所有驱动都实现相同属性。

它提供的是更完整、更可验证的显示控制接口。性能收益取决于后续是否使用 overlay
plane、direct scanout、nonblocking commit 和显式 fence。

### 10.6 当前 RK3568 实板的 Atomic 能力结论

本次在板端执行了只读探测：

```bash
modetest -a -D /dev/dri/card0 -p
```

命令返回 0，驱动接受 Atomic client capability；输出中存在：

- CRTC：`ACTIVE`、`MODE_ID`、`OUT_FENCE_PTR`；
- plane：`FB_ID`、`CRTC_ID`、`SRC_X/Y/W/H`、`CRTC_X/Y/W/H`；
- plane：`IN_FENCE_FD`；
- 部分 plane 的 `rotation`、`zpos`、`alpha` 等属性；
- 部分 plane 支持 NV12，但不同 plane 的格式、旋转和 possible CRTC 能力不同。

这证明项目具备开发 Atomic backend 的接口基础，但还没有完成：

- 对目标 connector/CRTC/plane 的属性 ID 自动发现；
- mode property blob 创建和销毁；
- Atomic `TEST_ONLY`；
- 真实 Atomic modeset/翻页；
- 对实际选择 plane 的 NV12、旋转、缩放组合验收；
- `IN_FENCE_FD/OUT_FENCE_PTR` 的端到端同步验证。

因此准确状态是：**实板能力探测通过，项目 Atomic 功能尚未实现。**

## 11. 什么是异步流水线

### 11.1 先用工厂流水线理解

假设有三道工序：

```text
采集 -> RGA转换 -> 屏幕翻页
```

同步方式像只有一个工件：工件完成第一道工序后，操作员站着等待第二道，再等待第三道，
全部结束才拿下一个工件。

异步流水线允许：

```text
ISP 正在采集帧 N+2
RGA 正在转换帧 N+1
VOP 正在扫描帧 N
```

三块硬件同时工作，但它们操作的是不同 buffer，并通过事件或 fence 交接所有权。

### 11.2 当前同步时间线

简化后，主线程看到的是：

```text
时间 --->

帧N：   [等待/DQ][RGA同步等待][提交flip][等待flip完成]
帧N+1：                                              [DQ][RGA][flip][等待]
```

当前实测 RGA 每帧约 5.2..9.0 ms，DSI 模式约 54.44 Hz，一个 vblank 周期约 18.4 ms。
虽然等待捕获、RGA 和翻页不一定每次都占满这些上限，但串行等待会限制调度余量，也使
某一阶段抖动直接传递给下一阶段。

### 11.3 目标异步时间线

```text
时间 --->

Capture： [N]------[N+1]------[N+2]------[N+3]
RGA：          [N]------[N+1]------[N+2]
Display：             [N]---------[N+1]---------[N+2]
```

每一行可以由硬件独立推进。用户态事件循环只在状态发生变化时处理：

- V4L2 fd 可读：有新 capture buffer；
- RGA fence 可读/完成：转换结束；
- DRM fd 可读：flip event 到达；
- timerfd 到期：某阶段超时；
- signalfd 可读：收到停止信号。

一个线程使用 `poll/epoll` 就能实现这种异步调度；异步不等于必须为每个阶段创建线程。

### 11.4 为什么异步通常需要三块 DRM buffer

双缓冲时：

```text
A = VOP 正在扫描
B = RGA 写完，等待下一次 flip 或已经 pending
```

此时没有第三块空闲目标供 RGA 提前处理下一帧。三缓冲可以形成：

```text
A = SCANNING   当前显示
B = PENDING    已提交，等待下一个 vblank
C = RENDERING  RGA 正在生成更后面的一帧
```

三缓冲不是越多越好。无限增加 buffer 会增加内存和延迟；实时预览一般使用固定小池，
配合最新帧优先丢帧策略。

### 11.5 fence 是什么

DMA-BUF fd 表示“哪块共享内存”，fence 表示“某次硬件读写什么时候完成”。二者不是
同一个对象。

异步版本必须回答三个问题：

1. RGA 什么时候不再读取 V4L2 源 buffer？只有这时才能 QBUF；
2. RGA 什么时候写完 DRM 目标 buffer？只有这时 VOP 才能开始扫描；
3. VOP 什么时候不再扫描旧 buffer？只有这时 RGA 才能重新覆盖它。

理想的显式同步链路是：

```text
RGA async submit
    -> 返回 completion fence
    -> fence 作为 KMS plane.IN_FENCE_FD
    -> Atomic NONBLOCK commit
    -> CRTC.OUT_FENCE_PTR / flip event 表示显示切换完成
```

但旧版 BSP 和板载 `librga 1.3.1` 是否能以可靠方式导出/传递 RGA fence，必须实板验证。
如果 RGA 只能同步运行，也可以先做“统一事件循环 + Atomic NONBLOCK”，不要伪造 fence
或在硬件仍使用 buffer 时提前复用。

### 11.6 异步 buffer 状态机

每个帧上下文建议有明确状态：

```text
V4L2_QUEUED
    | DQBUF
    v
CAPTURED
    | submit RGA
    v
RGA_PENDING
    | RGA fence complete
    +----------------------> 源 V4L2 buffer QBUF
    v
DISPLAY_READY
    | Atomic commit
    v
FLIP_PENDING
    | flip complete
    v
SCANNING
    | 被下一帧替换 / out-fence complete
    v
DRM_FREE
```

V4L2 源 buffer 和 DRM 目标 buffer 是两个对象，不能只用一个状态变量描述它们。推荐
`FrameContext` 同时记录 source index、source DMA-BUF、destination index、RGA fence、
atomic commit 序号和各阶段时间戳。

上图表示较保守的第一版：用户态等 RGA fence 完成后再提交 KMS。驱动组合验证可靠后，
也可以在 RGA 尚未完成时，把 fence 直接设置为 plane 的 `IN_FENCE_FD` 并提交 Atomic
request；KMS 会等待 fence 后再使用目标 buffer。即便这样，V4L2 源 buffer 仍必须等
RGA 读取完成后才能 QBUF，不能因为 KMS 已接受请求就提前归还。

### 11.7 最新帧优先

当 `DISPLAY_READY` 队列中已经有旧帧，而更新的帧又完成时：

```text
旧 Ready 帧：不再提交显示，释放它的 DRM 目标 buffer
最新 Ready 帧：在下一次可提交机会显示
```

不能丢掉仍在 `RGA_PENDING`、`FLIP_PENDING` 或 `SCANNING` 的 buffer，除非相应 fence
已经完成。所谓丢帧只是“不把它送到屏幕”，不代表可以违反硬件所有权。

## 12. Atomic KMS 与异步流水线的关系

四种组合都可能存在：

| 显示接口 | 调度方式 | 是否可行 |
| --- | --- | --- |
| legacy KMS | 同步等待 | 当前项目 |
| legacy KMS | 异步事件循环 | 可行，`drmModePageFlip` 本来就能发 event |
| Atomic KMS | 阻塞 commit | 可行，先用于正确性迁移 |
| Atomic KMS | NONBLOCK + event/fence | 推荐的最终方向 |

因此合理迁移过程是：

```text
当前 legacy + 同步
    -> Atomic + 阻塞/单 pending（先验证显示状态正确）
    -> Atomic NONBLOCK + 统一事件循环
    -> 验证 RGA async/fence 后增加多帧在途
```

这样每一步只有一个主要变量发生变化，出现花屏或死锁时容易定位。

## 13. 推荐的软件架构

```text
camera-display-supervisor
    |
    +-- 启停、心跳、退避、重启预算、最终报警
    |
    `-- camera-display-worker
          |
          +-- PipelineController      顶层状态机和恢复决策
          +-- CaptureSession          V4L2格式、buffer和stream生命周期
          +-- RgaEngine               同步/异步转换和fence
          +-- AtomicKmsDisplay        属性发现、TEST_ONLY、commit和event
          +-- FramePool               源/目标buffer所有权状态机
          +-- EventLoop               video/drm/fence/signal/timer事件
          +-- HealthReporter          指标、心跳、状态和错误原因
```

重要边界：

- `CaptureSession` 不决定是否重启整个进程，只报告带类型的错误；
- `AtomicKmsDisplay` 不猜测 RGA 是否完成，只接受 ready fence 或明确完成的 buffer；
- `FramePool` 是 buffer 所有权的唯一真相来源；
- `PipelineController` 决定丢帧、局部恢复或升级重启；
- supervisor 不直接操作 V4L2/DRM buffer，避免两层同时清理同一资源。

## 14. 建议的开发阶段

### 阶段 I：长期运行和确定性退出（0.11.0 已实现）

先不改变图像算法和 KMS 接口：

- 增加 `--run-forever`，与测试用 `--stream <seconds>` 分开；
- 抽出 `PipelineController` 生命周期状态；
- 使用统一 stop token 或 `signalfd`；
- 给正常退出、配置错误和运行故障定义退出原因/退出码；
- 每一次退出输出 capture/RGA/DRM 的最后进度和清理结果；
- 反复执行 1000 次启动、运行、SIGTERM、资源泄漏检查。

验收：任何初始化步骤失败和任意正常停止点都不残留 DRM client、video stream 或 fd。

当前已实现 `--run-forever`、生命周期状态、SIGINT/SIGTERM 正常清理、故障域退出码和
1..1000 次可配置的 ADB 验收工具；本次实板完成 3 轮重复 SIGTERM 测试。实现与实板
记录见[Camera Worker 长期运行与确定性退出](camera_worker_lifecycle.md)。1000 轮和
24/72 小时测试仍属于压力/长稳验收，不能由 3 轮功能测试替代。

### 阶段 II：同步模式下的局部恢复（L1 已在 0.12.0 实现）

- 单坏帧丢弃；
- 连续超时后执行 V4L2 stream restart；
- stream restart 失败后重开 V4L2 session；
- page flip 超时后重建 DRM session；
- 加入指数退避、时间窗口预算和状态转换日志；
- 恢复期间显示最后一帧、黑帧或故障图，策略必须明确。

验收：人工制造短暂断流后能恢复，永久故障不会忙循环或刷爆日志。

当前 0.12.0 已实现连续 3 次采集超时后的
`STREAMOFF -> QBUF all -> STREAMON`、60 秒最多 3 次的滑动窗口预算、恢复状态日志和
用户态故障注入验收。L2 V4L2 session rebuild、DRM session recovery 和真实硬件断流
验收仍未实现，不能把 L1 完成等同于整个阶段 II 完成。实现说明见
[V4L2 采集流局部恢复](capture_stream_recovery.md)。

### 阶段 III：Atomic KMS 同步后端

- 启用 `DRM_CLIENT_CAP_ATOMIC`；
- 枚举 connector/CRTC/plane 属性，不能写死 property ID；
- 自动选择与 CRTC、格式、旋转能力匹配的 plane；
- 创建 mode blob；
- 先做 `TEST_ONLY`，再做真实 modeset；
- 初期仍保持“一次只允许一个 commit pending”；
- 退出时用 Atomic 请求禁用 plane/CRTC并销毁 blob。

验收：色条和真实相机都通过，非法格式/缩放组合在 TEST_ONLY 阶段被拒绝且不改变屏幕。

### 阶段 IV：统一事件循环和异步显示

- `poll/epoll` 同时监听 V4L2、DRM、signalfd、timerfd；
- Atomic 使用 `NONBLOCK | PAGE_FLIP_EVENT`；
- DRM 目标扩展为三缓冲；
- 增加 `DISPLAY_READY/PENDING/SCANNING/FREE` 状态；
- 实现 latest-frame-wins 和 dropped frame 统计；
- 先保持 RGA `IM_SYNC`，验证显示异步状态机。

验收：任何时刻 buffer 状态唯一，压力下延迟有上界，没有覆盖扫描中 buffer。

### 阶段 V：验证 RGA 异步和显式同步

- 先确认板载 librga 的真实异步 API、返回 fence 语义和错误行为；
- RGA completion 前不 QBUF 源；
- 若驱动支持，向 KMS `IN_FENCE_FD` 传递 RGA fence；
- 正确关闭 fence fd，统计提交/完成时延；
- 不支持可靠 fence 时保留同步 RGA，不为了“异步”牺牲正确性。

验收：RGA、KMS 各有独立 deadline，故障注入后所有 buffer 最终可回收。

### 阶段 VI：服务守护和长稳验收

- BusyBox init 启动 supervisor；
- 心跳包含 capture/RGA/flip 三阶段进度；
- 日志轮转、磁盘上限、版本和配置指纹；
- 24/72 小时长稳；
- 定期记录 RSS、fd 数、温度、FPS、恢复计数和延迟分位数；
- 掉电、反复启动、SIGTERM、worker crash、设备断流等故障矩阵。

## 15. 工业验收矩阵

| 测试 | 操作 | 期望结果 |
| --- | --- | --- |
| 正常停止 | 运行中发送 SIGTERM | 有界时间退出，资源清空，退出码 0 |
| 快速反复启停 | 连续 1000 次启动/停止 | 无 fd/RSS 增长，无残留 DRM client |
| 单坏帧 | 注入/观察 ERROR flag | 丢 1 帧，服务继续，计数增加 |
| 短断流 | 暂停 2..5 秒 | 进入恢复状态，恢复后画面继续 |
| 永久断流 | 摄像头持续不可用 | 有退避，不忙循环，达到预算后报警 |
| RGA 失败 | 模拟 job 返回错误 | 不错误复用 buffer，按策略重建 |
| DRM flip 超时 | 屏蔽/延迟完成事件 | 停止新提交，重建显示或受控退出 |
| 非法 Atomic 配置 | 错误尺寸/plane | TEST_ONLY 失败，屏幕状态不变 |
| supervisor 恢复 | 强制 worker crash | 退避拉起，重启频率受限 |
| 日志容量 | 连续制造错误 | 日志可轮转，不占满根文件系统 |
| 长稳 | 24/72 小时运行 | 无死锁、花屏、资源增长和恢复风暴 |
| 非正常掉电 | 运行中断电再启动 | 能从未知显示状态重新初始化 |

## 16. 新手最容易混淆的问题

### “Atomic KMS 是不是一次只显示一帧？”

不是。Atomic 描述的是一次提交中多个显示属性的事务关系，与帧率和 buffer 数无关。

### “用了 Atomic 就自动异步了吗？”

不会。Atomic commit 可以阻塞；只有使用 NONBLOCK、event/fence 并让上层不立即等待，
才形成异步调度。

### “当前 `drmModePageFlip` 有事件，为什么还叫同步？”

内核接口是异步提交，但项目封装函数提交后马上阻塞等待 event，再返回主循环。因此从
整个 pipeline 的控制方式看仍是同步的。

### “异步是不是一定要三个线程？”

不是。一个 `epoll` 事件循环就能管理多个 fd 和多个硬件任务。线程只是实现选择。

### “三缓冲是不是一定比双缓冲快？”

不一定。三缓冲主要增加调度余量，使 scanning、pending、rendering 可以同时存在；
如果队列策略错误，也可能增加延迟。

### “有 DMA-BUF 就不需要 fence 吗？”

错误。DMA-BUF 解决共享哪块内存，fence 解决什么时候可以安全访问或复用。

### “当前算零拷贝吗？”

从 CPU 角度没有整帧 `memcpy`；但 RGA 会从 V4L2 buffer 读取并写入独立 DRM buffer，
因此存在硬件拷贝/转换。严格的 direct scanout 才是 VOP 直接扫描 capture DMA-BUF。

### “发生任何错误都重启进程最安全吗？”

不一定。重启简单但恢复慢，也可能形成重启风暴。buffer 所有权仍清楚时应优先局部
恢复；只有状态不可证明、资源无法重建或内部不变量破坏时才升级为进程重启。

### “程序崩溃后内核会回收 fd，为什么还要清理设计？”

内核回收不能替代应用层的停止顺序、最终统计、显示恢复、重试节流和故障定位。更重要
的是，工业程序必须证明正常退出和可预期故障不会长期依赖“让进程崩掉”来清理。

## 17. 对当前项目下一步的明确建议

阶段 I 的长期运行、确定性退出和稳定退出码已经在 0.11.0 完成；阶段 II 的第一步
V4L2 L1 stream recovery 和时间窗口预算已在 0.12.0 完成。最合适的下一个小阶段仍
不是立即实现全异步，而是继续完成：

```text
在同步 worker 中加入分级的 V4L2/DRM 局部故障恢复
```

后续开发任务应限制在：

1. stream 重启失败后的 V4L2 session 重建；
2. V4L2 session 重建的指数退避和独立预算；
3. page flip 失败后的 DRM session 重建；
4. 最近恢复原因、最后成功帧时间和周期健康状态；
5. 真实断流、设备消失和 session rebuild 失败注入测试。

完成后再做 Atomic KMS 同步迁移。这样即使 Atomic 开发期间出现配置或驱动错误，也有
成熟的退出和恢复框架承接，而不会把显示状态机、异步 fence 和服务守护一次混在一起。

## 18. 相关文档

- [当前连续同步显示实现](camera_display_stream.md)
- [V4L2 采集流局部恢复](capture_stream_recovery.md)
- [DRM legacy modeset 与 page flip](drm_probe.md)
- [摄像头到 DSI 的硬件/软件完整链路](rk3568_camera_to_dsi_pipeline.md)
- [项目总体架构](architecture.md)
- [板端 Weston/systemui 启停管理](board_desktop_autostart.md)
- [项目开发规范](development_guidelines.md)

进一步阅读：

- [Linux DRM/KMS userspace API](https://docs.kernel.org/gpu/drm-uapi.html)
- [Linux DRM/KMS core](https://docs.kernel.org/gpu/drm-kms.html)
- [V4L2 streaming I/O](https://docs.kernel.org/userspace-api/media/v4l/mmap.html)
- [DMA-BUF synchronization](https://docs.kernel.org/driver-api/dma-buf.html)
