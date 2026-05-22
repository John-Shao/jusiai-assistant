# JuSi AI Assistant — Rockchip RV1126B 版

`port/rv1126b` 分支：将「音视频互动 AI 助手」移植到瑞芯微 **RV1126B**（正点原子
ATK-DLRV1126B）开发板。功能对标 `jusi_meet_android` 的「AI 助手」一键通话，复用
同一套后端 `jusi_meet_suite1.9`（生产环境 `https://meet.jusiai.com`）。

桌面（x86 / SDL3）版本保留在 `main` / `dev` 分支；本分支是板级原生移植。

---

## 1. 与桌面版的区别

RV1126B 无 3D GPU，平台层从 SDL3 全部换成板原生接口：

| 能力 | 桌面版 | RV1126B 版 |
|------|--------|-----------|
| 显示 | SDL3 窗口 | LVGL → Linux framebuffer `/dev/fb0`（720×1280 竖屏）|
| 输入 | SDL3 鼠标/触摸 | evdev 触摸屏 `/dev/input/event*` |
| 摄像头 | SDL3 camera | V4L2 多平面采集 `/dev/video-camera0`（NV12）|
| 音频 | SDL3 audio | ALSA 采集 + 播放（ES8389 codec）|
| LiveKit SDK | 源码 `add_subdirectory` | 预编译 aarch64 `.so` 链接 |

控制器、状态机、设备 API 客户端、LiveKit 会话封装等与平台无关的代码两端共用。

平台层源码：`src/ui/lv_display.*`（framebuffer + evdev）、`src/media/camera_io.*`
（V4L2）、`src/media/audio_io.*`（ALSA）。

## 2. 闭环流程

与 Android 端「AI 助手」一致，全程走设备 API（`DEVICE_API_KEY` 鉴权）：

```
创建匿名房间  → 连接 LiveKit → 发布麦克风 + 摄像头
            → 唤起 AI 助手 → 订阅 AI 音频播放 → 挂断（停止 AI + 断开）
```

## 3. 构建环境

需要两台机器：

- **交叉编译机**（x86_64 Linux，本项目用 `192.168.126.129`）：装有 ATK-DLRV1126B
  SDK 的 Buildroot 工具链 + sysroot。
- **开发板**（ATK-DLRV1126B，本项目用 `192.168.10.241`）。

### 3.1 依赖前提（交叉编译机）

1. **ATK RV1126B SDK** —— 提供 `aarch64-buildroot-linux-gnu-` 工具链与 sysroot。
2. **LiveKit C++ SDK 头文件** —— 分支 `port/rv1126b-*` 的源码树，编译时取其
   `include/` 头文件（`LIVEKIT_SDK_DIR` 指向它）。预编译的 aarch64 共享库
   `liblivekit.so` / `liblivekit_ffi.so` 已**随仓库放在 `lib/` 目录**，以预编译
   库方式链接、不重新编译；更新 SDK 版本时替换 `lib/` 下这两个文件即可。
3. **暂存依赖**（避免构建时联网）：
   - LVGL v8.4 源码、nlohmann-json —— 放进项目 `deps/` 或 `~/lk-deps/`。
   - `build.sh` 会按内容自动识别并设置 `FETCHCONTENT_SOURCE_DIR_*`。

## 4. 构建

```bash
# 在交叉编译机上
cd jusiai-assistant
./build.sh --sdk /path/to/livekit-sdk-cpp
# 产物：build-rv1126b/bin/  （jusiai-assistant + liblivekit*.so + ca-certificates.crt + run.sh）
```

`build.sh` 会：source SDK 的 `scripts/env-rv1126b.sh`（工具链/sysroot/webrtc/MPP
环境）→ 用 SDK 的 `cmake/toolchains/rv1126b-aarch64-linux-gnu.cmake` 交叉编译 →
自动发现暂存的 LVGL / json。

## 5. 部署与运行

把 `build-rv1126b/bin/` 整个目录拷到板子（例如 `/opt/jusiai/`）：

```bash
scp -r build-rv1126b/bin/* root@<board>:/opt/jusiai/
```

在板子上运行：

```bash
cd /opt/jusiai && ./run.sh                 # 等待触屏点击「开始」
cd /opt/jusiai && ./run.sh --autostart     # 启动即自动发起 AI 通话（kiosk）
```

