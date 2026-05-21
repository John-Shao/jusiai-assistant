# JuSi AI Assistant（Linux 版 AI 助手）

基于 [LiveKit C++ SDK](../livekit-sdk-cpp)（`dev` 分支）开发的 Linux 桌面 / 嵌入式
「音视频互动 AI 助手」，功能对标 `jusi_meet_android` 中的「AI 助手」一键通话，
复用同一套后端服务 `jusi_meet_suite1.9`（生产环境 `https://meet.jusiai.com`）。

类似豆包的「打电话」功能：点击开始 → 采集麦克风 + 摄像头 → AI 助手加入房间 →
实时语音对话，AI 的语音通过扬声器播放，本地摄像头画面实时预览。

界面使用 **LVGL v8.4**，通过 **SDL3** 完成窗口渲染、摄像头采集、麦克风采集与扬声器播放。

> **状态**：已在 Ubuntu 26.04 LTS（x86_64）上完整构建并验证 —— 程序启动、闭环跑通：
> 创建房间 → 连接 LiveKit → 发布麦克风/摄像头 → 唤起 AI 助手 → AI 助手入会、
> 双向音频接通。

---

## 1. 工作原理

应用复刻 Android 端 `AiAgentViewModel` 的「闭环」流程，全程使用设备 API
（`device-api/v1.0`，凭 `DEVICE_API_KEY` 鉴权）：

```
点击开始 / --autostart
  │
  ├─ 1. POST /device-api/v1.0/rooms/                 创建匿名 1v1 AI 房间
  │       → 返回 livekit { url, token }（参与者身份 device-<device_id>）
  │
  ├─ 2. LiveKit Room.Connect(url, token)             加入房间
  │       → 发布麦克风音轨 + 摄像头视轨
  │
  ├─ 3. POST /device-api/v1.0/rooms/{id}/start-ai-agent/   唤起 AI 助手
  │       → 后端向房间派发 AI agent（identity 以 "ai-agent" 开头）
  │
  ├─ 4. AI agent 加入 → 订阅其音轨 → 解码 PCM → 扬声器播放    实时对话
  │
  └─ 5. 挂断：POST /device-api/v1.0/rooms/{id}/stop-ai-agent/ + 销毁房间连接
```

接口契约见 `jusi_meet_suite1.9/docs/ai_agent_integration.md` 与
`docs/mobile_integration_device.md`。

## 2. 目录结构

```
jusiai-assistant/
├── CMakeLists.txt              顶层构建脚本（add_subdirectory 引入 LiveKit SDK）
├── build.sh                    构建脚本（支持离线依赖暂存）
├── lv_conf.h                   LVGL v8.4 配置
├── cmake/
│   ├── sdl3.cmake / lvgl.cmake / json.cmake   FetchContent 脚本
│   └── LiveKitConfig.cmake.in   SDK 子项目构建所需的 shim（见 §6 注）
├── third_party/httplib.h        内置的 cpp-httplib 0.43.1（HTTPS 客户端）
├── config/jusiai-assistant.conf 示例配置文件
└── src/
    ├── main.cpp                程序入口
    ├── app_config.*            配置（文件 / 环境变量 / 命令行）
    ├── log.h                   轻量日志
    ├── api/                    设备 API HTTP 客户端
    ├── rtc/livekit_session.*   livekit::Room 封装：连接 / 发布 / 订阅
    ├── media/
    │   ├── sdl_media.*         内置的 SDL3 麦克风/摄像头/扬声器封装
    │   ├── audio_io.* camera_io.* frame_buffer.h   音视频引擎
    ├── core/assistant_controller.*  闭环状态机（独立工作线程）
    └── ui/                     LVGL 的 SDL3 显示/输入驱动 + 主界面
```

> `src/media/sdl_media.*` 与 `third_party/httplib.h` 原属 LiveKit SDK 的
> `examples/common/` 与 `third_party/`。SDK 的 `dev` 分支已移除这两处，因此本项目
> 将其内置，不再依赖 SDK 的示例目录。

