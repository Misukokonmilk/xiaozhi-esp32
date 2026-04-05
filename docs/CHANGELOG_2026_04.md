# EchoEar 固件开发日志 — 2026 年 4 月

> 记录客户端-服务器交互优化、唤醒词修复、以及环境配置踩坑的完整过程。

---

## 一、客户端-服务器交互优化

### 背景

对比 StarfireServer (`E:\Code\PYTHON\StarfireServer`) 服务端代码与 EchoEar 固件客户端代码，发现 13 个交互问题。经用户确认后，修复其中 8 个（排除 #4、#5、#6、#10）。

### 修复内容

| # | 模块 | 修改 | 文件 |
|---|------|------|------|
| T4 | HTTP Refresh 重试 | 新增 `HTTP_REFRESH_MAX_RETRIES=3` 和 `HTTP_REFRESH_RETRY_DELAY_MS=2000`，实现刷新 token 失败时的重试循环 | `protocols/config.h`, `protocols/http_refresh.cc` |
| T5 | 协议版本 | `WEBSOCKET_PROTOCOL_VERSION` 从 `1` 提升到 `3`，与服务器最新版本对齐 | `protocols/config.h` |
| T6 | 超时时间 | `kTimeoutSeconds` 从 `120` 提升到 `180`，与服务端 `IDLE_TIMEOUT_SECONDS=180` 一致 | `protocols/protocol.cc` |
| T7 | MCP 特性标记 | `features.mcp` 从 `true` 改为 `false`，因为当前未使用 MCP 协议 | `protocols/websocket_protocol.cc` |
| T8 | WebSocket 重连退避 | 实现指数退避策略：初始 1 秒，每次失败翻倍，上限 30 秒；连接成功时重置为 1 秒 | `application.cc`, `application.h` |

### 服务器端修复（未部署）

`StarfireServer/server/websocket/websocket_handler.py` 修复了 3 个 bug：

1. `device_type` 未定义的 `NameError` — 导致声音无法播放
2. `features` 未传递给 `send_hello_response()` — 导致客户端收不到正确的功能协商
3. 缺少 `device_id` 校验日志 — 影响调试效率

> ⚠️ 服务器修复仍在本地，尚未部署到远端 `47.109.195.63`。这是**声音无法播放**的直接原因。

---

## 二、唤醒词失效排查

### 现象

编译烧录固件后，表情正常、WiFi 正常、服务器连接正常，但唤醒词「你好零一」完全无效。

### 排查过程

#### 第一轮：sdkconfig 检查

发现 `sdkconfig` 中 `CONFIG_SR_MN_CN_MULTINET7_QUANT=y` 被重置为 `CONFIG_SR_MN_CN_NONE=y`，即固件根本没有编译 MultiNet7 引擎。

修复方法：参照 `sdkconfig.old`（之前能工作的配置），将 `MULTINET7_QUANT` 恢复为 `y`。

**但修复后仍然不工作。**

#### 第二轮：构建环境不一致（真正的根因）

发现实际使用的 ESP-IDF 环境与文档记录不一致：

| 项目 | 文档要求 (PROJECT_REFERENCE.md) | 实际使用的 | 影响 |
|------|-------------------------------|-----------|------|
| ESP-IDF 路径 | `E:\Code\ESP32\SDK\v5.4.2\esp-idf` (**v5.4.2**) | `E:\Code\ESP32\SDK\esp-adf\esp-idf` (**v5.4**) | MultiNet7 编译产物不同 |
| Python 环境 | `idf5.4_py3.11_env` | `idf5.5_py3.11_env` | 工具链版本不同 |
| 激活方式 | `activate.py --export` 脚本 | 手动设 `$env:PATH` | 可能缺少依赖工具 |

**v5.4 和 v5.4.2 虽然版本号接近，但对 esp-sr 组件（MultiNet7 引擎）的编译行为存在差异**。用 v5.4 编译出的固件，即使 sdkconfig 配置正确，唤醒词引擎也无法正常工作。

### 修复方法

严格按照文档 4.4 节的一键构建脚本：

```powershell
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
$activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1
& $activateScript
Set-Location "E:\Code\ESP32\SF\xiaozhi-esp32"
idf.py clean
idf.py build
```

**关键步骤**：

1. 必须先删除整个 `build/` 目录（不能只 `idf.py clean`），因为 CMake cache 中会残留旧 IDF 路径
2. 编译后用 `idf.py -p COM3 flash` 刷写固件
3. **必须**再单独刷写自定义 assets.bin（`idf.py flash` 会覆盖 assets 分区为默认内容）

```powershell
# 刷写自定义 assets（覆盖默认的）
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m esptool -p COM3 -b 460800 write_flash 0x800000 C:\Users\administered\Downloads\assets_final.bin
```

### 经验教训

> **编译固件时，ESP-IDF 版本必须是 v5.4.2，Python 环境必须是 idf5.4_py3.11_env。**
> 切换环境后必须完全清除 build 目录（`Remove-Item -Recurse build`），不能依赖 `idf.py clean`。
> 每次修改代码编译后，都必须重新刷写自定义 assets.bin。

---

## 三、当前设备状态（2026-04-05）

| 功能 | 状态 | 备注 |
|------|------|------|
| 唤醒词「你好零一」 | ✅ 正常 | MultiNet7，阈值 0.6 |
| EAF 表情动画 | ✅ 正常 | 21 种表情，EmoteDisplay |
| WiFi 连接 | ✅ 正常 | WPA2 |
| 服务器 WebSocket 连接 | ✅ 正常 | `ws://47.109.195.63:17777/ws` |
| 服务器声音播放 | ❌ 不工作 | **需要部署服务器代码**（`websocket_handler.py` 的 3 个 bug 修复） |

### 待办

- [ ] 部署 StarfireServer 到远端 `47.109.195.63`（修复声音播放）
- [ ] 验证端到端交互：唤醒 → ASR → LLM → TTS → 声音播放

---

## 四、文件变更清单

### 固件端（已提交）

| 文件 | 变更说明 |
|------|---------|
| `main/protocols/config.h` | 用户名 sf11→sf12，协议版本 1→3，新增 HTTP Refresh 重试常量 |
| `main/protocols/http_refresh.cc` | 实现 HTTP Refresh 重试循环（最多 3 次，间隔 2s） |
| `main/protocols/protocol.cc` | 超时时间 120→180s |
| `main/protocols/websocket_protocol.cc` | MCP 标记 true→false |
| `main/application.cc` | WebSocket 重连指数退避（5 个重连点），初始 1s，上限 30s |
| `main/application.h` | 新增 `ws_reconnect_delay_seconds_` 和 `kMaxReconnectDelay` |
| `scripts/spiffs_assets/pack_assets_direct.py` | 自定义 assets 打包脚本（唤醒词 + EAF 表情合并） |
| `scripts/spiffs_assets/extract_assets.py` | 从 assets.bin 提取文件的工具 |
| `scripts/spiffs_assets/patch_threshold.py` | 修改 assets.bin 唤醒词阈值的工具 |
| `docs/WAKEWORD_IMPLEMENTATION.md` | 唤醒词实现完整记录 |
| `docs/PROJECT_REFERENCE.md` | 项目参考手册 |
| `docs/CHANGELOG_2026_04.md` | 本文档 |

### 服务器端（未部署）

| 文件 | 变更说明 |
|------|---------|
| `StarfireServer/server/websocket/websocket_handler.py` | 修复 device_type NameError、传递 features、添加 device_id 日志 |
