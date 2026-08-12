# 下一阶段开发计划：V4L2 MMAP 连续采集

## 1. 阶段目标

在不引入 DMA-BUF 和 DRM/KMS 的前提下，让 `camera_demo` 能从 single-planar 和
multi-planar V4L2 capture node 连续取得测试帧：

```text
setFormat
-> VIDIOC_REQBUFS(V4L2_MEMORY_MMAP)
-> VIDIOC_QUERYBUF + mmap
-> VIDIOC_QBUF(all)
-> VIDIOC_STREAMON
-> poll
-> VIDIOC_DQBUF
-> 读取 metadata/测试图案
-> VIDIOC_QBUF
-> VIDIOC_STREAMOFF
-> munmap
```

开发主机使用 vivid 同时覆盖两种 capture API，完成后再到 RK3568 验证真实 RKISP
节点。本阶段只读取 mmap 数据和帧元数据，不导出 DMA-BUF，也不向屏幕显示。

## 2. 本阶段不包含的范围

- `VIDIOC_EXPBUF` 和 DMA-BUF fd。
- DRM PRIME、GEM handle 和 framebuffer。
- Atomic KMS/page flip。
- GPU/CPU 像素格式转换。
- 多线程 capture/display pipeline。
- IMX415、DPHY、RKISP media topology 配置。

这些功能只有在 MMAP QBUF/DQBUF 循环稳定后才能进入，避免把采集错误和显示错误
混在一起定位。

## 3. 代码结构

实现位于：

- `inc/v4l2_buffer.hpp`
- `src/v4l2_buffer.cpp`

核心对象为 `V4L2BufferQueue`：

```text
V4L2Device                  V4L2BufferQueue
├── owns video fd           ├── borrows video fd
├── QUERYCAP                ├── owns MMAP mappings
├── ENUM_FMT                ├── owns buffer queue state
└── S_FMT/G_FMT             └── does not close video fd
```

生命周期约束：`V4L2Device` 必须在 `V4L2BufferQueue` 析构之后才销毁。推荐局部对象
声明顺序为：

```cpp
V4L2Device device(path);
const VideoFormat format = device.setFormat(width, height, fourcc);
V4L2BufferQueue queue(device, format);
```

C++ 按声明的逆序析构，因此 queue 会先释放 mappings，device 最后关闭 fd。

MMAP 采集方法已经实现并接入命令行。T2～T11 仍保持“进行中”，直到在实际 vivid
节点完成运行验收；通过后应同步更新下表状态，不能只以编译成功替代运行验证。

## 4. 开发任务计划表

状态含义：`[ ]` 未开始、`[-]` 进行中、`[x]` 已完成并通过对应验收。

| ID | 状态 | 功能点 | 主要位置 | 完成验收 |
| --- | --- | --- | --- | --- |
| T0 | `[x]` | 定义 queue 状态、帧 metadata、plane view 和所有权契约 | `v4l2_buffer.hpp` | C++11 严格构建通过，公共接口注释完整 |
| T1 | `[x]` | 建立 RAII 骨架、借用 fd、析构 best-effort 清理 | `v4l2_buffer.cpp` | 析构不抛异常，未申请 buffer 时安全 |
| T2 | `[-]` | 使用 `VIDIOC_REQBUFS` 申请 MMAP buffer | `requestBuffers()` | 实现与严格构建已完成；待 vivid 验证 actual count 和错误路径 |
| T3 | `[-]` | 对 single/multi-planar buffer 执行 QUERYBUF 和 mmap | `requestBuffers()` | 两套 API 与失败回滚已实现；待 vivid 两个实例运行验证 |
| T4 | `[-]` | 将全部空 buffer QBUF | `queueAll()` | 实现与状态检查已完成；待 vivid 验证重复/部分失败路径 |
| T5 | `[-]` | 启动 streaming | `start()` | 实现已完成；待 vivid STREAMON/STREAMOFF 验证 |
| T6 | `[-]` | 使用 poll 等待帧 | `waitForFrame()` | timeout/EINTR/revents 已实现；待运行验证 |
| T7 | `[-]` | DQBUF 并生成 `CapturedFrame` | `tryDequeue()` | 两套 metadata 解析已实现；待运行验证 |
| T8 | `[-]` | 处理后重新 QBUF | `requeue()` | 所有权检查已实现；待连续采集 1000 帧 |
| T9 | `[-]` | 正常 STREAMOFF、munmap 和失败回滚 | `stop()`/析构 | 实现已完成；待 Ctrl+C 和泄漏验证 |
| T10 | `[-]` | 将采集模式接入 `main.cpp` | `main.cpp` | CLI 与 Ctrl+C 已实现；待 vivid 运行验证原有模式兼容性 |
| T11 | `[-]` | vivid 自动化测试 | `tools/` | 自动 probe/capture 已实现；待 single/multi-planar 各运行通过 |
| T12 | `[ ]` | RK3568 实板验证 | 目标板日志 | RKISP 连续采集 10 分钟，无卡死、泄漏或异常帧风暴 |

