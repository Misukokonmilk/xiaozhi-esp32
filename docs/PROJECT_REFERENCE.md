# EchoEar 项目参考手册

> xiaozhi-esp32 EchoEar 固件开发速查手册。本文档是 WAKEWORD_IMPLEMENTATION.md 的配套参考，专注于「怎么用」而非「怎么做的」。

---

## 1. 项目概述

| 项目 | 说明 |
|------|------|
| 名称 | xiaozhi-esp32 — 基于 MCP 协议的 ESP32 AI 语音聊天机器人 |
| 上游仓库 | https://github.com/78/xiaozhi-esp32 (MIT 协议, 作者: 虾哥) |
| 本地分支 | `feature/echoear-slim` (EchoEar 定制版) |
| 固件代号 | EchoEar |

### 核心功能

- Wi-Fi 语音对话 (流式 ASR + LLM + TTS 管线，服务端处理)
- 离线唤醒词 "你好零一" (MultiNet6, 本地推理)
- EAF 表情动画显示 (21 种表情, RGB565 LCD)
- OPUS 音频编解码 (16kHz 单声道)
- 双协议支持: WebSocket / MQTT+UDP

### 系统架构

```
┌─────────────┐   Wi-Fi    ┌──────────────┐
│  EchoEar    │◄──────────►│  Starfire    │
│  ESP32-S3   │  WebSocket │  Server      │
│             │            │              │
│ · 唤醒词    │   OPUS     │ · ASR (语音转文字)
│ · 麦克风    │◄──────────►│ · LLM (大模型对话)
│ · 扬声器    │   OPUS     │ · TTS (文字转语音)
│ · LCD表情   │            │              │
└─────────────┘            └──────────────┘
```

---

## 2. 设备信息

### 2.1 硬件规格

| 项目 | 值 |
|------|-----|
| 芯片 | ESP32-S3 (16MB Flash, QSPI) |
| 显示屏 | 320×240 RGB565 LCD |
| 音频 | 16kHz 单声道, OPUS 编解码 |
| 连接 | Wi-Fi (WPA2), 无 4G |
| 表情显示 | EmoteDisplay (`CONFIG_USE_EMOTE_MESSAGE_STYLE`) |
| 唤醒词 | MultiNet7 "你好零一" (ni hao ling yi), 阈值 0.45 |

### 2.2 板级配置

- 配置目录: `main/boards/echoear/`
- 配置文件:

| 文件 | 用途 |
|------|------|
| `config.h` | WiFi / 服务器 / 协议配置 |
| `config.json` | 板级 JSON 配置 |
| `EchoEar.cc` | 板级初始化代码 |
| `emote.json` | 表情映射 (21 种表情 → EAF 文件) |
| `layout.json` | 显示布局定义 |
| `touch.h` | 触摸按键配置 |

- 表情系统: 21 个 emote 定义在 `emote.json` 中，使用 EAF 动画文件渲染

### 2.3 服务器配置 (config.h)

| 项目 | 值 |
|------|-----|
| 服务器 | `47.109.195.63:17777` (Starfire) |
| 用户名 | `sf12` |
| 密码 | `123456` |
| 协议 | WebSocket (`ws://47.109.195.63:17777/ws`) |
| OTA | 已禁用 (板级配置中已注释) |

---

## 3. 开发环境

### 3.1 路径一览

| 项目 | 路径 |
|------|------|
| 项目目录 | `E:\Code\ESP32\SF\xiaozhi-esp32` |
| ESP-IDF SDK | `E:\Code\ESP32\SDK\v5.4.2\esp-idf` |
| Python 环境 | `E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env` |
| IDF Tools | `E:\Code\ESP32\SDK\.espressif` |
| EAF 资源 | `managed_components\espressif2022__esp_emote_gfx\emoji_large` |
| Git 分支 | `feature/echoear-slim` |

### 3.2 Python 依赖

ESP-IDF Python 环境需要额外安装 numpy 和 Pillow (用于资源打包脚本):

```powershell
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m pip install numpy Pillow
```

---

## 4. 编译命令

### 4.1 激活 ESP-IDF 环境 (PowerShell)

```powershell
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"; $env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"; $activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1; & $activateScript
```

### 4.2 设置 EAF 资源路径

```powershell
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
```

> ⚠️ **必须设置！** 不设置此变量，构建系统只打包默认 PNG 资源，EAF 动画不会包含进固件。

### 4.3 构建

```powershell
# 全量构建 (修改了资源或配置后使用)
idf.py clean
idf.py build

# 增量构建 (仅修改代码时使用)
idf.py build
```

### 4.4 一次性构建脚本 (完整版)

可直接复制粘贴到 PowerShell 执行:

```powershell
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
$activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1
& $activateScript
Set-Location "E:\Code\ESP32\SF\xiaozhi-esp32"
idf.py build
```

---

## 5. 刷写命令

### 5.1 方法一: 标准刷写 (仅固件)

```powershell
idf.py -p COM3 flash
```

> ⚠️ 此命令会同时写入 assets 分区 (0x800000)，使用构建系统生成的 `generated_assets.bin`。该文件**不包含 multinet_model 配置**。如果需要唤醒词功能，必须再执行方法二。

### 5.2 方法二: 刷写自定义 assets (带唤醒词)

先刷固件，再单独刷自定义 assets:

```powershell
# 第一步: 刷固件
idf.py -p COM3 flash

# 第二步: 刷自定义 assets (覆盖构建系统的默认 assets)
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m esptool -p COM3 -b 460800 write_flash 0x800000 C:\Users\administered\Downloads\assets_final.bin
```

