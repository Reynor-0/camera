# 项目开发与代码注释规范

## 1. 适用范围

本规范适用于本项目的全部 C++ 源码、头文件、测试程序和诊断工具。项目运行于
资源和工具链受限的嵌入式开发板，因此兼容性、资源生命周期清晰和硬件接口可诊断
优先于使用较新的语言特性。

本文以 Google C++ Style Guide 的可读性和接口设计原则为基础，并采用 Doxygen
标签描述函数参数和返回值。Google C++ Style Guide 本身不强制使用 `@param`；
本项目为了便于学习 V4L2/DRM API 和后续生成文档，明确要求使用这些标签。

## 2. C++ 语言版本

### 2.1 强制要求

- 项目最高使用 ISO C++11。
- 禁止使用 C++14、C++17、C++20 或更新版本的语言和标准库特性。
- 禁止依赖 `gnu++11` 方言扩展；应以 `-std=c++11` 编译。
- CMake 必须同时设置：

```cmake
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
target_compile_features(target_name PRIVATE cxx_std_11)
```

`CMAKE_CXX_STANDARD_REQUIRED ON` 禁止 CMake 在工具链不支持时静默降级；
`CMAKE_CXX_EXTENSIONS OFF` 禁止 CMake 使用 `gnu++11` 替代 `c++11`。

项目的最终运行平台是使用 64 位 Linux 用户态的 RK3568。提交代码时既要保证本机
构建，也要保证通过 [RK3568 交叉编译指南](cross_compilation_rk3568.md) 中的
toolchain 构建。禁止把宿主机头文件或 x86_64 库加入交叉编译 include/link path。

### 2.2 可以使用的 C++11 特性

- RAII 和标准智能指针。
- `nullptr`。
- `enum class`。
- 范围 `for`。
- 移动语义。
- `= delete` 和 `= default`。
- `override`、`final` 和 `noexcept`。
- 花括号初始化和类内成员初始化。
- `std::array`、`std::vector`、`std::string`。
- `std::unique_ptr` 和 `std::shared_ptr`，但所有权明确时优先 `unique_ptr`。
- `std::thread`、`std::mutex` 等 C++11 并发工具；当前 pipeline 第一版仍优先
  使用单线程事件循环。

### 2.3 禁止使用的常见高版本特性

以下示例不是完整列表。代码评审时只要无法确认目标板 C++11 工具链支持，就不得
引入。

| 特性 | 最低版本 | C++11 替代方案 |
| --- | --- | --- |
| generic lambda 的 `auto` 参数 | C++14 | 写出明确参数类型或普通函数 |
| `std::make_unique` | C++14 | 显式构造 `std::unique_ptr<T>(new T(...))` |
| 结构化绑定 | C++17 | 显式访问成员或使用 `std::tie` |
| `std::optional` | C++17 | 状态码、bool + 输出参数或自定义结果类型 |
| `std::string_view` | C++17 | `const std::string&` 或指针加长度 |
| `if constexpr` | C++17 | 普通分支、重载或模板特化 |
| filesystem 标准库 | C++17 | POSIX API 或项目内兼容封装 |
| designated initializer | C++20 | 构造函数或逐字段赋值 |
| concepts、ranges、`std::span` | C++20 | C++11 模板、迭代器或指针加长度 |

## 3. 文件和模块组织

- `.hpp` 只声明公共接口和必要的数据类型；复杂实现放在 `.cpp`。
- 每个头文件使用 `#pragma once`，不依赖其他头文件偶然包含的符号。
- Linux/V4L2/DRM 原始类型应限制在相应适配层，不向业务 pipeline 大面积泄漏。
- fd、mmap 地址、GEM handle、framebuffer ID 等资源必须有明确唯一所有者。
- 能使用 RAII 自动释放的资源，不依赖调用方记住手动清理。
- 禁止复制拥有 fd 或 mmap 资源的对象；根据需要实现移动构造和移动赋值。
- `main.cpp` 只负责参数解析、模块装配、输出最终结果和顶层错误处理。

## 4. 命名规范

| 对象 | 规范 | 示例 |
| --- | --- | --- |
| 类型、类、枚举 | PascalCase | `V4L2Device`, `VideoFormat` |
| 函数、方法 | lowerCamelCase | `queryCapabilities()` |
| 局部变量、参数 | snake_case | `pixel_format` |
| 类私有成员 | snake_case + `_` | `capture_type_` |
| 编译期常量 | `k` + PascalCase | `kDefaultBufferCount` |
| 宏 | UPPER_SNAKE_CASE | 内核提供的 `V4L2_CAP_STREAMING` |

项目保留 Linux UAPI 的原始命名，例如 `v4l2_format`、`VIDIOC_S_FMT` 和
`V4L2_BUF_TYPE_VIDEO_CAPTURE`，避免自行翻译后难以和内核文档对应。

## 5. 注释规范

### 5.1 总体原则

注释应解释读者仅看代码无法立即知道的内容，重点回答“为什么”和“内核 API 有什么
约束”，不要把语句逐字翻译成中文。