## 3. 开发环境（Ubuntu 26.04）

> 已验证环境：Ubuntu 26.04 LTS、GCC 15.2、CMake 4.2、Clang/LLVM 21、
> Rust 1.95、Ninja 1.13。开发机 `192.168.126.128`（用户 `johnshao`）。

应用以子项目方式编译 LiveKit C++ SDK，需要 SDK 的全部构建依赖。

```bash
# 1) 系统依赖
sudo apt update
sudo apt install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl wget ca-certificates \
    unzip xz-utils python3 \
    clang libclang-dev llvm-dev \
    libssl-dev protobuf-compiler libprotobuf-dev libabsl-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
    libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
    libgl-dev libegl-dev libgles-dev mesa-common-dev libgl1-mesa-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libudev-dev libdbus-1-dev libdrm-dev libgbm-dev libv4l-dev \
    libfreetype-dev xvfb

# 2) Rust 工具链（LiveKit SDK 的 FFI 层用 Rust 构建）
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
source "$HOME/.cargo/env"
```

`build.sh` 会自动探测 `libclang.so`（设置 `LIBCLANG_PATH`）并加上
`-Wno-deprecated-declarations`（新版 GCC 编译 WebRTC 旧代码需要）。

SDL3、LVGL v8.4、nlohmann-json 由 CMake `FetchContent` 自动拉取并构建。

## 4. 获取代码

本项目与两个 LiveKit SDK 仓库按同级目录摆放，**均使用 `dev` 分支**：

```
workspace/camera/
├── livekit-sdk-cpp/      # LiveKit C++ SDK（dev 分支）
│   └── client-sdk-rust/  # 子模块，固定在 livekit-sdk-rust dev 分支同一 commit
├── livekit-sdk-rust/     # LiveKit Rust SDK（dev 分支）
└── jusiai-assistant/     # 本项目
```

`livekit-sdk-cpp` 的 `client-sdk-rust` 子模块需初始化（含其嵌套子模块
`livekit-protocol/protocol`、`yuv-sys/libyuv`）：

```bash
cd livekit-sdk-cpp
git checkout dev
git submodule update --init --recursive
```

## 5. 构建

### 方式 A —— 网络可直连 GitHub

```bash
cd jusiai-assistant
chmod +x build.sh
./build.sh release
# 如 SDK 不在 ../livekit-sdk-cpp：./build.sh release --sdk /path/to/livekit-sdk-cpp
```

首次构建会编译整个 LiveKit SDK（含 Rust FFI），并通过 CMake FetchContent /
cargo 拉取 spdlog、abseil、protobuf、SDL3、LVGL、nlohmann-json 及预编译
libwebrtc，耗时较长（约 20–40 分钟）；后续增量构建很快。

### 方式 B —— GitHub 访问不稳定（推荐用于国内开发机）

实测国内开发机直连 GitHub 下载大文件经常 `Connection reset`。可在一台网络良好的
机器上预先下载下列依赖，放进一个目录后用 `--deps` 传入：

| 子目录 | 下载地址 |
|--------|----------|
| `spdlog-1.15.1/` | `https://github.com/gabime/spdlog/archive/refs/tags/v1.15.1.tar.gz` |
| `SDL-release-3.2.26/` | `https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.26.tar.gz` |
| `lvgl-8.4.0/` | `https://github.com/lvgl/lvgl/archive/refs/tags/v8.4.0.tar.gz` |
| `json/` | `https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz` |
| `linux-x64-release/` | `https://github.com/livekit/rust-sdks/releases/download/webrtc-7af9351/webrtc-linux-x64-release.zip`（解压后即此目录，约 690 MB） |

