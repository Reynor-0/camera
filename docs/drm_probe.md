# DRM/KMS 只读资源探测

## 1. 阶段边界

`drm_probe` 只查询 DRM driver、dumb-buffer capability、connector、preferred
mode、encoder 和 CRTC。它不会申请 DRM master、创建 framebuffer 或执行 modeset，
因此可以在 Weston 运行时使用，不会改变屏幕内容。

## 2. 模块

```text
inc/drm_device.hpp      归一化探测结果和 DrmDevice 接口
src/drm_device.cpp      libdrm 查询、自动选择和 RAII 清理
src/drm_test_main.cpp   --help/--version、参数解析和结果输出
```

connector、encoder 和 CRTC object ID 由程序自动选择，不能把某次启动得到的数值
写死到代码中。

## 3. 构建

普通开发机缺少 libdrm 开发文件时，CMake 保留 `camera_demo` 并跳过
`drm_probe`。RK3568 交叉构建把探测程序设为必需目标，避免误以为已经生成：

```bash
./tools/cross_build_rk3568.sh
```

输出位于：

```text
build-rk3568/stage/bin/drm_probe
```

## 4. 板端运行

```bash
/home/reynor/drm_probe /dev/dri/card0
```

当前 RK3568 实板验收结果：

```text
Driver: rockchip
Dumb buffer: supported
Connector: DSI-1
Mode: 1080x1920
Refresh: 54.44 Hz
Preferred: yes
```

程序运行前后 Weston 和 systemui 均保持运行，DSI connector 保持 connected，当前
CRTC framebuffer ID 未改变。

## 5. 完成状态

- [x] ISO C++11。
- [x] 自动选择 connected connector。
- [x] 优先选择 preferred mode。
- [x] 自动选择当前或 compatible CRTC。
- [x] libdrm 查询对象和 DRM fd 使用 RAII 清理。
- [x] RK3568 严格告警交叉构建通过。
- [x] Weston 运行时实板探测通过。

下一小阶段是创建一个独立的 XRGB8888 DRM dumb buffer；在完成 framebuffer 创建和
清理验证前，仍不停止 Weston、不执行 modeset。
