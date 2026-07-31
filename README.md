# QQ Xwayland Portal preload

这是已在 QQ `3.2.31-51102` 和 `3.2.32-51802` 上核对调用链的独立
`LD_PRELOAD` 兼容层。它只处理
`resources/app/avsdk/broadcast-core.so` 中已经确认的录屏路径，以及
`resources/app/wrapper.node` 中已经确认的截图路径。编译后只需
向 QQ 注入一个 `libqq-xwayland-screencast-preload.so`。

## 工作方式

1. 启动器让 QQ 使用 Xwayland 采集后端。本库不修改会话环境变量。
2. `broadcast-core.so` 调用 `XShmAttach` 时，在独立线程中执行
   `CreateSession -> SelectSources -> Start -> OpenPipeWireRemote`。
3. `XShmGetImage` 不调用真实 Xlib 函数，而是把最新 PipeWire 帧写入
   QQ 提供的 `XImage`。因此录屏帧不会再进入 Xwayland 当前的
   Screenshot portal 路径。
4. `wrapper.node` 请求完整根窗口的 `XGetImage` 时，改为调用
   `org.freedesktop.portal.Screenshot.Screenshot`。请求使用
   `interactive=false`，由 GdkPixbuf 解码 portal 返回的 URI，再写入
   QQ 需要的完整 Xwayland 根窗口图像；QQ 自身继续负责后续裁剪。
5. 录屏 portal 被拒绝、尚未产生帧或流发生错误时，仍向 QQ
   返回成功，但目标 `XImage` 为黑屏。截图 portal 被拒绝、调用
   失败或 URI 无法解码时，同样返回有效的黑色 `XImage`，不会回退到
   真实 X11 截图。
6. QQ 的录制线程调用 `XCloseDisplay` 时停止 PipeWire、调用
   `org.freedesktop.portal.Session.Close` 并回收线程。

所有 Xlib hook 都按调用动态库和录制专用 `Display *` 过滤。截图
hook 还会校验默认根窗口、起点、完整几何尺寸、`AllPlanes` 和
`ZPixmap`。QQ 中其他 X11 图像读取以及其他 X11 客户端会继续进入
真实 Xlib。

## 构建

下面命令同时安装编译依赖和离线测试使用的 Xvfb。

Arch Linux：

```bash
sudo pacman -S --needed base-devel meson ninja pkgconf dbus libpipewire \
    libx11 libxext gdk-pixbuf2 xorg-server-xvfb
```

Fedora：

```bash
sudo dnf install gcc meson ninja-build pkgconf-pkg-config dbus-devel \
    dbus-daemon \
    pipewire-devel libX11-devel libXext-devel gdk-pixbuf2-devel \
    xorg-x11-server-Xvfb
```

Debian：

```bash
sudo apt install build-essential meson ninja-build pkg-config \
    libdbus-1-dev dbus-daemon libpipewire-0.3-dev libx11-dev libxext-dev \
    libgdk-pixbuf-2.0-dev xvfb
```

构建并运行离线测试：

```bash
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

测试使用 Xvfb、一个最小的假 `broadcast-core.so` 和一个假
`wrapper.node`，验证：

- 测试在启动器提供的 X11 会话契约下运行。
- `XShmAttach` 能标记录制专用 X11 连接。
- portal 不可用或被禁用时，`XShmGetImage` 和 `XGetImage` 返回黑帧。
- `XCloseDisplay` 能结束对应会话。
- 非 QQ 的 `XGetImage` 调用在会话前后均不受影响。
- 只有 `wrapper.node` 对完整根窗口的截图请求会转发 portal。
- 截图 portal 被禁用时返回黑图，有效图像 URI 能正确解码为
  `XImage`，无效 URI 仍返回黑图。
- 私有测试 D-Bus 上的假 portal 返回成功 URI 和拒绝码 `2` 时，
  截图路径分别得到正确图像和全黑图像。
- BGRx、BGRA、RGBx、RGBA、裁剪、缩放和旋转所使用的帧转换路径。

测试只会启动 `xvfb-run -a` 分配的隔离 X server，不会连接当前
`DISPLAY`。

## 启动 QQ

宿主 X11 会话不需要本兼容层。宿主 Wayland 会话中，在重写
`XDG_SESSION_TYPE` 之前完成会话类型判断，然后启动 QQ：

```bash
preload="$PWD/build/libqq-xwayland-screencast-preload.so"

if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
    unset WAYLAND_DISPLAY
    export XDG_SESSION_TYPE=x11
    export LD_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}"
fi

exec /path/to/qq
```

不要改写环境中已有的 `DISPLAY`；QQ 必须连接该 Xwayland display。
不要额外传入启用 Native Wayland 的 Electron 参数。如果在容器中
运行，还需要容器能访问宿主会话 D-Bus，并让宿主
xdg-desktop-portal 能检索到 `qq.xwayland.desktop`。

## 环境变量

- `QQ_PRELOAD_DEBUG=1`：向标准错误输出 portal 请求、会话阶段和
  PipeWire 状态。
- `QQ_PRELOAD_DISABLE_PERSISTENCE=1`：不发送 `persist_mode`，也不读写
  restore token。
- `QQ_PRELOAD_DISABLE_PORTAL=1`：不创建 portal 会话，所有录制帧和 QQ
  截图请求直接返回黑屏，主要用于测试和故障隔离。

持久 restore token 默认保存在：

```text
$XDG_STATE_HOME/qq-xwayland-screencast/restore-token
```

如果没有设置 `XDG_STATE_HOME`，则使用：

```text
$HOME/.local/state/qq-xwayland-screencast/restore-token
```

为了让 xdg-desktop-portal 将请求显示为 QQ，首次运行时会在
`$XDG_DATA_HOME/applications` 或
`$HOME/.local/share/applications` 创建隐藏的
`qq.xwayland.desktop`，并通过
`org.freedesktop.host.portal.Registry` 注册 `qq.xwayland`。

## 范围

这是 QQ 专用兼容层，不替代 Xwayland 项目中通用的持续录屏实现。
它也不会修改 QQ 文件、Xwayland 或 xdg-desktop-portal。

## 广告

该项目由 GPT-5.6 Sol(Max) 完成全部工作, 我只是喂了相关项目源码进去, 欢迎大佬们贡献各类PR