## 5. 推荐提交顺序

一次只完成一个可验证闭环：

1. **提交 A：T2 + T3**
   - 只申请、映射、立即释放。
   - 用 `/proc/<pid>/maps` 或日志核对地址和长度。
   - 故意在第 N 个 mmap 后注入失败，验证已映射 plane 全部回滚。
2. **提交 B：T4 + T5 + T9**
   - 所有 buffer QBUF 后启动，再立即 STREAMOFF。
   - 尚不 DQBUF，专门验证生命周期和退出路径。
3. **提交 C：T6 + T7 + T8**
   - 完成 `poll -> DQBUF -> QBUF` 循环。
   - 先采 10 帧，再提升到 1000 帧。
4. **提交 D：T10 + T11**
   - 接入命令行和自动化测试。
   - 固化 single/multi-planar 回归测试。
5. **提交 E：T12**
   - 交叉编译并部署到 RK3568。
   - 保存 capability、actual format 和持续采集统计。

## 6. T2/T3：申请和映射 buffer

### 6.1 REQBUFS

single-planar 和 multi-planar 都使用同一个 `v4l2_requestbuffers`：

```cpp
v4l2_requestbuffers request{};
request.count = requested_count;
request.type = capture_type_;
request.memory = V4L2_MEMORY_MMAP;
```

注意：

- `V4L2_MEMORY_MMAP` 表示 buffer 由 V4L2 驱动分配，应用随后 mmap。
- 驱动返回的 `request.count` 才是真实数量。
- count 返回 0 是失败，不能继续 QUERYBUF。
- 第一版建议请求 4 个，但不得假定驱动一定返回 4 个。
- 如果后续 allocation 中途失败，需要 munmap 已成功部分，并使用
  `REQBUFS(count=0)` 释放内核 buffers。

### 6.2 single-planar QUERYBUF

```cpp
v4l2_buffer buffer{};
buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
buffer.memory = V4L2_MEMORY_MMAP;
buffer.index = index;
```

QUERYBUF 成功后：

- `buffer.m.offset` 是传给 mmap 的 offset。
- `buffer.length` 是 mmap 长度。
- 一个 V4L2 memory plane 可以包含 NV12 的 Y/UV 两个 image planes。

### 6.3 multi-planar QUERYBUF

```cpp
v4l2_buffer buffer{};
v4l2_plane planes[VIDEO_MAX_PLANES]{};
buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
buffer.memory = V4L2_MEMORY_MMAP;
buffer.index = index;
buffer.m.planes = planes;
buffer.length = plane_count_;
```

QUERYBUF 成功后，每个 `planes[p].m.mem_offset` 和 `planes[p].length` 分别用于一次
mmap。不能使用 single-planar 的 `buffer.m.offset`。

## 7. T4/T5：排队和启动

每个 buffer 初始由应用持有。QBUF 成功后的所有权变化为：

```text
Application-owned --VIDIOC_QBUF--> Driver-owned
```

必须先把所有 buffer 排入 queue，再执行：

```cpp
v4l2_buf_type type = capture_type_;
ioctl(fd_, VIDIOC_STREAMON, &type);
```

如果只 QBUF 一部分便发生错误，需要明确记录哪些 slot 已交给驱动。最简单的回滚
方式是对该 queue 执行 STREAMOFF/REQBUFS(0)，而不是假装所有 slot 仍由应用持有。

## 8. T6/T7/T8：事件循环

推荐第一版单线程循环：

```text
waitForFrame(2000 ms)
    ├── timeout: 统计并决定重试或退出
    └── readable: tryDequeue()
                     ├── 打印 index/sequence/timestamp/bytesused
                     ├── 可选检查测试图案数据
                     └── requeue(index)
```

必须处理：

