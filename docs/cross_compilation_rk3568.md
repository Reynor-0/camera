# RK3568 交叉编译与部署指南

## 1. 目标环境假设

RK3568 使用四核 Arm Cortex-A55 CPU。本项目默认面向运行 64 位 Linux 用户态的
RK3568，即板端 `uname -m` 输出 `aarch64`。编译主机可以是 x86_64 Linux，构建
产物不能直接在编译主机运行。

先在开发板执行：

```bash
uname -m
getconf LONG_BIT
ldd --version
```

预期前两项分别为：

```text
aarch64
64
```

如果输出是 `armv7l` 和 `32`，说明目标 rootfs 是 32 位用户态。此时不能使用本文
的 AArch64 toolchain，即使 RK3568 CPU 本身支持 64 位；需要另建 armhf toolchain
并匹配板端 hard-float ABI。

## 2. 为什么必须使用板端匹配的 sysroot

交叉编译涉及三个相互独立的环境：

```text
x86_64 build host
    │ 运行 cmake、make/ninja、交叉编译器
    ▼
AArch64 compiler + target sysroot
    │ 生成 ARM64 ELF，链接目标版 libc/libdrm
    ▼
RK3568 target board
    运行 camera_demo，访问 /dev/video* 和 /dev/dri/card*
```

sysroot 是目标板根文件系统的编译视图，至少应包含目标架构的头文件和库。它决定：

- 使用哪个版本和 ABI 的 libc/libstdc++。
- 后续链接哪个版本的 libdrm。
- `pkg-config` 和 CMake 应该查找哪些目标库。

不能从宿主机 `/usr/include` 和 `/usr/lib/x86_64-linux-gnu` 查找 libdrm，否则可能
在编译期链接失败，或者更隐蔽地生成与板端不兼容的程序。

最佳来源是板卡厂商提供的、与当前固件相同版本的 Rockchip BSP/SDK sysroot。
也可以从目标 rootfs 制作 sysroot，但必须保留符号链接，并保证开发头文件完整。

## 3. 工具链参数

项目提供 [rk3568-aarch64.cmake](../cmake/toolchains/rk3568-aarch64.cmake)。它接受：

| 参数 | 用途 | 默认值 |
| --- | --- | --- |
| `RK3568_CROSS_COMPILE` | 交叉工具完整前缀，末尾包含 `-` | `aarch64-linux-gnu-` |
| `RK3568_SYSROOT` | 与开发板固件匹配的 sysroot | 空，使用编译器内建值 |
| `RK3568_CPU_FLAGS` | 可选 CPU 优化参数 | 空 |

工具链前缀可以只是 PATH 中的名称：

```text
aarch64-linux-gnu-
```

也可以包含 SDK 的绝对路径：

```text
/opt/rockchip-sdk/toolchain/bin/aarch64-buildroot-linux-gnu-
```

不要假定所有 Rockchip SDK 都使用同一个前缀。应进入 SDK 的 `bin` 目录查看实际的
`*-gcc`、`*-g++`、`*-readelf` 名称。

## 4. 推荐构建方法

使用项目脚本：

```bash
export RK3568_CROSS_COMPILE=/opt/rockchip-sdk/toolchain/bin/aarch64-linux-gnu-
export RK3568_SYSROOT=/opt/rockchip-sdk/sysroot

./tools/cross_build_rk3568.sh
```

如果使用 Ubuntu/Debian 的通用交叉工具链，并且工具已在 PATH：

```bash
export RK3568_CROSS_COMPILE=aarch64-linux-gnu-
./tools/cross_build_rk3568.sh
```

当前 V4L2 probe 只使用 Linux UAPI、libc 和 libstdc++，通用工具链通常可以完成
构建。不过进入 DRM/KMS 阶段后必须使用包含目标板 libdrm 的 sysroot。

脚本执行以下步骤：

1. 检查交叉 `g++` 是否存在。
2. 使用 RK3568 toolchain 配置独立的 `build-rk3568` 目录。
3. 以 Release 模式构建。
4. 安装到 `build-rk3568/stage/bin`。
5. 使用交叉 `readelf` 检查 ELF machine 必须是 AArch64。

不要复用本机 `build/` 目录。CMake 会缓存编译器路径，同一个 build directory 不能
在宿主机编译器和交叉编译器之间切换。