```bash
# 解压到 deps/ 目录（放在项目内的 deps/，或与项目同级的 ../deps/）
mkdir -p deps && cd deps
tar -xzf spdlog-1.15.1.tar.gz
tar -xzf SDL-release-3.2.26.tar.gz
tar -xzf lvgl-8.4.0.tar.gz
tar -xJf json-3.11.3.tar.xz
unzip -q webrtc-linux-x64-release.zip      # 得到 linux-x64-release/
cd ..

./build.sh release            # 自动发现 ./deps 或 ../deps
# 依赖目录在其它位置时显式指定：./build.sh release --deps /path/to/deps
```

`build.sh` 会自动发现 `./deps` 或 `../deps`，把各依赖目录映射为 CMake 的
`FETCHCONTENT_SOURCE_DIR_*`，并把 `linux-x64-release/` 导出为 `LK_CUSTOM_WEBRTC`
环境变量，使 `webrtc-sys` 跳过下载。

> **注意**：`LK_CUSTOM_WEBRTC` 是环境变量。若不经 `build.sh` 而直接调用
> `cmake --build`，需自行 `export LK_CUSTOM_WEBRTC=<deps>/linux-x64-release`，
> 否则 `webrtc-sys` 仍会尝试联网下载预编译 libwebrtc。

> abseil、protobuf 由 SDK 的 `cmake/protobuf.cmake` 经 FetchContent 拉取；
> 如该步骤也因网络失败，可另设
> `-DFETCHCONTENT_SOURCE_DIR_LIVEKIT_ABSEIL=` 与
> `-DFETCHCONTENT_SOURCE_DIR_LIVEKIT_PROTOBUF=`。
> cargo 依赖从 crates.io 拉取；如需离线可配置 cargo 镜像源。

构建产物：`build-release/bin/jusiai-assistant`。`liblivekit.so`、
`liblivekit_ffi.so`、`libSDL3.so` 会自动拷贝到可执行文件同目录
（可执行文件 RPATH 为 `$ORIGIN`）。

## 6. 运行

```bash
cd build-release/bin
./jusiai-assistant
```

窗口左侧显示摄像头预览，右侧为状态与控制按钮：

- **Start** — 执行闭环：创建房间 → 连接 → 唤起 AI 助手；
- **Hang Up** — 停止 AI 助手并断开房间；
- **Microphone** — 通话中静音 / 取消静音本地麦克风。

无摄像头时预览区显示动态占位画面，应用其余功能照常工作。

无显示器的服务器上可用虚拟显示做冒烟测试：

```bash
Xvfb :99 -screen 0 1024x600x24 &
DISPLAY=:99 ./jusiai-assistant --autostart --log-level debug
```

## 7. 配置

配置优先级（后者覆盖前者）：内置默认值 → 配置文件 → `JUSIAI_*` 环境变量 → 命令行参数。

配置文件查找顺序：`--config <路径>` → `./jusiai-assistant.conf` →
`~/.config/jusiai-assistant/jusiai-assistant.conf`。示例见
`config/jusiai-assistant.conf`（构建后亦拷贝为 `bin/jusiai-assistant.conf.sample`）。

| 配置键 / 命令行 | 说明 | 默认值 |
|----------------|------|--------|
| `base_url` / `--base-url` | 后端基础地址 | `https://meet.jusiai.com` |
| `device_api_key` / `--device-api-key` | 设备预共享密钥 | `jusi-device-2025` |
| `device_id` / `--device-id` | 设备标识 | 自动生成并持久化 |
| `provider` / `--provider` | AI 提供商：`doubao` / `doubao_s2s` / `qwen` | `doubao` |
| `voice` / `--voice` | 输出音色（留空用默认） | 空 |
| `prompt_label` / `--prompt-label` | AI 助手人设标签 | `通用 AI 助手` |
| `publish_video` / `--no-video` | 是否发布本地摄像头视轨 | `true` |
| `fullscreen` / `--fullscreen` | 全屏（嵌入式定屏设备） | `false` |
| `autostart` / `--autostart` | 启动后立即开始通话（无需点 Start） | `false` |