`run.sh` 会先按正确顺序停掉板载出厂摄像头固件栈（`watchdog_d` / `camera_ui_d` /
`camera_core_d`，详见 §8.1），再启动 `rkaiq_3A_server` 接管 ISP 3A，最后启动应用。
可执行文件 RPATH 为 `$ORIGIN`，`liblivekit*.so` 与 `ca-certificates.crt` 须与
可执行文件同目录（部署目录已包含）。

退出：`Ctrl-C` 或 `kill -INT`。

### 5.1 无屏设备（headless）运行与控制 API

无屏设备加 `--headless`：跳过 LVGL/framebuffer/触摸 UI（无 `/dev/fb0` 时本来也起
不来），只跑 AI 闭环。此时设备通过**本地 HTTP 控制 API** 操控 —— 由同设备上的语音
模块、手机命令模块调用；那两个模块各自负责语音识别和手机协议，jusiai-assistant
只对外提供自身功能的控制接口。

```bash
cd /opt/jusiai && ./run.sh --headless              # 等待控制 API 指令
cd /opt/jusiai && ./run.sh --headless --autostart  # 启动即发起 AI 通话
```

`--headless` 时若未指定 `--control-port`，自动用默认端口 **8765**（无屏设备必须可
控）。控制 API 默认只绑 `127.0.0.1`（调用方是本机的兄弟模块）。带屏设备也可设
`control_port` 开启同一套 API，触摸与 API 并存。

| 方法 / 路径 | 作用 | 参数 |
|------------|------|------|
| `POST /start`   | 发起 AI 通话 | — |
| `POST /stop`    | 结束 AI 通话 | — |
| `POST /mic`     | 麦克风静音开关 | `muted`（true/false）|
| `POST /camera`  | 摄像头开关 | `enabled`（true/false）|
| `GET /status`   | 当前状态（JSON）| — |
| `GET /events`   | 状态变化推送（SSE, Server-Sent Events）| — |
| `GET /healthz`  | 存活探测 | — |

`POST /mic`、`POST /camera` 的参数可走 URL query（`?muted=true`）或 JSON body
（`{"muted":true}`）。`/status` 与 `/events` 的状态字段：

| 字段 | 说明 |
|------|------|
| `state` | 机器可读状态：`idle` / `creating_room` / `connecting` / `waiting_agent` / `in_call` / `stopping` / `error` |
| `status` / `detail` | 人类可读文案（随 `language` 本地化，供显示用）|
| `mic_muted` / `camera_muted` | 麦克风 / 摄像头当前是否关闭 |
| `agent_speaking` | AI 当前是否在说话 |
| `camera_available` | 摄像头是否可用 |

`GET /events` 是 Server-Sent Events 长连接：连上即先收到一次当前状态，之后每次状态
变化推送一条 `event: status` 帧（data 为上面的 JSON），另每 10 s 一次保活帧。

### 5.2 手机直连操控

手机直接连 API 操控。接口与状态字段同 §5.1（无状态 HTTP，每条命令一个请求），差别只在
**绑定地址**这一点。

**让 API 可被直连。** 控制 API 默认只绑 `127.0.0.1`，手机连不上 —— 直连场景必须让
它监听局域网地址：

| 方式 | 设置 |
|------|------|
| 命令行 | `./run.sh --headless --control-bind 0.0.0.0` |
| 配置文件 | `control_bind = 0.0.0.0`、`control_port = 8765` |
| 环境变量 | `JUSIAI_CONTROL_BIND=0.0.0.0` |

启动日志会打印 `control: HTTP control API listening on 0.0.0.0:8765`。手机
的访问地址即 `http://<设备局域网IP>:8765`；设备 IP 需自行获知（静态 IP / 路由器
DHCP 绑定 / 手动输入，程序未内置 mDNS 发现）。

**完整 curl 示例**（各接口、状态字段的含义见 §5.1）：

```bash
DEV=http://192.168.10.241:8765      # 改成设备实际 IP

curl -s $DEV/healthz                                  # 探活：设备/控制 API 就绪
curl -s $DEV/status                                   # 查当前状态
curl -s -X POST $DEV/start                            # 发起 AI 通话
curl -sN $DEV/events                                  # 订阅状态变化（SSE 长连接）
curl -s -X POST "$DEV/mic?muted=true"                 # 麦克风静音（query 传参）
curl -s -X POST $DEV/mic -H 'Content-Type: application/json' \
     -d '{"muted":false}'                             # 取消静音（JSON body 传参）
curl -s -X POST "$DEV/camera?enabled=false"           # 关摄像头
curl -s -X POST $DEV/stop                             # 挂断
```