本项目中的“完整注释”是指：

- 每个公共类、结构体、枚举和公共函数都有接口注释。
- 每个公共函数说明用途、参数、返回值、异常和关键所有权约束。
- 每个重要 V4L2/DRM 宏第一次参与逻辑判断时解释它代表的能力或状态。
- 每个 ioctl 说明使用的结构体、成功后的状态变化和允许的结束错误。
- buffer 状态变化必须说明调用前后所有权属于应用、V4L2 还是 DRM。
- 复杂或容易误解的内部辅助函数也使用同样格式。
- 显而易见的 getter、析构函数可以使用简短的一行接口注释。

注释必须随着实现更新。过期注释按缺陷处理，不能保留与代码不一致的描述。

### 5.2 公共函数注释模板

公共接口使用 `/** ... */`，采用以下标签：

```cpp
/**
 * @brief 查询并验证 V4L2 设备能力。
 *
 * 这里写调用顺序、Linux API 语义、资源所有权或其他不能从函数名直接得出的信息。
 *
 * @param capability VIDIOC_QUERYCAP 返回的设备能力结构体。
 * @param required_caps 调用方要求设备具备的 V4L2_CAP_* 位集合。
 * @return 当前 video node 实际生效的能力位集合。
 * @throws std::runtime_error ioctl 失败或设备缺少必要能力时抛出。
 */
```

要求：

- `@brief` 使用一句话说明函数对调用方产生的结果。
- 每个具名参数都必须有一个 `@param`。
- 非 `void` 函数必须有 `@return`。
- 函数可能抛出异常时写 `@throws`；明确 `noexcept` 的函数不写。
- 参数描述不仅重复类型，还要说明单位、合法范围和所有权。
- 指针可能为空时必须写明；未写明则默认不允许为空。
- 返回引用或指针时必须说明生命周期和所有权。

### 5.3 类和结构体注释

类注释至少说明：

- 它代表什么系统对象。
- 拥有哪些资源。
- 是否可复制或移动。
- 线程安全性；未特别声明的对象默认不是线程安全的。
- 对象在正常使用中的关键状态顺序。

结构体字段应标明单位和语义，例如：

```cpp
/** 每个 memory plane 一行占用的字节数，包含硬件对齐填充。 */
std::uint32_t bytes_per_line;
```

### 5.4 实现注释

函数内部使用 `//` 解释紧邻代码的约束：

```cpp
// VIDIOC_ENUM_FMT 使用 EINVAL 表示 index 已越过最后一个格式，这是正常的
// 枚举结束条件，不能作为设备错误抛出。
if (errno == EINVAL) {
    break;
}
```

不要写没有额外信息的注释：

```cpp
// 错误：给 index 加一。
++index;
```

### 5.5 `TODO` 格式

`TODO` 必须包含责任人或可追踪事项，并描述完成条件：

```cpp
// TODO(camera-buffer): V4L2 MMAP 采集稳定后，实现每个 memory plane 的 EXPBUF。
```

禁止使用没有上下文的 `// TODO: fix this`。

## 6. V4L2/DRM 专用注释要求

V4L2 和 DRM 的很多名字看起来相似，但语义不同。涉及以下内容时必须在接口或首次
实现处说明：

### 6.1 能力宏

- `V4L2_CAP_VIDEO_CAPTURE`：传统 single-planar capture API，格式使用
  `v4l2_pix_format`。
- `V4L2_CAP_VIDEO_CAPTURE_MPLANE`：multi-planar capture API，格式使用
  `v4l2_pix_format_mplane`，buffer 使用 `v4l2_plane[]`。
- `V4L2_CAP_STREAMING`：支持 `REQBUFS/QBUF/DQBUF/STREAMON/STREAMOFF`。
- `V4L2_CAP_DEVICE_CAPS`：当前 node 的能力需要从 `device_caps` 读取。

这里的 single/multi-planar 指 memory plane 数量，不等同于图像颜色分量数量。
例如 NV12 可以是一个 DMA-BUF 内含 Y/UV 两个 image plane，也可以是两个独立的
memory plane。

### 6.2 ioctl

每个 ioctl 的注释至少覆盖：

- 输入结构体和必须预先填写的字段。
- 驱动会回填或修改哪些字段。
- `type`、`memory`、plane count 等字段必须与哪一步保持一致。
- 哪些 errno 表示正常流程，例如枚举结束时的 `EINVAL`、非阻塞 DQBUF 的
  `EAGAIN`。
- 调用成功后 buffer 或设备的所有权/状态如何变化。

### 6.3 Buffer 所有权

注释统一使用以下术语：

- `QueuedToCapture`：已经 QBUF，buffer 属于 V4L2 驱动，应用不得读写。
- `Ready`：已经 DQBUF，V4L2 写入完成，buffer 可交给显示模块。
- `PendingScanout`：已提交 atomic commit，等待 flip-complete。
- `ScanningOut`：显示控制器正在读取，不能重新 QBUF 给摄像头。