## 5. 手动使用 CMake

```bash
cmake -S . -B build-rk3568 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3568-aarch64.cmake \
  -DRK3568_CROSS_COMPILE=/opt/rockchip-sdk/toolchain/bin/aarch64-linux-gnu- \
  -DRK3568_SYSROOT=/opt/rockchip-sdk/sysroot \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-rk3568 --parallel
cmake --install build-rk3568 --prefix build-rk3568/stage
```

检查产物：

```bash
/opt/rockchip-sdk/toolchain/bin/aarch64-linux-gnu-readelf \
  -h build-rk3568/stage/bin/camera_demo

/opt/rockchip-sdk/toolchain/bin/aarch64-linux-gnu-readelf \
  -d build-rk3568/stage/bin/camera_demo
```

ELF header 的 Machine 必须是 `AArch64`。动态依赖中不应出现开发主机专有路径。

## 6. 部署到开发板

示例使用 `scp`，实际 IP 和目录按目标板环境调整：

```bash
scp build-rk3568/stage/bin/camera_demo root@board-ip:/usr/local/bin/
```

如果不希望修改板端系统目录，可部署到临时目录：

```bash
scp build-rk3568/stage/bin/camera_demo root@board-ip:/tmp/
ssh root@board-ip /tmp/camera_demo /dev/video0
```

运行前检查权限和设备：

```bash
ls -l /dev/video* /dev/media* /dev/dri/*
id
```

直接 KMS 阶段还需要确认没有 Weston/X11 占用 DRM master。不要为了绕过权限问题
长期使用宽松的 `chmod 777`；应通过 udev rule、`video`/`render` 用户组或受控的
服务账户配置权限。

## 7. 动态库兼容性检查

在板端执行：

```bash
file /usr/local/bin/camera_demo
ldd /usr/local/bin/camera_demo
```

常见错误：

### `No such file or directory`，但文件确实存在

通常是 ELF interpreter 在板端不存在。用交叉 `readelf -l` 查看 `Requesting program
interpreter`，并与板端 `/lib` 下的动态加载器比较。这常见于工具链 libc 与 rootfs
不一致，例如用 glibc 工具链构建后部署到 musl rootfs。

### `GLIBC_x.y not found` 或 `GLIBCXX_x.y.z not found`

表示程序链接到了比板端更新的 libc 或 libstdc++。应换用板卡 SDK 工具链和对应
sysroot重新编译，不建议简单把开发主机的动态库复制到板端覆盖系统库。

### 找不到 `libdrm.so`

进入 DRM 阶段后，应同时确认 sysroot 内存在 libdrm 的头文件、目标架构库和
pkg-config 文件，并在板端安装 ABI 兼容的运行库。

## 8. RK3568 构建约束

- 架构固定为 AArch64，但默认不强制 `-mcpu=cortex-a55`，以兼容较旧的 BSP
  编译器。需要时可设置 `RK3568_CPU_FLAGS=-mcpu=cortex-a55`。
- C++ 标准仍固定为 ISO C++11，交叉编译不会放宽此限制。
- 不允许在配置阶段运行目标程序；CMake 的 `try_compile` 只构建静态库。
- 构建工具从宿主机查找，头文件、库和 package 从目标 sysroot 查找。
- 每次固件/SDK 升级后重新创建交叉构建目录，避免 CMake 缓存旧 ABI 路径。

## 9. 当前环境验证状态

当前代码工作区主机为 x86_64，未发现 `aarch64-linux-gnu-g++` 或 Rockchip SDK
交叉编译器。因此当前只能验证：

- 本机 ISO C++11 编译成功。
- toolchain、构建脚本和 CMake 参数组织完成。
- 缺少交叉编译器时脚本能够立即报告明确错误。

取得板卡 SDK 或安装 AArch64 工具链后，必须执行一次真实交叉构建，并在 RK3568
板端验证 ELF、动态依赖和 V4L2 ioctl。

## 10. 参考资料

- [Rockchip RK3568 Datasheet](https://opensource.rock-chips.com/images/b/b6/Rockchip_RK3568_Datasheet_V1.3-20220929P.PDF)
- [CMake Toolchains Manual](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
- [CMake `CMAKE_SYSROOT`](https://cmake.org/cmake/help/latest/variable/CMAKE_SYSROOT.html)
