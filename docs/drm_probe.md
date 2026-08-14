# DRM/KMS 资源探测、Dumb Buffer 与单帧 Modeset

## 1. 阶段目标与边界

`drm_probe` 目前提供三个彼此独立的运行模式：

- 默认模式只查询 DRM driver、dumb-buffer capability、connector、preferred mode、
  encoder 和 CRTC。
- `--test-dumb-buffer` 在完成上述查询后，额外创建、映射、写入、校验并销毁一个
  XRGB8888 dumb framebuffer。
- `--show-color-bars` 在桌面程序已停止的前提下取得 DRM master，把 XRGB8888 色条
  framebuffer 绑定到 CRTC，保持指定时间后关闭 CRTC 并释放资源。

前两种模式可以在 Weston 运行期间使用。第三种模式会真正改变显示状态，必须通过
板端受控脚本停止 Weston、systemui 和厂商 camera 后运行。

## 2. 代码组织

```text
inc/drm_device.hpp      DRM fd 与 connector/mode/encoder/CRTC 探测接口
src/drm_device.cpp      libdrm 查询、自动选择和查询对象的 RAII 清理
inc/drm_display.hpp     Dumb framebuffer 与独占 CRTC 会话接口
src/drm_display.cpp     buffer 生命周期、legacy modeset 和逆序清理
src/drm_test_main.cpp   命令行解析、结果输出与小阶段测试流程
tools/run_drm_color_bars_rk3568.sh  板端桌面停止、测试和恢复编排
```

connector、encoder、CRTC 和 framebuffer object ID 都由内核动态分配或由程序自动
选择，不能把某一次启动得到的数值写死到代码中。

## 3. Dumb Buffer 创建流程

```text
DRM_IOCTL_MODE_CREATE_DUMB
        |
        | 返回 GEM handle、pitch、size
        v
DRM_IOCTL_MODE_MAP_DUMB
        |
        | 返回用于 mmap 的 offset
        v
mmap(MAP_SHARED)
        |
        | 用户态写入 XRGB8888 色条
        v
drmModeAddFB2
        |
        | 返回 KMS framebuffer ID，但本阶段不绑定 CRTC
        v
checksum
```

各对象的含义如下：

| 对象 | 含义 | 是否是普通内存地址 |
| --- | --- | --- |
| GEM handle | 当前 DRM fd 内引用 buffer 的句柄 | 否 |
| `pitch` | 相邻两行起始地址的实际字节间隔 | 否 |
| `size` | 驱动实际分配和允许映射的总字节数 | 否 |
| mmap 地址 | CPU 访问 buffer 的用户态虚拟地址 | 是 |
| framebuffer ID | KMS 用来描述尺寸、格式和 GEM 存储的对象 ID | 否 |

必须使用驱动返回的 `pitch` 和 `size`。例如实板宽度为 1080，XRGB8888 可见数据每行
为 `1080 * 4 = 4320` 字节，但 Rockchip 驱动返回的 `pitch` 是 4352 字节；每行末尾
有 32 字节对齐填充。如果代码直接使用 4320 作为行间隔，图像将逐行错位。

## 4. 资源所有权与清理顺序

`DrmDumbFramebuffer` 借用 `DrmDevice` 的 fd，但独占以下资源：

```text
framebuffer ID -> mmap 地址 -> GEM handle
```

正常路径显式调用 `release()`，以相反顺序释放：

```text
drmModeRmFB -> munmap -> DRM_IOCTL_MODE_DESTROY_DUMB
```

任何构造步骤失败时，已经取得的资源也会按相同顺序回滚。清理某一步失败不会阻止
后续步骤继续执行；`release()` 会在全部清理尝试完成后报告第一个错误。析构函数只
作为不抛异常的兜底。

## 5. 单帧 Modeset 流程

```text
停止 systemui/camera/Weston
        |
        v
创建并填充 XRGB8888 framebuffer
        |
        v
确认 CRTC 状态 + 取得 DRM master
        |
        v
drmModeSetCrtc(framebuffer + connector + full mode timing)
        |
        | 固定色条持续 1 至 300 秒，可用 Ctrl-C 提前结束
        v
drmModeSetCrtc(framebuffer=0) -> RmFB -> munmap -> DESTROY_DUMB
        |
        v
释放 master，按需重启 Weston/systemui
```

本阶段使用 legacy `drmModeSetCrtc`，尚未实现 atomic commit、page flip 或 DRM event。
静态 framebuffer 在保持期间不发生交换，因此不需要 event loop。

### 5.1 为什么还需要显式确认桌面已停止

RK3568 的 4.19 BSP 实测存在两个厂商行为：