收到 DRM flip-complete 之前，不得把旧 scanout buffer 重新交给 V4L2。相关代码
必须在状态转换处重复说明这一硬件并发约束。

### 6.4 DMA-BUF fd 所有权

- `VIDIOC_EXPBUF` 成功返回的 fd 归调用进程所有，必须且只能由一个明确对象关闭。
- 本项目由 `V4L2BufferQueue` 拥有原始导出 fd。`CapturedPlane::dma_buf_fd` 和
  `dmaBufFd()` 只提供借用值，调用方不得 `close()`。
- 如果消费者需要比 `V4L2BufferQueue` 活得更久，必须先通过 `dup()` 创建自己的
  fd，并由消费者关闭副本。
- DMA-BUF fd 与 mmap 虚拟地址是同一底层 buffer 的两种引用，不代表发生了一次
  图像数据复制。
- 销毁 capture pool 时，先停止所有使用者，再关闭导出 fd、`munmap()`，最后执行
  `VIDIOC_REQBUFS(count=0)`。DRM 导入后还必须先删除 framebuffer、关闭 GEM handle。

## 7. 错误处理与日志

- 系统调用失败信息必须包含操作名、设备路径、`strerror(errno)` 和 errno 数值。
- ioctl 遇到 `EINTR` 应通过统一包装函数重试。
- 不使用裸整数作为模糊错误码；可恢复状态和致命错误必须区分。
- 枚举 ioctl 返回 `EINVAL` 通常表示枚举结束，必须按对应 UAPI 语义处理。
- 非阻塞 `DQBUF` 返回 `EAGAIN` 表示暂时没有帧，不应退出程序。
- 顶层 `main()` 捕获异常、打印上下文并返回非零退出码。
- 错误路径同样必须满足 fd、mmap、framebuffer 等资源的释放顺序。

## 7.1 命令行程序的自描述要求

交付可执行文件时，用户可能看不到源码和项目文档。每个面向用户的命令行程序必须
提供：

```bash
program --help
program -h
program --version
```

- `-h/--help` 必须在访问设备或文件之前处理，向 stdout 输出完整用法并返回 0。
- `--version` 必须向 stdout 输出程序版本和目标架构并返回 0。
- 参数错误向 stderr 输出原因、返回非零，并提示用户运行 `--help`。
- 帮助中必须包含模式说明、全部参数、默认值、单位和至少一个可运行示例。
- 帮助和版本命令不得要求 `/dev/video*` 存在或具有设备权限。

## 8. 编译与提交前检查

普通构建：

```bash
cmake -S . -B build
cmake --build build -j
```

提交前至少执行一次严格告警构建：

```bash
cmake -S . -B build-strict \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion"
cmake --build build-strict -j
```

检查生成的编译命令，必须出现 `-std=c++11`，不能出现 `gnu++11` 或更高标准：

```bash
rg -- "-std=" build/compile_commands.json
```

目标板功能验证需要记录：

- 编译器和内核版本。
- video node、驱动名称和 capture API 类型。
- 请求格式与驱动实际返回格式。
- 每个 memory plane 的 stride 和 sizeimage。
- 失败 ioctl 的 errno 和完整日志。

没有真实摄像头的开发主机，应按照
[虚拟摄像头测试指南](virtual_camera_testing.md) 使用 vivid 同时覆盖 single-planar
和 multi-planar API。虚拟设备测试通过不能替代 RK3568 实板测试。

修改 V4L2 buffer queue 后，至少运行：

```bash
./tools/test_virtual_v4l2_probe.sh
```

该测试不仅枚举格式，还必须实际完成 MMAP streaming、每个 memory plane 的
DMA-BUF 导出，以及固定帧数的 QBUF/DQBUF 循环；只有 probe 输出正确不能视为
buffer queue 验收通过。

RK3568 交叉构建还需要检查：

- [ ] 使用独立的 `build-rk3568`，没有复用本机 build cache。
- [ ] 编译器目标为 AArch64，而不是宿主机 x86_64。
- [ ] sysroot 与开发板正在运行的 BSP/rootfs 版本匹配。
- [ ] `readelf -h` 中的 Machine 为 AArch64。
- [ ] 板端 `ldd` 没有缺失或版本不兼容的动态库。
- [ ] 交叉构建仍使用 `-std=c++11`。

## 9. 代码评审清单

- [ ] 代码只使用 ISO C++11。
- [ ] 新 target 明确声明 `cxx_std_11`。
- [ ] 公共接口具有 `@brief/@param/@return/@throws` 注释。
- [ ] 新使用的 V4L2/DRM 宏已说明语义。
- [ ] ioctl 参数结构体已清零，reserved 字段未留垃圾值。
- [ ] single-planar 与 multi-planar 分支没有混用 union 成员。
- [ ] 使用驱动返回的 actual format，而不是请求值。
- [ ] fd、mmap、DMA-BUF、GEM handle 和 framebuffer ID 所有权明确。
- [ ] buffer 只有在消费者使用完成后才返回生产者。
- [ ] 错误日志包含操作、设备、errno 和可读原因。
- [ ] 严格告警构建通过。