### 5.3 分区布局

| 分区 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| App | 0x20000 | - | 固件主程序 |
| Assets | 0x800000 | 8MB | 资源分区 (srmodels + emoji + icons + index.json) |

### 5.4 生成自定义 assets.bin

```powershell
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" "E:\Code\ESP32\SF\xiaozhi-esp32\scripts\spiffs_assets\pack_assets_direct.py"
```

- 输出: `C:\Users\administered\Downloads\assets_final.bin`
- 大小: 6,159,927 字节 (23 个文件)

---

## 6. 监控日志

### 6.1 启动 Monitor

```powershell
idf.py -p COM3 monitor
```

退出快捷键: `Ctrl+]`

### 6.2 刷写 + Monitor (一步到位)

```powershell
idf.py -p COM3 flash monitor
```

### 6.3 关键日志关键词

| 关键词 | 含义 |
|--------|------|
| `Successfully load srmodels` | 语音模型加载成功 |
| `SetEmotion: idle → neutral` | 表情初始化成功 |
| `Command: ni hao ling yi` | 唤醒词配置正确 |
| `set det threshold to 0.600000` | 检测阈值设置正确 |
| `1 active speech commands` | 唤醒词已注册 |
| `WiFi connected` | Wi-Fi 连接成功 |
| `Login successful` / `STATE: idle` | 服务器连接正常 |
| `No emoji data found` | ⚠️ 表情资源缺失 (BOARD_RES_PATH 未设置) |
| `multinet_model: NOT FOUND` | ⚠️ 唤醒词配置缺失 (需要刷自定义 assets) |

---

## 7. 项目目录结构 (关键文件)

```
xiaozhi-esp32/
├── main/
│   ├── boards/echoear/               # ★ EchoEar 板级配置
│   │   ├── config.h                  #   WiFi / 服务器配置
│   │   ├── config.json               #   板级 JSON 配置
│   │   ├── EchoEar.cc                #   板级初始化代码
│   │   ├── emote.json                #   表情映射 (21 种 → EAF 文件)
│   │   ├── layout.json               #   显示布局
│   │   └── touch.h                   #   触摸按键
│   ├── protocols/config.h            #   全局协议配置 (服务器地址/端口/凭证)
│   └── ...
├── managed_components/
│   └── espressif2022__esp_emote_gfx/
│       └── emoji_large/              # EAF 动画 + 图标资源
│           ├── Happy.eaf             #   表情动画文件
│           ├── Sad.eaf
│           ├── neutral.eaf
│           ├── ...
│           ├── icon_mic.bin          #   麦克风图标
│           └── battery_*.bin         #   电池图标
├── scripts/spiffs_assets/
│   ├── pack_assets_direct.py         # ★ 自定义 assets 打包 (含唤醒词 + EAF)
│   ├── extract_assets.py             #   从 assets.bin 提取文件
│   ├── patch_threshold.py            #   修改已有 assets.bin 的 threshold
│   └── spiffs_assets_gen.py          #   原始打包工具
├── docs/
│   ├── WAKEWORD_IMPLEMENTATION.md    #   唤醒词实现记录 (完整过程)
│   ├── BUILD_COMMANDS.md             #   构建命令备忘 (旧版)
│   ├── CUSTOMIZATION_WAKEWORD.md     #   唤醒词配置指南
│   ├── CUSTOMIZATION_EMOJI.md        #   表情配置指南
│   └── PROJECT_REFERENCE.md          #   ★ 本文档
└── README.md
```

---

## 8. 常见问题排查

### 表情不显示 / `No emoji data found`

1. 确认设置了 `BOARD_RES_PATH` 环境变量
2. 执行 `idf.py clean` 清除旧构建产物
3. 重新执行 `idf.py build`

### 唤醒词不工作 / `multinet_model: NOT FOUND`

`idf.py flash` 生成的 assets.bin **不包含** multinet_model。必须单独刷写自定义 assets:

```powershell
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m esptool -p COM3 -b 460800 write_flash 0x800000 C:\Users\administered\Downloads\assets_final.bin
```

### 误触发 (唤醒词太灵敏)

当前阈值为 0.6。值越高越不容易触发。修改 `scripts/spiffs_assets/pack_assets_direct.py` 中的 threshold 参数，重新生成 assets.bin 并刷写。

### `idf.py` 命令找不到

未激活 ESP-IDF 环境。参照第 4.1 节激活。

### `ModuleNotFoundError: numpy`

ESP-IDF Python 环境缺少依赖。参照第 3.2 节安装。

### Wi-Fi 连接失败

检查 `main/boards/echoear/config.h` 中的 WiFi SSID 和密码是否正确。

### 服务器连接失败

检查 `main/protocols/config.h` 和 `main/boards/echoear/config.h` 中的服务器地址、端口、用户名、密码。

---

## 9. 快速操作速查

| 场景 | 命令 |
|------|------|
| 激活环境 | 见 4.1 节 |
| 设置资源路径 | `$env:BOARD_RES_PATH = "...emoji_large"` |
| 全量构建 | `idf.py clean; idf.py build` |
| 增量构建 | `idf.py build` |
| 刷固件 | `idf.py -p COM3 flash` |
| 刷自定义 assets | `esptool ... write_flash 0x800000 assets_final.bin` |
| 查看日志 | `idf.py -p COM3 monitor` |
| 刷写 + 日志 | `idf.py -p COM3 flash monitor` |
| 打包 assets | `python pack_assets_direct.py` |
| 提取 assets | `python extract_assets.py` |
| 修改阈值 | `python patch_threshold.py` |

---

*最后更新: 2026-04-04*