- Weston 正常退出后，CRTC 仍可能保持最后一帧为 active；所以不能仅凭 active 状态
  判断 compositor 仍在运行。
- 即使 debugfs 仍把 Weston 标记为 master，另一个 root 客户端也可能成功执行 legacy
  modeset；所以不能仅依赖 `drmSetMaster()` 失败作为互斥保护。

因此命令必须带 `--confirm-desktop-stopped`，而推荐入口脚本还会使用 `pidof weston`
确认进程已退出。程序先查询 CRTC，再触碰 master，避免拒绝路径影响正在运行的桌面。

## 6. 构建与部署

普通 x86 开发机缺少 libdrm 开发文件时，CMake 保留 `camera_demo` 并跳过
`drm_probe`。RK3568 交叉构建将该目标设为必需：

```bash
./tools/cross_build_rk3568.sh
./tools/deploy_rk3568.sh
```

构建产物位于 `build-rk3568/stage/bin/drm_probe`。部署后板端文件为：

```text
/home/reynor/drm_probe
/home/reynor/run_drm_color_bars_rk3568.sh
```

不能使用 `/tmp` 作为最终运行目录。

## 7. 板端命令

只读探测：

```bash
/home/reynor/drm_probe /dev/dri/card0
```

Dumb Buffer 生命周期测试：

```bash
/home/reynor/drm_probe --test-dumb-buffer /dev/dri/card0
```

查看完整用法：

```bash
/home/reynor/drm_probe --help
```

推荐的独占色条测试会自动停止并恢复桌面：

```bash
/home/reynor/run_drm_color_bars_rk3568.sh 5
```

只有明确准备让桌面保持停止时才使用：

```bash
/home/reynor/run_drm_color_bars_rk3568.sh 5 --keep-desktop-stopped
```

底层命令不负责停止服务，只应由已经确认桌面停止的脚本或工程师调用：

```bash
/home/reynor/drm_probe \
  --show-color-bars 5 \
  --confirm-desktop-stopped \
  /dev/dri/card0
```

## 8. RK3568 实板结果

本阶段在 Weston、systemui 和厂商 camera 进程仍运行时连续执行两次通过：

```text
Format: XRGB8888
Resolution: 1080x1920
Pitch: 4352 bytes
Size: 8355840 bytes
GEM handle: 1
Framebuffer ID: 166
Checksum: 0x6345ae94419ea325
Bound to CRTC: no
Cleanup: complete
```

两次 checksum 相同，证明 mmap 区域的写入和读取结果稳定。第二次仍获得 fd-local
GEM handle 1 和 framebuffer ID 166，说明第一次退出前相关对象已释放，内核可以
复用编号；编号复用本身不是全局唯一性保证。

独占显示测试保持 5 秒并正常退出：

```text
Color bars are now visible:
  Framebuffer ID: 167
  CRTC ID: 115
  Connector ID: 163
  Duration: 5 seconds maximum
Display cleanup: CRTC safely disabled; restart Weston
```

显示期间 `/sys/kernel/debug/dri/0/clients` 只有 `drm_probe` 且 `master=y`；程序退出
码为 0，退出后 DRM client 表为空，证明 fd/master 没有遗留。之后已重新启动 Weston
和 systemui，并从新的 ADB 会话确认进程及 1080x1920 CRTC 正常。

## 9. 完成状态与下一步

- [x] ISO C++11，并通过 `-Wall -Wextra -Wpedantic -Werror` 交叉构建。
- [x] 自动选择 connected connector、preferred mode 和 compatible CRTC。
- [x] 创建 1080x1920 XRGB8888 dumb buffer。
- [x] 使用驱动返回的 pitch/size，完成 MAP_DUMB 与 mmap。
- [x] 使用 `drmModeAddFB2` 创建未绑定 framebuffer。
- [x] 写入确定性色条并计算完整映射 checksum。
- [x] 正常路径和异常路径均具有逆序资源清理。
- [x] RK3568 连续两次实板测试通过，现有显示管线未被本程序接管。
- [x] 显式确认桌面停止后取得 DRM master。
- [x] 使用完整 mode timing 将 XRGB8888 色条绑定到 DSI CRTC。
- [x] SIGINT/SIGTERM 只设置退出标志，由正常控制流完成 DRM 清理。
- [x] 板端脚本按顺序停止/恢复桌面，并验证独占显示 5 秒通过。

下一小阶段是创建两个 XRGB8888 dumb framebuffer，使用 `drmModePageFlip` 和
flip-complete event 在两张不同色条之间低频切换，先验证 framebuffer 状态和回收
时机。该阶段仍不接入 V4L2、DMA-BUF 或 RGA。
