# V4L2 DMA-BUF 导出开发计划

## 1. 本阶段要解决什么问题

当前程序已经能够通过 V4L2 MMAP 连续采集。下一步不是把图像从 MMAP 内存再复制
到一块所谓的“DMA-BUF 内存”，而是通过 `VIDIOC_EXPBUF` 为同一块 V4L2 capture
buffer 创建一个可跨驱动传递的 fd：

```text
V4L2/ISP 分配的同一块底层存储
├── mmap 虚拟地址：CPU/当前进程读取 metadata 或像素
└── DMA-BUF fd：交给 DRM PRIME、RGA 或 GPU 等内核设备导入
```

因此，本阶段的验收目标只有两个：

1. 所有 V4L2 buffer 的所有 memory plane 都能成功导出 fd。
2. 导出后原有的 `QBUF -> STREAMON -> DQBUF -> QBUF` 采集循环仍然正常。

本阶段不创建 DRM framebuffer，也不声称已经完成跨设备零拷贝显示。vivid 可以验证
V4L2 API 和 fd 生命周期，但不能证明 RK3568 的 RKISP buffer 能被 RKVOP 成功导入。

## 2. 接口与项目结构

本阶段继续保留现有的小型工程结构：

```text
inc/v4l2_buffer.hpp       对外接口、帧视图和所有权注释
src/v4l2_buffer.cpp       VIDIOC_EXPBUF、fd 保存和清理
src/main.cpp              --export-dmabuf 参数及诊断输出
tools/test_virtual_v4l2_probe.sh
                          vivid single/multi-planar 自动验证
docs/dma_buf_export_plan.md
                          本阶段边界、顺序和验收方法
```

暂时不新建 DRM 类。只有导出阶段在 x86 和 RK3568 上验证完成后，再加入
`DrmDevice`、`DrmFramebuffer` 和显示状态机，避免采集错误与显示错误混在一起。

## 3. 开发任务表

| ID | 状态 | 功能点 | 完成条件 |
| --- | --- | --- | --- |
| E01 | `[x]` | 为每个 MMAP plane 执行 `VIDIOC_EXPBUF` | single/multi-planar 使用正确的 index 和 plane |
| E02 | `[x]` | DMA-BUF fd 生命周期管理 | 异常回滚和析构均关闭全部导出 fd |
| E03 | `[x]` | 帧模型公开借用 fd | `CapturedPlane` 能关联 mmap 地址与 DMA-BUF fd |
| E04 | `[x]` | 命令行入口 | `--export-dmabuf` 无需源码即可从 `--help` 发现 |
| E05 | `[x]` | x86_64 严格编译 | ISO C++11 且 warning-as-error 构建通过 |
| E06 | `[ ]` | vivid 功能验证 | single/multi-planar 均导出并连续采集 100 帧 |
| E07 | `[ ]` | RK3568 交叉构建 | AArch64 ELF、板端依赖完整 |
| E08 | `[ ]` | RK3568 RKISP 验证 | 真实节点全部 plane 导出并连续采集至少 10 分钟 |
| E09 | `[ ]` | fd 泄漏验证 | 循环运行和正常退出后 fd 数量稳定 |

`[x]` 表示代码或主机构建已完成；需要特定设备的任务只有在相应环境实际运行后才能
勾选，不能用编译通过代替。

## 4. 实现顺序和关键语义

### 4.1 初始化顺序

```text
open video node
-> VIDIOC_QUERYCAP
-> VIDIOC_S_FMT / VIDIOC_G_FMT
-> VIDIOC_REQBUFS(V4L2_MEMORY_MMAP)
-> 对每个 buffer 执行 VIDIOC_QUERYBUF + mmap
-> 对每个 buffer/plane 执行 VIDIOC_EXPBUF
-> 将所有 buffer VIDIOC_QBUF
-> VIDIOC_STREAMON
```

`VIDIOC_EXPBUF` 的 `type` 必须与 `REQBUFS` 一致，`index` 是 capture pool 的 buffer
编号，`plane` 是 memory plane 编号。single-planar API 的 `plane` 固定为 0；
multi-planar API 逐 plane 导出。

当前仍然以 `V4L2_MEMORY_MMAP` 执行 `QBUF/DQBUF`。`V4L2_MEMORY_DMABUF` 表示应用
从外部导入 buffer 给 V4L2 使用，是另一种分配方向，不应在本方案中混用。

### 4.2 每帧路径

```text
poll(video fd)
-> VIDIOC_DQBUF 得到 buffer index
-> 用 index 找到 mmap 地址和已导出的 DMA-BUF fd
-> 当前阶段只打印 metadata
-> VIDIOC_QBUF 立即把 buffer 交还驱动
```

DMA-BUF fd 在每一帧中不会重新创建。它与 buffer pool 的 `index/plane` 一一对应，
从申请完成一直存在到 queue 被销毁。DQBUF/QBUF 改变的是 buffer 的使用所有权，不是
fd 的数值。

### 4.3 清理顺序

```text
VIDIOC_STREAMOFF
-> close 所有 VIDIOC_EXPBUF fd
-> munmap 所有 CPU 映射
-> VIDIOC_REQBUFS(count=0)
-> V4L2Device 析构并 close video fd
```

以后接入 DRM 后，关闭 DMA-BUF fd 之前还要先删除 DRM framebuffer、关闭 GEM
handle，并确保显示控制器已经不再 scanout。否则即使应用侧 fd 已关闭，其他内核
引用仍可能让 buffer 保持存活或使释放顺序难以诊断。

## 5. 本机验证

先构建并加载 vivid：

```bash
cmake -S . -B build -DCAMERA_DEMO_WARNINGS_AS_ERRORS=ON
cmake --build build -j
sudo ./tools/virtual_camera_modules.sh load
./tools/test_virtual_v4l2_probe.sh
```

也可以只运行一个节点：

```bash
./build/camera_demo --capture /dev/video0 \
  --width 1920 --height 1080 --format NV12 \
  --buffers 4 --frames 100 --timeout-ms 2000 \
  --export-dmabuf
```

预期先看到每个 buffer/plane 的 fd，例如：

```text
DMA-BUF exports:
  buffer=0 plane=0 fd=4
  buffer=1 plane=0 fd=5
```

fd 数值由进程运行时分配，不要求与示例相同，也不是物理地址。随后每一帧中相同
buffer/plane 应报告相同 fd，最后出现 `Capture complete`。

如果 `VIDIOC_EXPBUF` 返回 `EINVAL` 或 `ENOTTY`，通常说明该驱动/queue 不支持导出、
buffer 不是 MMAP 分配，或者传入的 type/plane 不正确。应保留完整 errno 日志并用
`v4l2-ctl` 对照驱动能力，不能把失败静默降级为“已经零拷贝”。

## 6. RK3568 验收

按照[RK3568 交叉编译指南](cross_compilation_rk3568.md)生成独立构建目录，部署后记录：

- 内核、BSP 和编译器版本。
- 实际 `/dev/video*` 节点及 single/multi-planar API。
- actual fourcc、分辨率、stride、sizeimage 和 memory plane 数。
- 每个 buffer/plane 是否成功得到 DMA-BUF fd。
- 连续 10 分钟的帧数、错误帧、超时和 sequence gap。
- 退出前后 `/proc/<pid>/fd` 或循环启动测试是否发现 fd 泄漏。

只有 E06 至 E09 都完成后，才开始下一阶段的 DRM PRIME import。下一阶段先做“导入
一个 DMA-BUF、创建 framebuffer、静态显示并保持 buffer 不 QBUF”，再实现实时回收
状态机。