`device_id` 留空时自动生成形如 `JUSI-LINUX-xxxxxxxxxxxx` 的标识，持久化在
`~/.config/jusiai-assistant/device_id`，对应房间内参与者身份 `device-<device_id>`。

可用的 `provider` / `voice` / `prompt_label` 取值：

```bash
curl -s https://meet.jusiai.com/api/v1.0/rooms/ai-agent-config/ | python3 -m json.tool
```

## 8. 与 Android「AI 助手」的对应关系

| Android (`jusi_meet_android`) | 本项目 |
|------------------------------|--------|
| `AiAgentRepository` / `DeviceApi` | `src/api/device_api_client.*` |
| `AiAgentViewModel`（闭环状态机） | `src/core/assistant_controller.*` |
| `LiveKitController` | `src/rtc/livekit_session.*` |
| `AiAgentScreen`（一键进入） | `src/ui/ui_app.*`（LVGL 界面） |
| LiveKit Android SDK | LiveKit C++ SDK（dev 分支） |

两端使用完全相同的 `device-api/v1.0` 接口与 `DEVICE_API_KEY`。

## 9. 说明与已知限制

- **`dev` 分支差异**：`livekit-sdk-cpp` 的 `dev` 分支已移除 `examples/` 与
  `third_party/`，并且 `Room` 没有显式的 `Disconnect()`（销毁 `Room` 即离开房间）。
  本项目已据此适配。
- **`LiveKitConfig.cmake.in` shim**：SDK 的 `CMakeLists.txt` 用 `${CMAKE_SOURCE_DIR}`
  解析该打包模板，作为子项目构建时会指向本项目根目录，故在 `cmake/` 下放了一份
  同名占位文件。生成的 `LiveKitConfig.cmake` 不被使用。
- **界面语言**：LVGL 内置 Montserrat 字体仅含拉丁字符，故界面文字为英文 + 图标符号。
  如需中文界面，可在 `lv_conf.h` 中开启 `LV_USE_FREETYPE` 并加载系统中文字体
  （如 `fonts-noto-cjk`）。
- **摄像头**：通过 SDL3 的相机后端（Linux 上为 V4L2 / PipeWire）访问；无摄像头时
  自动使用合成画面。
- **音频**：麦克风以 10ms 实时帧推入 LiveKit；扬声器在收到 AI 第一帧音频时按其
  采样率惰性打开。
- **AI 等待超时**：唤起 AI 助手后最多等待 45 秒，超时报错并可重试。
- **房间有效期**：设备 API 创建的匿名房间默认 2 小时后过期。

## 10. 故障排查

| 现象 | 排查 |
|------|------|
| `LiveKitConfig.cmake.in does not exist` | 确认 `cmake/LiveKitConfig.cmake.in` 存在 |
| FetchContent / webrtc 下载 `Connection reset` | 用 §5 方式 B 预先暂存依赖 |
| Rust bindgen 找不到 libclang | 安装 `libclang-dev`；或手动 `export LIBCLANG_PATH=` |
| 编译 WebRTC 旧代码报 deprecated 错误 | build.sh 已自动加 `-Wno-deprecated-declarations` |
| `-Werror=format-security` 报错 | 已修复（Ubuntu 默认开启该硬化选项） |
| 运行时找不到 `liblivekit_ffi.so` | 确认其与可执行文件同目录（构建已自动拷贝） |
| `Cannot reach the server` | 检查到 `meet.jusiai.com` 的网络 / DNS / 证书 |
| 创建房间返回 401 | `device_api_key` 不正确 |
| 无声音 / 无画面 | 确认麦克风、摄像头权限与默认设备 |

## 11. 许可

LiveKit C++ SDK、cpp-httplib、LVGL、SDL3 等依赖各自的开源许可；本项目源码随仓库约定。
