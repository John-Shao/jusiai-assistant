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
| `autostart` / `--autostart` | 启动即发起通话 | `false` |
| `fullscreen` | 占满面板 | `true` |

TLS：板上 Buildroot rootfs 无系统 CA 库，应用启动时会自动把 `SSL_CERT_FILE`
指向与可执行文件同目录的 `ca-certificates.crt`（OpenSSL 与 LiveKit SDK 内的
rustls 都据此校验）。

## 7. 已验证

**已在 ATK-DLRV1126B 实测通过**（2026-05-21）：交叉编译 → 上板运行 → framebuffer
720×1280 显示界面 → 触摸屏识别 → V4L2 摄像头采集预览（画质正常）→ ALSA 麦克风
采集 + 扬声器播放（音质正常）→ 完整闭环（创建房间 → 连接 LiveKit → 发布音视频 →
唤起 AI 助手 → **与 AI 实时语音互动**）。

移植过程中关于板级音频/摄像头的关键发现见下节 §8。其它已知事项：

- **MPP 硬件编解码**：LiveKit SDK 已集成，本应用走 SDK 默认路径。
- **界面语言**：LVGL 内置 Montserrat 字体仅含拉丁字符，界面文字为英文 + 图标。
  中文需接入 FreeType + CJK 字体。

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

## 9. 目录结构

```
jusiai-assistant/
├── CMakeLists.txt          交叉编译构建（链接预编译 LiveKit SDK）
├── build.sh                一键交叉编译
├── lv_conf.h               LVGL v8.4 配置
├── assets/ca-certificates.crt  随包 CA 根证书
├── lib/                    随仓库的预编译 LiveKit SDK 库（liblivekit*.so）
├── scripts/run.sh          板级启动脚本
├── cmake/                  LVGL / json 的 FetchContent
├── config/                 示例配置
├── tools/                  板级 ALSA 诊断工具（alsadiag / fdtest）
└── src/
    ├── main.cpp
    ├── app_config.*        配置解析
    ├── api/                设备 API HTTP 客户端
    ├── rtc/livekit_session.*  LiveKit 会话封装
    ├── media/              V4L2 摄像头、ALSA 音频、帧缓冲
    ├── core/               闭环状态机
    └── ui/                 LVGL framebuffer/evdev 驱动 + 主界面
```
