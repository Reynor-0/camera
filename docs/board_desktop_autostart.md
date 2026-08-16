# RK3568 禁用与恢复 Weston/systemui 开机启动

## 1. 板端启动机制

当前 Buildroot 使用 BusyBox init，`/etc/inittab` 在启动时执行 `/etc/init.d/rcS`。
`rcS` 按文件名顺序执行 `/etc/init.d/S??*`，其中桌面相关项是：

```text
/etc/init.d/S49weston
/etc/init.d/S50systemui
```

摄像头 ISP 3A 使用独立的 `/etc/init.d/S40rkaiq_3A`，禁用桌面时不得移动或停止它。
ADB、网络、SSH等服务同样不在本操作范围内。

仅删除执行权限并不合适，因为该版本 `rcS` 不检查 `-x`，仍会尝试执行并在启动日志
中产生 `Permission denied`。本方案将两个桌面脚本移动到不会匹配 `S??*` 的目录：

```text
/etc/init.d/desktop-disabled/S49weston
/etc/init.d/desktop-disabled/S50systemui
```

原文件没有删除，恢复脚本会把它们原位移回。

## 2. 板端目录分工

```text
/home/reynor/camera-project/
  bin/       相机项目AArch64可执行文件
  scripts/   相机项目运行和测试脚本
  logs/      相机项目日志

/home/reynor/board-admin/
  desktop/   板级Weston/systemui启动策略管理脚本
```

板级管理脚本会改变 `/etc/init.d` 持久状态，因此不与相机项目产物混放。

## 3. 部署管理脚本

在 WSL 工程目录运行：

```bash
ADB=/home/reynor/tools/platform-tools/adb \
  ./tools/deploy_board_admin_rk3568.sh
```

这个部署动作只复制脚本，不修改启动配置。

## 4. 禁用桌面开机启动

在开发板以 root 运行：

```bash
/home/reynor/board-admin/desktop/disable_desktop_autostart_rk3568.sh
```

脚本会：

1. 校验两个服务不存在重复或缺失；
2. 将 `S49weston`、`S50systemui` 移入 `desktop-disabled`；
3. 停止当前 camera、sysvolume、systemui 和 Weston 桌面进程；
4. 执行 `sync`，确保下次启动仍保持禁用；
5. 验证 Weston/systemui 已退出。

脚本可以重复执行；已禁用时只报告当前状态。

禁用后可核对：

```bash
ls -l /etc/init.d/S49weston /etc/init.d/S50systemui
ls -l /etc/init.d/desktop-disabled
ps | grep -E '[w]eston|[s]ystemui'
```

首次修改后建议人工重启一次，确认系统能够正常启动、ADB仍可连接且桌面不会出现。

## 5. 恢复桌面

恢复开机启动，并立即启动桌面：

```bash
/home/reynor/board-admin/desktop/restore_desktop_autostart_rk3568.sh
```

只恢复下一次开机启动，但当前保持无桌面：

```bash
/home/reynor/board-admin/desktop/restore_desktop_autostart_rk3568.sh --no-start
```

恢复脚本会将两个文件移回 `/etc/init.d`、恢复 `0755` 权限并执行 `sync`。默认模式
还会按 Weston→systemui 顺序启动并等待进程出现。

## 6. 与相机运行脚本的关系

项目运行脚本会检查 `/etc/init.d/S49weston` 和 `S50systemui` 是否存在。桌面开机启动
已经被禁用时，相机测试结束后不会擅自恢复桌面；启动配置恢复后，则维持原来的
“测试前停止、测试后恢复”行为。