- `poll()` 返回 `EINTR`：重试或检查退出信号。
- `poll()` 返回 0：这是正常超时，不是系统错误。
- `POLLERR/POLLHUP/POLLNVAL`：报告为设备/stream 错误。
- 非阻塞 `VIDIOC_DQBUF` 返回 `EAGAIN`：可能发生竞争，应作为暂时无帧处理。
- `V4L2_BUF_FLAG_ERROR`：帧可 dequeue/requeue，但应增加 error-frame 统计。
- `buffer.index >= buffers_.size()`：驱动或应用状态异常，禁止访问 vector。
- multi-planar 的 `buffer.length` 不得超过 `plane_count_` 和
  `VIDEO_MAX_PLANES`。

## 9. T9：退出与错误回滚

正常释放顺序：

```text
VIDIOC_STREAMOFF（仅 Streaming 状态）
-> munmap 每个成功映射的 plane
-> VIDIOC_REQBUFS(count=0) 释放内核 buffers
-> 清空 slots
-> queue 状态回到 Idle
-> V4L2Device 最后 close(fd)
```

queue 析构函数只能 best-effort 清理，因为 C++ 析构函数不应抛异常。正常路径必须显式
调用 `stop()`，以便把 STREAMOFF 错误报告给调用方。实现 T9 时建议额外提供一个
显式 `releaseBuffers()`，或者让 `requestBuffers()` 的失败回滚和析构共用同一个
私有清理函数。

## 10. T10：命令行接口建议

保留当前两种用法：

```bash
camera_demo /dev/video0
camera_demo /dev/video0 1920 1080 NV12
```

程序同时提供不访问设备的自描述接口：

```bash
camera_demo --help
camera_demo -h
camera_demo --version
```

采集模式可以增加明确选项，避免继续堆叠位置参数：

```bash
camera_demo --capture /dev/video0 \
  --width 1920 \
  --height 1080 \
  --format NV12 \
  --buffers 4 \
  --frames 100 \
  --timeout-ms 2000
```

第一版至少输出：

```text
frame=0 buffer=2 sequence=17 timestamp=... flags=... planes=1
  plane=0 bytesused=3110400 data_offset=0 mapped=3110400
```

结束时汇总 captured、error、timeout 和 sequence-gap 数量。

## 11. vivid 测试矩阵

先加载本项目创建的两个 vivid 实例：

```bash
sudo ./tools/virtual_camera_modules.sh load
./tools/virtual_camera_modules.sh status
```

| 场景 | 节点 | 示例格式 | 预期结果 |
| --- | --- | --- | --- |
| single-planar | `vivid-000-vid-cap` | `NV12 1920x1080` | 1 memory plane，1000 帧循环稳定 |
| multi-planar | `vivid-001-vid-cap` | 从枚举选择 `NM16/NM61` 等 | 多 memory plane，bytesused 分别正确 |
| timeout | 任一 vivid 节点 | 短 timeout/暂停 stream | 返回 false 或清晰超时统计 |
| invalid index | 单元测试 | `bufferCount()` 之外 | 抛出 `std::out_of_range` |
| duplicate requeue | 单元测试 | 同一 index 连续两次 | 第二次被状态检查拒绝 |
| early exit | 任一 vivid 节点 | 采集过程中 Ctrl+C | STREAMOFF、munmap，无资源泄漏 |

设备编号会受系统其他视频设备影响，测试脚本必须根据 `/sys/class/video4linux/*/name`
查找 vivid，而不是假定永远是 `/dev/video0` 和 `/dev/video1`。

## 12. RK3568 验收记录模板

```text
Board/kernel:
Cross compiler:
Video node:
Driver/card/bus:
Capture API:
Requested format:
Actual format:
Buffer count requested/actual:
Plane stride/size:
Test duration:
Captured frames:
Error frames:
Timeouts:
Sequence gaps:
RSS/fd before and after:
Result:
```

## 13. 完成定义

只有同时满足以下条件，才能进入 DMA-BUF 导出阶段：

- 所有 T0～T12 任务完成或明确记录不适用原因。
- x86_64 严格 ISO C++11 构建无警告。
- vivid single-planar 和 multi-planar 各连续采集至少 1000 帧。
- RK3568 实际 capture node 连续采集至少 10 分钟。
- 正常退出、超时和初始化失败路径均无 mmap/fd 泄漏。
- 代码注释说明所有 ioctl 的输入、输出、errno 和 buffer 所有权变化。
- 尚未把 DMA-BUF/DRM 逻辑耦合进 `V4L2BufferQueue`。
