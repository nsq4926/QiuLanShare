# 02 · Windows 端构建（插件 DLL + 托盘 exe）

> 目标：把 `lanshare.dll` 塞进 `TrafficMonitor\plugins\` 就能跑；exe 版是可选的独立托盘客户端。
> 协议与目录约定见 `01`；脚本细节见 `12`；当前状态见 `08`。

## 一、环境

| 项 | 要求 |
|---|---|
| 编译器 | Visual Studio 2022（v143/v145 工具集均可），需 **C++ 桌面开发** 工作负载 |
| 目标位数 | 必须与 `TrafficMonitor.exe` **同位数**（64 位 TM → x64 DLL） |
| 工程 | `windows\TrafficMonitorLanShare.sln`（DLL + exe 两个 vcxproj 共用 `src\`） |
| 运行库 | **/MT 静态链接**（别改 /MD，否则用户机器缺 `vcruntime140.dll` 会加载失败） |

确认 TM 位数：任务管理器 → TrafficMonitor.exe → 是否带 `(32 位)`。

## 二、编译

```bat
cd windows
build.bat x64 Release        :: → build\x64\Release\lanshare.dll
build.bat x86 Release        :: → build\Win32\Release\lanshare.dll
build-app.bat Release        :: → build\x64\Release\app\（exe 便携目录）
```

`build.bat` 自己用 `vswhere.exe` 找 VS、载入编译环境、清空代理变量（`HTTPS_PROXY` 与 `https_proxy`
同时存在会让 MSBuild 报 `MSB6001`）。成功时最后一行是 `[OK] ...\lanshare.dll`。

也可以直接开 `.sln`，选 `Release|x64` 后 `Ctrl+Shift+B`。

## 三、部署

```bat
:: TrafficMonitor 必须先完全退出（托盘右键 → 退出），否则 DLL 被占用
tools\deploy-windows.bat "C:\Tools box\System Tools\TrafficMonitor"
```

脚本会等 TM 进程退出再复制，然后重启 TM。手动方式等价于把 DLL 复制到 `plugins\`。

首次加载后在 TM 里勾选显示项：任务栏窗口右键 → **显示项目** → **局域网地址 / 未读数**。
插件标识为 **秋秋快传 (QiuLanShare)**。

## 四、运行时文件

| 位置 | 内容 | 归属 |
|---|---|---|
| `plugins\lanshare.ini` | `[server] port= / dir=`，`[firewall]` 的"不再提示" | 程序 |
| `plugins\LanShare\` | `lanshare_devices[.<inst>].json`、`lanshare_convs[.<inst>].json`、`lanshare_history.jsonl`、`lanshare.<inst>.lock`、`lanshare.log` | **程序，不可被清理带走** |
| `saveDir\<设备名>-<deviceId>\` | 收到的图片与文件；默认系统"下载" | 用户 |

- 数据目录与保存目录必须分开；换保存目录必须带迁移（验收：`deviceId` 逐字符相同）。
- `TMLS_INSTANCE` 可起多实例，每个实例独立 ini 段 / 数据文件 / 锁；锁失败就**不监听**，不静默降级。

## 五、自检

```bash
curl.exe -s --noproxy "*" http://127.0.0.1:8765/api/ping        # {"ok":true}
curl.exe -s --noproxy "*" http://127.0.0.1:8765/api/hello       # deviceId / port / resumable
curl.exe -s --noproxy "*" http://127.0.0.1:8765/api/devices     # 能看到手机才算通
```

三条肉眼项：左栏 10 秒不跳、切设备不闪、删在线设备 1 分钟内不自动重现。

## 六、排障（按概率排序）

| 症状 | 原因 / 处置 |
|---|---|
| 插件列表里没有 LanShare | DLL 位数不对 / 未重启 TM / `plugins` 目录不存在 |
| 端口反复递进 | 上一实例没退干净（看 `lanshare.log`）；确认 TCP 用 `SO_EXCLUSIVEADDRUSE` |
| 手机能发来、PC 收不到 | Windows 防火墙拦入站 → 网页底部提示条点"一键放行"（需管理员） |
| 网页改了没生效 | 服务端**优先读磁盘** `plugins\web\index.html`；浏览器缓存加 `?v=随机` |
| 改了 `index.html` 但 DLL 里是旧页面 | `web.rc` 未 touch（`build.bat` 已无条件 touch，勿改） |
| 任务栏卡 / explorer 无响应 | 插件在 UI 线程阻塞：`DllMain` 里别做事、`DataRequired` 别等锁、`ShellExecuteW` 丢工作线程 |
| 中文路径保存失败 | `ini` 必须 UTF-16 读写；临时文件与目标同盘改名才是原子的 |
| `dir` 显示 127.0.0.1 | 没有可用局域网网卡（VPN/虚拟网卡被过滤是正常的） |
| `curl` 访问 127.0.0.1 报 `upstream connect failed` | 本机有 `HTTP_PROXY`，加 `--noproxy "*"` |

## 七、源码地图（`windows/`）

| 文件 | 职责 | 改动风险 |
|---|---|---|
| `src/TransferHub.cpp/.h` | 消息库、历史落盘、文件归档与去重、传输进度、会话版本 | 高（协议与数据格式） |
| `src/HttpServer.cpp` | HTTP/1.1 子集、22 个 `/api/*`、分片续传 `StreamUpload` | 高 |
| `src/Discovery.cpp` | UDP 8766 广播与解析 | 中（字段必须与移动端一致） |
| `src/DeviceRegistry.cpp/.h` | 设备表、五态、MAC 合并、落盘 | 高 |
| `src/PeerClient.cpp` | 出站投递（文字/文件/探活） | 中 |
| `src/Plugin.cpp` | ITMPlugin 接口、任务栏显示、托盘菜单、气泡 | 中（UI 线程约束） |
| `src/Firewall.cpp` | 防火墙检测与加规则 | 低 |
| `web/index.html` | 网页 UI（单文件） | 低（改完刷新即可） |
| `app/` | exe 壳（Win32 + WebView2，复用 `src/` 核心） | 中 |

**两个不能碰的约束**：插件 UI 线程不许阻塞（见上表）；JSON 拼串一律"键名+冒号"分段 +
`JsonEscape`，并跑 `tools\check-json-quotes.py`。
