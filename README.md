# 秋秋快传 · QiuLanShare

在同一个 Wi-Fi 下，让**电脑（Windows）和手机（Android）互传文件、图片与文字消息**。
所有数据只在局域网内点对点直连，**不经过任何服务器**，无需注册、无需登录。

> 本仓库包含 **Android 客户端** 与 **Windows 端 TrafficMonitor 插件** 的最新源码与发行版。
> 鸿蒙（HarmonyOS）版「秋秋快传」**已在应用商店上架**，其源码不在本仓库内。

---

## 目录

- [一、支持平台](#一支持平台)
- [二、功能特性](#二功能特性)
- [三、下载与发行版](#三下载与发行版)
- [四、Windows 插件安装](#四windows-插件安装)
- [五、Android 安装说明](#五android-安装说明)
- [六、从源码编译](#六从源码编译)
- [七、目录结构](#七目录结构)
- [八、隐私与安全](#八隐私与安全)
- [English Version](#qiulanshare-english)

---

## 一、支持平台

| 平台 | 形态 | 源码位置 | 状态 |
|---|---|---|---|
| Windows | **TrafficMonitor 插件**（`lanshare.dll`） | [`windows/`](windows/) | ✅ 本仓库 |
| Android | 独立 App（Kotlin + Jetpack Compose） | [`android/`](android/) | ✅ 本仓库 |
| HarmonyOS 鸿蒙 | 元服务「秋秋快传」 | —— | ✅ **已在应用商店上架**（源码不在本仓库） |

> **Windows 插件是 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的第三方插件。**
> TrafficMonitor 是 zhongyang219 开发的桌面悬浮窗软件（显示网速 / CPU / 内存）。
> 本插件通过其官方插件接口 `ITMPlugin` 寄生在 `TrafficMonitor.exe` 中运行，宿主仓库：
> **<https://github.com/zhongyang219/TrafficMonitor>**。

---

## 二、功能特性

- **自动发现**：同一 Wi-Fi 下自动出现在设备列表；可分别开关「主动发现 / 被发现」。
- **文字消息**：双向即时聊天，历史落盘保存，可按天数自动清理或手动清空。
- **文件 / 图片传输**：拖拽或选择发送，**分片续传**、实时进度与速度、断点续传。
- **系统通知**：后台收到消息 / 传输进度在通知栏实时显示，点击直达会话。
- **看图**：内置图片查看器，支持缩放与左右滑动翻页。
- **系统分享**：Android 端可作为系统「分享」目标接收文件，也可把 App 内图片/文件分享出去。
- **主题**：亮 / 暗 / 跟随系统三态。
- **对等网络**：Windows、Android、鸿蒙三端都是对等节点，任意两端可直连互通。

---

## 三、下载与发行版

前往本仓库的 **[Releases](https://github.com/nsq4926/QiuLanShare/releases)** 页面下载。

### Android

按 CPU 架构拆分的安装包（推荐选与自己手机匹配的版本以减小体积）：

| 文件 | 适用架构 |
|---|---|
| `QiuLanShare-android-arm64-v8a-1.0.0.apk` | 绝大多数现代手机（64 位 ARM） |
| `QiuLanShare-android-armeabi-v7a-1.0.0.apk` | 老款 32 位 ARM 设备 |
| `QiuLanShare-android-x86-1.0.0.apk` | x86 模拟器 / 部分平板 |
| `QiuLanShare-android-x86_64-1.0.0.apk` | x86_64 模拟器 / 部分设备 |
| `QiuLanShare-android-universal-1.0.0.apk` | **通用包**：不确定架构时用这一个即可，四架构通吃 |

> 版本：`versionCode 10000 / versionName 1.0.0`，使用正式发布密钥签名（可直接覆盖升级）。
> 应用仅支持局域网直连，首次启动需先阅读并同意隐私政策。

### Windows

| 文件 | 说明 |
|---|---|
| `QiuLanShare-windows-plugin-x64-1.0.0.zip` | 64 位 TrafficMonitor 插件（`TrafficMonitor.exe` 显示为 64 位时用），解压得 `lanshare.dll` |
| `QiuLanShare-windows-plugin-x86-1.0.0.zip` | 32 位 TrafficMonitor 插件（`TrafficMonitor.exe` 显示为 32 位时用），解压得 `lanshare.dll` |
| `QiuLanShare-windows-app-x64-1.0.0.zip` | 独立托盘程序（免安装 TrafficMonitor，可选），解压运行 `LanShare.exe` |

> 插件位数**必须与你的 `TrafficMonitor.exe` 位数一致**，否则任务栏里看不到插件。
> 宿主程序请从 <https://github.com/zhongyang219/TrafficMonitor> 获取。

---

## 四、Windows 插件安装

1. 确认 `TrafficMonitor.exe` 是 32 位还是 64 位（任务管理器 → 详细信息 → 平台列），下载对应位数的 `lanshare.dll`。
2. 在 TrafficMonitor 安装目录下新建 `plugins` 文件夹（若不存在）。
3. 把 `lanshare.dll` 复制进 `plugins\`：

   ```
   TrafficMonitor\
   ├── TrafficMonitor.exe
   └── plugins\
       └── lanshare.dll
   ```

4. **完全退出 TrafficMonitor 再重新打开**（托盘图标右键 → 退出）。
5. 右键任务栏网速窗口 → **显示项目** → 勾选 **局域网地址 / 未读数**。
6. 浏览器打开 `http://127.0.0.1:8765` 即为内置传输界面（手机浏览器同地址也能用）。
7. 首次启动 Windows 防火墙会弹窗，**必须点「允许访问」（勾选专用网络）**，否则手机连不进来。

> 插件标识为 **秋秋快传 (QiuLanShare)**；配置与运行数据保存在 `plugins\` 目录下。

---

## 五、Android 安装说明

- 直接安装上表对应架构的 APK（或通用包）。若系统提示「未知来源」，允许本次安装即可。
- 首次启动会展示**隐私同意页**：未同意前不会启动后台服务、不会申请通知权限；拒绝则退出。
- 同意后 App 会在局域网内监听（默认 8765），与同一 Wi-Fi 下的电脑/手机自动互相发现。
- 应用需要「网络 / Wi-Fi 状态 / 前台服务 / 通知」等权限，用途见 [隐私政策说明](docs/15-隐私政策与权限说明.md)。

---

## 六、从源码编译

### Android

环境：JDK 21、Android SDK（compileSdk 36）、Gradle 8.13（无需 Gradle Wrapper，直接用本机 gradle）。

```bash
cd android
# 指向你自己的 SDK
echo "sdk.dir=/path/to/AndroidSDK" > local.properties

export JAVA_HOME=/path/to/jdk-21
gradle assembleRelease
# 产物：app/build/outputs/apk/release/app-*-release.apk（四个 ABI + universal）
```

> Release 签名通过 `android/keystore.properties` + `android/keystore/*.jks` 读取（二者**不入库**）。
> 没有签名文件时会退回未签名包，仅用于本地验证。ABI 拆分配置见
> [`android/app/build.gradle.kts`](android/app/build.gradle.kts) 的 `splits { abi { … } }`。

### Windows 插件

环境：Visual Studio 2022/2026，安装「使用 C++ 的桌面开发」工作负载。

```bat
cd windows
build.bat x64 Release    :: → build\x64\Release\lanshare.dll
build.bat x86 Release    :: → build\Win32\Release\lanshare.dll
build-app.bat Release    :: 可选：生成独立托盘 exe（build\x64\Release\app\）
```

> 运行库为 **/MT 静态链接**，用户机器无需安装 vcruntime。
> 构建细节、部署与排障见 [`docs/02-Windows端构建.md`](docs/02-Windows端构建.md)。

---

## 七、目录结构

```
QiuLanShare/
├── README.md                 ← 本文件（中 / 英）
├── android/                  ← Android 客户端（Kotlin + Compose）
│   ├── app/build.gradle.kts      ← 含 ABI 拆分 / 签名 / R8
│   └── app/src/main/java/com/sanqiu/lanshare/
├── windows/                  ← Windows TrafficMonitor 插件（C++）
│   ├── TrafficMonitorLanShare.sln
│   ├── build.bat / build-app.bat
│   ├── src/                      ← HttpServer / TransferHub / Plugin / Discovery …
│   └── web/index.html            ← 内置网页界面（PC / 手机浏览器通用）
└── docs/                     ← 架构、构建、上架、隐私、功能清单等文档
```

---

## 八、隐私与安全

- **纯局域网**：所有数据在设备间直连传输，不经过任何服务器，不上传云端，不收集个人信息。
- **无账号**：不注册、不登录、无追踪 SDK。
- 传输使用明文 HTTP（局域网内无证书），请仅在可信网络中使用。
- Android 端实现了「同意先于联网」的隐私门禁；详见 [隐私政策与权限说明](docs/15-隐私政策与权限说明.md)。

---

## 致谢

- Windows 插件宿主程序：[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)（by zhongyang219）。

---
---

# QiuLanShare (English)

Transfer **files, images and text messages between your Windows PC and Android phone** over the same Wi-Fi.
Everything is peer-to-peer on the local network — **no server, no account, no cloud**.

> This repository contains the latest source code and releases of the **Android client** and the
> **Windows TrafficMonitor plugin**. The **HarmonyOS version ("秋秋快传") is already published on the app store**
> and its source is not part of this repository.

## Platforms

| Platform | Form | Source | Status |
|---|---|---|---|
| Windows | **TrafficMonitor plugin** (`lanshare.dll`) | [`windows/`](windows/) | ✅ this repo |
| Android | Standalone app (Kotlin + Jetpack Compose) | [`android/`](android/) | ✅ this repo |
| HarmonyOS | Meta-service "秋秋快传" | — | ✅ **published on the app store** (source not here) |

> **The Windows plugin is a third-party plugin for [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)**,
> a desktop floating-window tool for showing network speed / CPU / memory, by zhongyang219.
> The plugin implements TrafficMonitor's official `ITMPlugin` interface and runs inside `TrafficMonitor.exe`.
> Host repository: **<https://github.com/zhongyang219/TrafficMonitor>**.

## Features

- **Auto discovery** on the same Wi-Fi, with independent "discover / be discovered" switches.
- **Text chat** with persisted history and auto-cleanup by days.
- **File & image transfer** with chunked resumable upload, live progress and speed.
- **System notifications** for incoming messages and transfer progress.
- **Image viewer** with zoom and swipe paging.
- **System share** target on Android (receive shared files / share files out).
- **Light / Dark / Follow-system** themes.
- **Peer-to-peer**: Windows, Android and HarmonyOS are symmetric peers; any two can interconnect.

## Downloads / Releases

Go to **[Releases](https://github.com/nsq4926/QiuLanShare/releases)**.

**Android** — per-ABI APKs (pick the one matching your device for smaller size):

| File | ABI |
|---|---|
| `QiuLanShare-android-arm64-v8a-1.0.0.apk` | 64-bit ARM (most modern phones) |
| `QiuLanShare-android-armeabi-v7a-1.0.0.apk` | 32-bit ARM (older devices) |
| `QiuLanShare-android-x86-1.0.0.apk` | x86 emulator / some tablets |
| `QiuLanShare-android-x86_64-1.0.0.apk` | x86_64 emulator / some devices |
| `QiuLanShare-android-universal-1.0.0.apk` | **Universal** — works on all four ABIs |

> Version `versionCode 10000 / versionName 1.0.0`, signed with the release key (over-the-top updates OK).

**Windows** — download the plugin zip matching your `TrafficMonitor.exe` bitness and extract `lanshare.dll`
into the `plugins\` folder. The plugin bitness **must match** the host. Get the host program from
<https://github.com/zhongyang219/TrafficMonitor>.

| File | Notes |
|---|---|
| `QiuLanShare-windows-plugin-x64-1.0.0.zip` | 64-bit plugin → `lanshare.dll` |
| `QiuLanShare-windows-plugin-x86-1.0.0.zip` | 32-bit plugin → `lanshare.dll` |
| `QiuLanShare-windows-app-x64-1.0.0.zip` | Optional standalone tray app → run `LanShare.exe` |

## Windows plugin install

1. Check whether `TrafficMonitor.exe` is 32-bit or 64-bit and download the matching `lanshare.dll`.
2. Create a `plugins` folder inside the TrafficMonitor install directory.
3. Copy `lanshare.dll` into `plugins\`.
4. Fully exit and restart TrafficMonitor.
5. Right-click the taskbar window → **Display items** → enable **LAN address / unread count**.
6. Open `http://127.0.0.1:8765` — the built-in transfer UI (also reachable from a phone browser).
7. Allow access in the Windows firewall prompt (private network) so phones can connect.

## Build from source

**Android** (JDK 21, Android SDK compileSdk 36, Gradle 8.13):

```bash
cd android
echo "sdk.dir=/path/to/AndroidSDK" > local.properties
export JAVA_HOME=/path/to/jdk-21
gradle assembleRelease
# outputs: app/build/outputs/apk/release/app-*-release.apk (4 ABIs + universal)
```

**Windows plugin** (Visual Studio 2022/2026 with "Desktop development with C++"):

```bat
cd windows
build.bat x64 Release    :: -> build\x64\Release\lanshare.dll
build.bat x86 Release    :: -> build\Win32\Release\lanshare.dll
build-app.bat Release    :: optional standalone tray exe
```

## Privacy & security

- **LAN only**: data goes directly between devices; no server, no cloud, no personal data collected.
- **No account**, no tracking SDK.
- Plain HTTP on the local network (no certificate) — use only on trusted networks.
- Android enforces "consent before networking"; see [privacy & permissions](docs/15-隐私政策与权限说明.md).

## Credits

- Host program for the Windows plugin: [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) by zhongyang219.