成功返回 `{"ok":true}`；`/mic`、`/camera` 缺参数返回 `HTTP 400` +
`{"ok":false,"error":"..."}`。一次完整通话（可直接当冒烟测试）：

```bash
DEV=http://192.168.10.241:8765
curl -s $DEV/healthz                                          # 设备在线？
curl -s -X POST $DEV/start                                    # 发起
until curl -s $DEV/status | grep -q '"state":"in_call"'; do sleep 1; done
curl -s -X POST "$DEV/mic?muted=true"                         # 通话中静音
curl -s -X POST "$DEV/mic?muted=false"                        # 取消静音
curl -s -X POST $DEV/stop                                     # 挂断
```

**典型接入流程：**

1. 进入界面：`GET /healthz` 判断设备可达，并发起一条 `GET /events` 长连接，UI 全靠
   它刷新（连上即收到当前状态）。
2. 用户按「呼叫」：`POST /start`，界面随 SSE 从 `connecting` 过渡到 `in_call`。
3. 通话中：静音键 → `POST /mic`，摄像头键 → `POST /camera`；按钮选中态由 SSE 推来的
   `mic_muted` / `camera_muted` 回填，`agent_speaking` 驱动「AI 说话中」指示。
4. 用户按「挂断」：`POST /stop`，SSE 回到 `idle`。
5. 遇到 `error` 状态：用 `detail` 提示用户，「重试」即再发一次 `POST /start`。

若手机不便维持 SSE 长连接，可改用每 1~2 s 轮询 `GET /status`。

> **安全提示**：控制 API **当前无鉴权** —— 绑到 `0.0.0.0` 后，同一局域网内任何人都
> 能操控设备。仅适用于可信家庭网络；产品化建议加预共享令牌校验
> （`Authorization: Bearer …`，与设备 API 的 `device_api_key` 同思路）。

### 5.3 手机端接收 SSE 状态推送

`GET /events` 是一条**不关闭的 HTTP GET**：手机发起请求后，服务端持续在该连接上推送
文本帧，格式为：

```
event: status
data: {"state":"in_call","mic_muted":false,...}

```

手机端读取该流、按空行切分、取 `data:` 行解析 JSON 即可。

**三个必须注意的点：**

1. **事件名是 `status`，不是 `message`** —— 服务端发的是具名事件（`event: status`）。
   Web 端必须 `addEventListener('status', …)`，用 `onmessage` 收不到；原生 SSE 库的
   `onEvent` 回调会带上 type，不受影响。
2. **明文 HTTP 要放行** —— API 是 `http://`（非 https）：Android 9+ 在
   `AndroidManifest.xml` 的 `<application>` 加 `android:usesCleartextTraffic="true"`
   （或用 network-security-config 只放行设备网段）；iOS 在 `Info.plist` 的
   `NSAppTransportSecurity` 加 `NSAllowsLocalNetworking = true`。
3. **要自己重连** —— 设备重启 / WiFi 抖动会断流。服务端每 10 s 推一帧保活，可借此
   判断连接是否还活着。

**Android（Kotlin + OkHttp SSE）：**

```kotlin
// build.gradle: implementation("com.squareup.okhttp3:okhttp-sse:4.12.0")
val client = OkHttpClient.Builder()
    .readTimeout(0, TimeUnit.MILLISECONDS)   // 0=不超时，SSE 是长连接
    .build()
val request = Request.Builder().url("http://192.168.10.241:8765/events").build()

val listener = object : EventSourceListener() {
    override fun onEvent(es: EventSource, id: String?, type: String?, data: String) {
        val s = JSONObject(data)             // type=="status"，data 是状态 JSON
        runOnUiThread { updateUi(s.getString("state"), s.getBoolean("mic_muted")) }
    }
    override fun onFailure(es: EventSource, t: Throwable?, resp: Response?) {
        // 断线：延迟 2~5 s 后重新 newEventSource() 重连
    }
}
EventSources.createFactory(client).newEventSource(request, listener)
```

`readTimeout(0)` 是关键，否则 OkHttp 会掐掉「空闲」的长连接。

**iOS（Swift）：** 最省事用成熟库 swift-eventsource（自带重连）：

```swift
var cfg = EventSource.Config(handler: myHandler,
            url: URL(string: "http://192.168.10.241:8765/events")!)
EventSource(config: cfg).start()
// MessageHandler.onMessage(eventType: "status", messageEvent:) → messageEvent.data 是 JSON
```

不想加依赖，可用 `URLSession` + `URLSessionDataDelegate`：必须用 delegate 版的
`dataTask`（completion-handler 版会把整个响应缓冲到结束才回调），在
`urlSession(_:dataTask:didReceive:)` 里把到达的分片拼进缓冲、按 `\n\n` 切块、取
`data:` 行解析。

**Web / WebView（手机 UI 为网页时）：** 浏览器原生支持且自动重连：

```js
const es = new EventSource('http://192.168.10.241:8765/events');
es.addEventListener('status', e => {        // 注意：不是 onmessage
  const s = JSON.parse(e.data);
  updateUi(s.state, s.mic_muted, s.agent_speaking);
});
```

**生命周期：** SSE 连接随界面打开建立、整个会话保持；App 切后台时关闭、回前台再重连。

## 6. 配置

优先级（后者覆盖前者）：内置默认值 → 配置文件 → `JUSIAI_*` 环境变量 → 命令行参数。
配置文件查找：`--config <路径>` → `./jusiai-assistant.conf` →
`~/.config/jusiai-assistant/jusiai-assistant.conf`。示例见
`config/jusiai-assistant.conf`。

常用项：

| 键 / 命令行 | 说明 | 默认值 |
|------------|------|--------|
| `base_url` / `--base-url` | 后端基础地址 | `https://meet.jusiai.com` |
| `device_api_key` / `--device-api-key` | 设备预共享密钥 | `jusi-device-2025` |
| `tls_verify` | 校验后端 TLS 证书（板上无系统 CA 库，默认关）| `false` |
| `provider` / `--provider` | `doubao` / `doubao_s2s` / `qwen` | `doubao` |
| `camera_rotation` | 摄像头顺时针旋转角（传感器物理装配补偿）| `90` |
| `camera_device` | V4L2 摄像头节点 | `/dev/video-camera0` |
| `audio_mic_gain` | 麦克风软件增益（编解码器 PGA 已足够，默认不额外加）| `1.0` |
| `audio_aec` / `--no-aec` | 回声消除（扬声器与麦克风共用全双工编解码器，默认开，见 §8.5）| `true` |
| `audio_aec_delay_ms` / `--aec-delay` | 回声消除的扬声器→麦克风延迟估计（ms）| `90` |
| `autostart` / `--autostart` | 启动即发起通话 | `false` |
| `fullscreen` | 占满面板 | `true` |
| `language` / `--language` | 界面语言：`zh`（简体中文）/ `en` | `zh` |
| `headless` / `--headless` | 无屏模式：跳过显示 UI，仅靠控制 API 操控（见 §5.1）| `false` |
| `control_port` / `--control-port` | 本地控制 API 端口（`0` 关闭；headless 时默认 `8765`）| `0` |
| `control_bind` / `--control-bind` | 控制 API 绑定地址 | `127.0.0.1` |

TLS：板上 Buildroot rootfs 无系统 CA 库，应用启动时会自动把 `SSL_CERT_FILE`
指向与可执行文件同目录的 `ca-certificates.crt`（OpenSSL 与 LiveKit SDK 内的
rustls 都据此校验）。

## 7. 已验证

**已在 ATK-DLRV1126B 实测通过**（2026-05-21）：交叉编译 → 上板运行 → framebuffer
720×1280 显示界面 → 触摸屏识别 → V4L2 摄像头采集预览（画质正常）→ ALSA 麦克风
采集 + 扬声器播放（音质正常）→ 完整闭环（创建房间 → 连接 LiveKit → 发布音视频 →
唤起 AI 助手 → **与 AI 实时语音互动**）。

移植过程中关于板级音频/摄像头的关键发现见 §8；界面多语言见 §9。其它已知事项：

- **MPP 硬件编解码**：LiveKit SDK 已集成，本应用走 SDK 默认路径。

## 8. 板级关键发现（音频 / 摄像头踩坑记录）

移植到 ATK-DLRV1126B 时，板级硬件/固件有几处非显而易见的约束，曾导致「画面暗灰」
「声音爆破音」「AI 听不到说话」三个问题。根因与解法记录如下，相关逻辑已落到
`src/media/audio_io.cpp`、`src/media/alsa_setup.c`、`scripts/run.sh` 的代码注释里。

### 8.1 出厂摄像头固件栈与 watchdog

板子出厂 rootfs 自带一套摄像头应用，由 init.d 启动：

- `camera_core_d`（`S95camera-core-d`）—— 核心守护进程，**独占** `/dev/video*`
  摄像头节点**和 ALSA 采集设备** `/dev/snd/pcmC0D0c`，并在进程内跑 ISP 3A。
- `camera_ui_d`（`S96camera-ui-d`）—— 出厂屏幕 UI。
- `watchdog_d`（`S97watchdog`）—— 看门狗，约每 5 s 检测，发现 core/ui 挂掉就重启。

要点：

- **直接 `kill camera_core_d` 没用** —— `watchdog_d` 几秒内把它拉起来，重新抢占
  麦克风，应用便永远采不到音、AI 听不到。必须按 init.d 顺序停整套栈，**watchdog
  最先停**（`S97watchdog` 脚本注释自己也强调这个顺序），否则停 core/ui 时看门狗
  正好把它们又拉起来：
  ```
  /etc/init.d/S97watchdog      stop
  /etc/init.d/S96camera-ui-d   stop
  /etc/init.d/S95camera-core-d stop
  ```
- **必须用 SIGTERM 优雅停**（init.d `stop` 即是）—— `camera_core_d` 的信号处理会
  干净交还 ISP/VENC。用 `kill -9` 强杀会把 ISP 留在坏状态，导致**摄像头画面暗灰**。
- 停掉 `camera_core_d` 也停掉了它进程内的 ISP 3A 调校；`run.sh` 随后启动独立的
  `rkaiq_3A_server` 接管自动曝光/白平衡，画面恢复正常。

### 8.2 ES8389 codec 的 ALSA 全双工约束

板子只有一块声卡 `hw:0,0`，采集（`pcmC0D0c`）与播放（`pcmC0D0p`）共用同一个
**全双工 ES8389 codec**（经 Rockchip SAI 接口）。两条硬约束：

- **采集只能立体声 + 显式 hw_params**。ES8389/SAI 采集路径只接受 2 声道；用单声道
  打开、或用 `snd_pcm_set_params` 由延迟推导周期几何，`snd_pcm_readi` 几乎必返回
  `-EIO`。必须显式 `snd_pcm_hw_params` 指定 **S16_LE / 48 kHz / 立体声 / period
  1024 / 4 periods**（power-of-2 周期），再软件下混成单声道喂给 SDK。
- **全双工：播放 PCM 必须用与采集流完全相同的 hw_params 打开**。播放在
  `hw:0,0` 上打开时若参数与正在跑的采集流不一致（典型如用 `plughw` 单声道），
  会重新配置共享的 SAI，**把正在跑的采集 DMA 直接卡死**。所以播放也固定
  S16_LE / 48 kHz / 立体声 / period 1024，AI 下行音频在软件里重采样 + 上混到该
  格式。`snd_pcm_link()` 解决不了此问题（实测反而更糟）。

此外：

- ES8389 录音增益须用 `snd_ctl_*` API 按控件名设置（板上无 `amixer`），其中
  `ALC Capture Switch` 必须 **OFF** —— 该驱动 ALC 打开会把采集电平压成 0。详见
  `src/media/alsa_setup.c`。
- 不当的 ALSA 采集配置可能直接把整块板子卡死/重启，调试时需留意。

### 8.3 SDK 直采帧约束

LiveKit `AudioSource` 在 `queue_size_ms = 0`（实时直采模式）下，`captureFrame`
**严格要求 10 ms 帧**（48 kHz 下 = 480 样本/帧）。`snd_pcm_readi` 可能返回不足量，
须累积补满到正好 480 样本再提交，否则 SDK 抛异常丢帧。

### 8.4 板级 ALSA 诊断工具

`tools/` 下有两个独立单文件诊断程序，调板级音频时很有用：

| 文件 | 用途 |
|------|------|
| `tools/alsadiag.c` | 枚举 codec 全部控件（板上无 `amixer` 的替代）、采集设备 hw_params 范围，并用显式 hw_params 探测采集流（验证立体声可用 / 单声道 `-EIO`）|
| `tools/fdtest.c`   | 复现全双工冲突：对比匹配/不匹配参数打开播放对采集流的影响 |

交叉编译（单文件，仅依赖 libasound）：

```bash
aarch64-buildroot-linux-gnu-gcc -O2 tools/alsadiag.c -lasound -lm -o alsadiag
```

### 8.5 回声消除

扬声器和麦克风是同一块全双工 ES8389（见 §8.2），麦克风必然录到正在外放的 AI
语音 —— 不消回声的话 AI 会听到自己、自问自答。

本移植版把 WebRTC 的音频设备模块换成了自定义 ALSA 收发，于是 WebRTC 自带的自动
AEC 被绕过了；应用改为**显式**调用 LiveKit SDK 的 `AudioProcessingModule`
（WebRTC APM，含 AEC3）。接入点在 `src/media/audio_io.cpp`：

- 播放写线程在 `snd_pcm_writei` 前，把**即将外放的那一帧 10 ms 音频**下混成单声道
  喂给 `processReverseStream()` 作回声参考；
- 采集线程把麦克风 10 ms 帧喂给 `processStream()` 原地消回声，再上送给 SDK。

两条路径本来就以 10 ms 为粒度（SDK 直采也要求严格 10 ms 帧），与 APM 要求一致。
开关为配置项 `audio_aec`（默认开），`audio_aec_delay_ms` 是扬声器→麦克风回环延迟
的初始估计（默认 90 ms，AEC3 会自适应，板上若有残留回声可调）。同时启用了降噪与
高通滤波；AGC 关闭（ES8389 的 PGA 已设定电平，开 AGC 会与之相互打架）。

## 9. 界面语言（中文 / 英文）

界面默认**简体中文**，也支持英文，由配置项 `language`（`zh` / `en`）切换。

- 所有界面文案集中在 `src/i18n.cpp` 的 `kZh` / `kEn` 表，`tr(Msg::...)` 按当前
  语言返回；语言在启动时由 `main.cpp` 依据配置选定。
- 中文字形：`assets/fonts/lv_font_zh_{14,16,20}.c` 是用 `tools/gen_zh_font.js`
  从 CJK 字体生成的**子集位图字体**，只含 ASCII + 界面用到的几十个汉字（三个
  字号合计约 270 KB），直接编进程序，无运行时字体引擎。按钮图标仍用 LVGL 内置
  的 Montserrat 符号字体。
- 改动 `src/i18n.cpp` 里的中文后需重新生成字体：

  ```bash
  npm install -g lv_font_conv
  node tools/gen_zh_font.js
  ```

  脚本会扫描 `i18n.cpp` 里的全部非 ASCII 字符并按需重新子集化。默认用随 Windows
  的 SimHei 生成；产品化建议换开源 CJK 字体（如 Noto Sans SC）：
  `FONT_TTF=/path/to/font.ttf node tools/gen_zh_font.js`。

## 10. 目录结构

```
jusiai-assistant/
├── CMakeLists.txt          交叉编译构建（链接预编译 LiveKit SDK）
├── build.sh                一键交叉编译
├── lv_conf.h               LVGL v8.4 配置
├── assets/ca-certificates.crt  随包 CA 根证书
├── assets/fonts/           子集中文位图字体（lv_font_zh_*.c，生成物）
├── lib/                    随仓库的预编译 LiveKit SDK 库（liblivekit*.so）
├── scripts/run.sh          板级启动脚本
├── cmake/                  LVGL / json 的 FetchContent
├── config/                 示例配置
├── tools/                  ALSA 诊断工具 + 中文字体生成脚本
└── src/
    ├── main.cpp
    ├── app_config.*        配置解析
    ├── i18n.*              界面多语言文案表（中文 / 英文）
    ├── api/                设备 API HTTP 客户端
    ├── rtc/livekit_session.*  LiveKit 会话封装
    ├── media/              V4L2 摄像头、ALSA 音频、帧缓冲
    ├── core/               闭环状态机
    ├── control/            无屏设备的本地 HTTP 控制 + 状态 API
    └── ui/                 LVGL framebuffer/evdev 驱动 + 主界面
```
