# EchoEar 项目瘦身实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 xiaozhi-esp32 fork 项目从支持 70+ 设备的通用固件瘦身到只编译 EchoEar（喵伴）设备所需代码，减少编译时间、Flash 占用和代码维护负担。

**Architecture:** 分 6 个阶段渐进式瘦身，每个阶段完成后都能独立编译通过。顺序为：managed components → 源码编译列表 → Kconfig 配置 → CMakeLists 板型分支 → 多语言资源 → 文件清理。核心原则是**每一步都保证 `idf.py build` 能成功**。

**Tech Stack:** ESP-IDF 5.4+, CMake, Kconfig, C++17

**ESP-IDF Path:** `E:\Code\ESP32\SDK\v5.4.2\esp-idf` (WSL: `/mnt/e/Code/ESP32/SDK/v5.4.2/esp-idf`)

**Build command prefix:** 所有 `idf.py` 命令需先 `export IDF_PATH=/mnt/e/Code/ESP32/SDK/v5.4.2/esp-idf` 或 source `export.sh`

**Hardware target:** ESP32-S3, ST77916 QSPI 360x360 LCD, CST816S touch, ES8311+ES7210 audio, WiFi

---

## 前置依赖分析

### EchoEar 实际使用的组件链
```
EchoEar.cc
├── esp_lcd_st77916          (QSPI 显示驱动)
├── esp_lcd_touch_cst816s    (触摸驱动)
│   └── esp_lcd_touch        (触摸基类)
├── esp_codec_dev            (音频编解码抽象)
├── button                   (BOOT 按键)
├── esp-wifi-connect         (WiFi 连接管理)
├── esp-sr                   (语音唤醒/识别)
│   ├── esp-dsp              (DSP)
│   └── dl_fft               (FFT)
├── esp-opus-encoder         (OPUS 音频编码)
│   └── esp-opus             (OPUS 库)
├── xiaozhi-fonts            (字体资源)
├── lvgl                     (图形库)
│   └── esp_lvgl_port        (LVGL ESP-IDF 移植)
├── esp_emote_gfx            (表情动画引擎)
│   ├── esp_new_jpeg         (JPEG 解码)
│   └── freetype             (字体渲染)
├── esp_mmap_assets          (资源分区映射)
└── esp_lcd_panel_io_additions (LCD IO 工具)
    └── esp_io_expander      (IO 扩展器基类)
```

### 编译但可删除的源码依赖链
```
SOURCES 中无条件编译的文件 → 它们引入的 managed component:
oled_display.cc → esp_lvgl_port (已有), 无额外 managed dep
lcd_display.cc → esp_lvgl_port + lvgl (已有), 无额外 managed dep
mqtt_protocol.cc → 无 managed dep (只用 ESP-IDF 内置 mqtt/udp)
single_led.cc → single_led.h → led_strip.h → espressif/led_strip
circular_strip.cc → led_strip.h → espressif/led_strip
gpio_led.cc → 无 managed dep (纯 ESP-IDF LEDC)
```

### 代码中的编译时引用（删文件需同步改代码）
- `application.cc:6` → `#include "mqtt_protocol.h"` (只 include，从未实例化 MqttProtocol)
- `board.cc:5` → `#include "display/oled_display.h"` + `board.cc:162` → `dynamic_cast<OledDisplay*>` 检查

---

## Task 1: 精简 managed components 依赖声明

**Files:**
- Modify: `main/idf_component.yml`

**目标:** 从 37 个依赖减少到 ~14 个，节省编译时间和磁盘空间。

**要删除的依赖（22 个，分三批）:**

### 第一批：其他板子的 LCD/OLED 驱动（9 个）
```yaml
# 删除以下行：
espressif/esp_lcd_ili9341          # ILI9341 SPI LCD
espressif/esp_lcd_gc9a01           # GC9A01 圆屏
espressif/esp_lcd_st7701           # ST7701 LCD
espressif/esp_lcd_st7796           # ST7796 LCD (echoear 用 ST77916)
espressif/esp_lcd_axs15231b        # AXS15231B LCD
espressif/esp_lcd_spd2010          # SPD2010 LCD
78/esp_lcd_nv3023                  # NV3023 LCD
waveshare/esp_lcd_sh8601           # SH8601 AMOLED
tny-robotics/sh1106-esp-idf        # SH1106 OLED
```

### 第二批：其他板子的触摸/IO 扩展器（7 个）
```yaml
# 删除以下行：
espressif/esp_lcd_touch_ft5x06     # FT5x06 触摸
espressif/esp_lcd_touch_gt911      # GT911 触摸
espressif/esp_lcd_touch_gt1151     # GT1151 触摸
waveshare/esp_lcd_touch_cst9217    # CST9217 触摸
espressif/esp_io_expander_tca9554  # TCA9554 IO 扩展
espressif/esp_io_expander_tca95xx_16bit  # TCA95xx IO 扩展
```

### 第三批：其他板子的硬件/功能组件（6 个）
```yaml
# 删除以下行：
espressif/knob                     # 旋钮编码器
espressif/esp32-camera             # 摄像头
espressif/adc_mic                  # ADC 麦克风
espressif/adc_battery_estimation   # ADC 电池估算
78/esp-ml307                       # ML307 4G 模块
wvirgil123/sscma_client            # SenseCAP SSCMA 客户端
```

### 第四批：特定板子的资源组件（2 个）
```yaml
# 删除以下行：
espressif2022/image_player         # ESP-HI 机器狗专用
txp666/otto-emoji-gif-component    # Otto Robot 专用
```

### 第五批：非 ESP32-S3 目标的条件依赖（6 个）
```yaml
# 删除以下行（这些只为 esp32p4/esp32c3 条件编译，ESP32-S3 构建不会下载）：
waveshare/esp_lcd_jd9365_10_1      # ESP32-P4 10.1 寸屏
waveshare/esp_lcd_st7703           # ESP32-P4 ST7703 屏
espressif/esp_lcd_ili9881c         # ESP32-P4 ILI9881C 屏
espressif/esp_hosted               # ESP32-P4/H2 WiFi hosted
espressif/esp_wifi_remote          # ESP32-P4 WiFi remote
espfriends/servo_dog_ctrl          # ESP32-C3 舵机控制
```

- [ ] **Step 1: 编辑 idf_component.yml 删除上述 22 个依赖**

  编辑 `main/idf_component.yml`，删除上述所有条目。  保留的依赖应该只有：
  ```yaml
  dependencies:
    espressif/esp_lcd_st77916: ^1.0.1
    espressif/esp_lcd_touch_cst816s: ^1.0.6
    78/esp-wifi-connect: ~2.5.2
    78/esp-opus-encoder: ~2.4.1
    78/xiaozhi-fonts: ~1.5.3
    espressif/esp_codec_dev: ~1.4.0
    espressif/esp-sr: ~2.1.5
    espressif/button: ~4.1.3
    lvgl/lvgl: ~9.3.0
    esp_lvgl_port: ~2.6.0
    espressif2022/esp_emote_gfx: ^1.1.0
    espressif/esp_mmap_assets: '>=1.2'
    espressif/esp_lcd_panel_io_additions: ^1.0.1
    espressif/led_strip: ~3.0.1

    idf:
      version: '>=5.4.0'
  ```

  > **注意**: `led_strip` 暂时保留，因为 `single_led.cc` 和 `circular_strip.cc` 还在 SOURCES 列表中。后续 Task 3 会删除这些源文件和此依赖。
  >
  > **注意**: `esp_lvgl_port` 没有 `espressif/` 前缀，与原文件保持一致。

- [ ] **Step 2: 清除旧的 managed_components 和 lock 文件**

  ```bash
  rm -rf managed_components/ dependencies.lock
  ```

- [ ] **Step 3: 重新配置让 IDF 下载新依赖**

  ```bash
  idf.py reconfigure
  ```

  预期：成功下载约 20 个组件（而不是之前的 46 个）。

- [ ] **Step 4: 尝试编译，验证无缺失依赖**

  ```bash
  idf.py build
  ```

  预期：可能失败，因为 `oled_display.cc`、`lcd_display.cc`、`mqtt_protocol.cc` 等源文件仍被编译，但它们引用的某些头文件可能缺失。如果失败，记录错误信息，这是预期的——后续 Task 会修复。

  > **如果编译成功**（因为被删组件的头文件没有被直接 include），则本 Task 完成。

- [ ] **Step 5: 提交**

  ```bash
  git add main/idf_component.yml
  git commit -m "slim: remove 22 unused managed component dependencies"
  ```

---

## Task 2: 精简 CMakeLists.txt SOURCES — 删除无用协议和显示驱动

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/application.cc` (移除 mqtt_protocol.h include)

**目标:** 从 SOURCES 列表中移除 MQTT 协议和 OLED 显示的编译。

### 分析

`mqtt_protocol.cc` 被 SOURCES 无条件编译，但 `application.cc` 中只 `#include "mqtt_protocol.h"` 从未创建 `MqttProtocol` 实例（只用 `WebsocketProtocol`）。删除编译 + 移除 include 即可。

`oled_display.cc` 被 SOURCES 无条件编译。`board.cc` 中有 `dynamic_cast<OledDisplay*>` 检查，但 EchoEar 永远不会创建 `OledDisplay`。需要同时修改 `board.cc` 移除该检查。

- [ ] **Step 1: 从 SOURCES 删除 mqtt_protocol.cc**

  编辑 `main/CMakeLists.txt`，从 SOURCES 列表中删除这一行：
  ```cmake
  "protocols/mqtt_protocol.cc"
  ```

- [ ] **Step 2: 从 application.cc 移除 mqtt_protocol.h include**

  编辑 `main/application.cc`，删除：
  ```cpp
  #include "mqtt_protocol.h"
  ```

- [ ] **Step 3: 从 SOURCES 删除 oled_display.cc**

  编辑 `main/CMakeLists.txt`，从 SOURCES 列表中删除：
  ```cmake
  "display/oled_display.cc"
  ```

- [ ] **Step 4: 从 board.cc 移除 oled_display 依赖**

  编辑 `main/boards/common/board.cc`：
  1. 删除 `#include "display/oled_display.h"`
  2. 删除或注释掉 `dynamic_cast<OledDisplay*>` 相关的代码块（约在 162 行附近）。需要查看具体代码决定如何处理，可能是一个 if-else 分支，需要将 OledDisplay 分支删除，只保留其他分支。

  > **注意**: 在执行此步骤前，需要先阅读 `board.cc` 162 行附近的完整代码，理解 `dynamic_cast<OledDisplay*>` 的作用域和逻辑，确保删除后不会影响其他显示类型的正常工作。

- [ ] **Step 5: 编译验证**

  ```bash
  idf.py build
  ```

  预期：编译成功。

- [ ] **Step 6: 提交**

  ```bash
  git add main/CMakeLists.txt main/application.cc main/boards/common/board.cc
  git commit -m "slim: remove MQTT protocol and OLED display from build"
  ```

---

## Task 3: 精简 CMakeLists.txt SOURCES — 删除无用 LED 和音频编解码器

**Files:**
- Modify: `main/CMakeLists.txt`

**目标:** 删除 EchoEar 不使用的 LED 驱动和音频编解码器。

### 分析

EchoEar 的 `BUILTIN_LED_GPIO = GPIO_NUM_NC`，不使用任何 LED。`BoxAudioCodec` 内部使用 ES8311，不依赖独立的 `es8311_audio_codec.cc`。

**安全删除的源文件：**
- `led/single_led.cc` → 引用 `led_strip.h`
- `led/circular_strip.cc` → 引用 `led_strip.h`
- `led/gpio_led.cc` → ESP32-S3 下编译但 EchoEar 不使用
- `audio/codecs/es8374_audio_codec.cc` → 其他板子用的编解码器
- `audio/codecs/es8388_audio_codec.cc` → 其他板子用的编解码器
- `audio/codecs/es8389_audio_codec.cc` → 其他板子用的编解码器
- `audio/codecs/dummy_audio_codec.cc` → 空壳编解码器
- `audio/codecs/no_audio_codec.cc` → 空壳编解码器

**必须保留的：**
- `audio/codecs/box_audio_codec.cc` → EchoEar 用的 ES8311+ES7210
- `audio/codecs/es8311_audio_codec.cc` → BoxAudioCodec 可能内部引用

- [ ] **Step 1: 确认 BoxAudioCodec 是否依赖 es8311_audio_codec**

  ```bash
  grep -n 'es8311_audio_codec\|Es8311AudioCodec' main/audio/codecs/box_audio_codec.cc main/audio/codecs/box_audio_codec.h
  ```

  如果没有引用 → `es8311_audio_codec.cc` 也可以安全删除。
  如果有引用 → 必须保留。

- [ ] **Step 2: 从 SOURCES 删除 LED 文件**

  编辑 `main/CMakeLists.txt` SOURCES 列表，删除：
  ```cmake
  "led/single_led.cc"
  "led/circular_strip.cc"
  "led/gpio_led.cc"
  ```

- [ ] **Step 3: 从 SOURCES 删除无用音频编解码器**

  编辑 `main/CMakeLists.txt` SOURCES 列表，删除：
  ```cmake
  "audio/codecs/no_audio_codec.cc"
  "audio/codecs/es8374_audio_codec.cc"
  "audio/codecs/es8388_audio_codec.cc"
  "audio/codecs/es8389_audio_codec.cc"
  "audio/codecs/dummy_audio_codec.cc"
  ```

  如果 Step 1 确认 `es8311_audio_codec.cc` 也不被 BoxAudioCodec 依赖，也删除它。

- [ ] **Step 4: 现在可以从 idf_component.yml 删除 led_strip 依赖**

  编辑 `main/idf_component.yml`，删除：
  ```yaml
  espressif/led_strip: ~3.0.1
  ```

- [ ] **Step 5: 编译验证**

  ```bash
  rm -rf managed_components/ dependencies.lock
  idf.py reconfigure
  idf.py build
  ```

  预期：编译成功。如果有 linker 错误提示缺少符号，检查是否有其他代码引用了被删源文件中的函数。

- [ ] **Step 6: 提交**

  ```bash
  git add main/CMakeLists.txt main/idf_component.yml
  git commit -m "slim: remove unused LED drivers, audio codecs, and led_strip dep"
  ```

---

## Task 4: 精简 Kconfig.projbuild — 只保留 EchoEar 选项

**Files:**
- Modify: `main/Kconfig.projbuild`

**目标:** 从 598 行配置文件瘦身到约 100 行，移除 70+ 个无用板型选项和子菜单。

### 要保留的 Kconfig 配置

```kconfig
menu "Xiaozhi Assistant"
    # Flash Assets 选择
    choice "Flash Assets" ... endchoice

    # 语言选择 — 全部保留（22 种语言不影响编译体积，只在编译时选一个）
    choice "Default Language" ... endchoice

    # 板型选择 — 只保留 EchoEar
    choice BOARD_TYPE
        prompt "Board Type"
        default BOARD_TYPE_ECHOEAR
        config BOARD_TYPE_ECHOEAR
            bool "EchoEar"
            depends on IDF_TARGET_ESP32S3
    endchoice

    # 显示风格选择 — 保留
    choice DISPLAY_STYLE ... endchoice

    # 唤醒词配置 — 全部保留
    config USE_ESP_WAKE_WORD ...
    config USE_AFE_WAKE_WORD ...
    config USE_CUSTOM_WAKE_WORD ...
    config CUSTOM_WAKE_WORD ...
    config CUSTOM_WAKE_WORD_DISPLAY ...
    config CUSTOM_WAKE_WORD_THRESHOLD ...

    # 音频处理 — 保留
    config USE_AUDIO_PROCESSOR ...
    config USE_DEVICE_AEC ...
    config USE_SERVER_AEC ...
    config USE_AUDIO_DEBUGGER ...
    config USE_ACOUSTIC_WIFI_PROVISIONING ...

    # 接收自定义消息 — 保留
    config RECEIVE_CUSTOM_MESSAGE ...
endmenu
```

### 要删除的 Kconfig 配置

- 所有非 EchoEar 的 `config BOARD_TYPE_*` 条目（~80 个）
- `DISPLAY_OLED_TYPE` 子菜单（OLED 屏幕类型选择）
- `DISPLAY_LCD_TYPE` 子菜单（LCD 屏幕类型选择）
- `DISPLAY_ESP32S3_KORVO2_V3` 子菜单
- `DISPLAY_ESP32S3_AUDIO_BOARD` 子菜单
- `ESP_S3_LCD_EV_Board_Version_TYPE` 子菜单
- `TAIJIPI_I2S_TYPE` 子菜单

- [ ] **Step 1: 重写 Kconfig.projbuild**

  编辑 `main/Kconfig.projbuild`，保留上述"要保留"的配置，删除其余所有板型和子菜单配置。文件应从 598 行减少到约 120 行。

- [ ] **Step 2: 编译验证**

  ```bash
  idf.py build
  ```

  预期：编译成功。如果 Kconfig 语法错误会导致 menuconfig 阶段失败。

- [ ] **Step 3: 提交**

  ```bash
  git add main/Kconfig.projbuild
  git commit -m "slim: remove 70+ unused board type options from Kconfig"
  ```

---

## Task 5: 精简 CMakeLists.txt 板型分支 — 只保留 EchoEar

**Files:**
- Modify: `main/CMakeLists.txt`

**目标:** 从 820 行 CMake 文件瘦身到约 400 行，删除 70+ 个 `elseif(CONFIG_BOARD_TYPE_*)` 分支。

### 要保留的 CMakeLists 逻辑

```cmake
# 第 1-66 行: SOURCES 定义, INCLUDE_DIRS, BOARD_COMMON_SOURCES — 保留
# 第 67-69 行: 默认字体和 BOARD_TYPE — 简化为只设 echoear
# 第 218-222 行: echoear 板型配置 — 保留（移到首位）
# 第 505-507 行: echoear 唯一性检查 — 可以删除（已经没有其他选项了）
# 第 508-512 行: BOARD_SOURCES glob — 保留
# 第 514-525 行: 音频处理器/唤醒词选择 — 保留
# 第 527-572 行: 语言目录选择 — 保留
# 第 574-587 行: ESP32 目标排除 — 可以删除（只编译 ESP32-S3）
# 第 589-593 行: idf_component_register — 保留
# 第 595-603 行: 编译定义 — 保留
# 第 605-620 行: 语言生成 — 保留
# 第 622-632 行: esp-sr 和字体查找 — 保留
# 第 634-669 行: ESP-HI emoji 下载 — 可以删除（echoear 不用）
# 第 672-820 行: assets 构建 — 保留
```

- [ ] **Step 1: 重写 CMakeLists.txt 板型选择区域**

  将第 73-507 行替换为：
  ```cmake
  # EchoEar is the only supported board
  set(BOARD_TYPE "echoear")
  set(BUILTIN_TEXT_FONT font_puhui_20_4)
  set(BUILTIN_ICON_FONT font_awesome_20_4)
  set(DEFAULT_EMOJI_COLLECTION twemoji_64)
  ```

- [ ] **Step 2: 删除 ESP-HI emoji 下载逻辑**

  删除 `if(CONFIG_BOARD_TYPE_ESP_HI)` 块（约第 634-669 行）。

- [ ] **Step 3: 删除 ESP32 目标排除逻辑**

  删除 `if(CONFIG_IDF_TARGET_ESP32)` 块（约第 581-587 行），因为只编译 ESP32-S3。

- [ ] **Step 4: 编译验证**

  ```bash
  idf.py build
  ```

  预期：编译成功。

- [ ] **Step 5: 提交**

  ```bash
  git add main/CMakeLists.txt
  git commit -m "slim: simplify CMakeLists.txt to echoear-only build"
  ```

---

## Task 6: 精简多语言资源

**Files:**
- Delete: `main/assets/locales/` 下 20 个不使用的语言目录
- Modify: `main/Kconfig.projbuild` 中的语言选择菜单（可选）

**目标:** 从 22 种语言减少到 2 种（zh-CN + en-US），减少编译到固件中的资源。

### 要保留的语言
- `zh-CN/` — 中文（主要语言）
- `en-US/` — 英文（备用）

### 要删除的语言（20 个目录）
```
ar-SA, cs-CZ, de-DE, es-ES, fi-FI, fr-FR, hi-IN, id-ID, it-IT,
ja-JP, ko-KR, pl-PL, pt-PT, ro-RO, ru-RU, th-TH, tr-TR, uk-UA,
vi-VN, zh-TW
```

- [ ] **Step 1: 删除不用的语言目录**

  ```bash
  cd main/assets/locales
  rm -rf ar-SA cs-CZ de-DE es-ES fi-FI fr-FR hi-IN id-ID it-IT \
         ja-JP ko-KR pl-PL pt-PT ro-RU ru-RU th-TH tr-TR uk-UA \
         vi-VN zh-TW
  ```

- [ ] **Step 2: 更新 Kconfig 中的语言选项（可选）**

  如果希望 menuconfig 也只显示中文和英文，编辑 `main/Kconfig.projbuild` 的 `choice "Default Language"` 块，只保留：
  ```kconfig
  config LANGUAGE_ZH_CN
      bool "Chinese"
  config LANGUAGE_EN_US
      bool "English"
  ```

- [ ] **Step 3: 编译验证**

  ```bash
  idf.py build
  ```

  预期：编译成功。

- [ ] **Step 4: 提交**

  ```bash
  git add -A
  git commit -m "slim: reduce language support to zh-CN and en-US only"
  ```

---

## Task 7: 删除无用源文件

**Files:**
- Delete: `main/protocols/mqtt_protocol.cc`, `main/protocols/mqtt_protocol.h`
- Delete: `main/display/oled_display.cc`, `main/display/oled_display.h`
- Delete: `main/led/single_led.cc`, `main/led/single_led.cc`, `main/led/circular_strip.cc`, `main/led/circular_strip.h`, `main/led/gpio_led.cc`, `main/led/gpio_led.h`
- Delete: `main/audio/codecs/es8374_audio_codec.cc/.h`, `es8388_audio_codec.cc/.h`, `es8389_audio_codec.cc/.h`, `dummy_audio_codec.cc/.h`, `no_audio_codec.cc/.h`（如果 Task 3 确认 es8311 也可删则一并删除）
- Delete: `main/boards/common/ml307_board.cc/.h`, `dual_network_board.cc/.h`, `axp2101.cc/.h`, `esp32_camera.cc/.h`, `knob.cc/.h`, `lamp_controller.h`, `afsk_demod.cc/.h`, `camera.h`
- Delete: 其他板子的 `sdkconfig.defaults.*` 文件（保留 `sdkconfig.defaults` 和 `sdkconfig.defaults.esp32s3`）
- Delete: `main/boards/README.md`（上游的多板指南，已不适用）

**目标:** 物理删除所有不再编译的源文件，保持仓库干净。

- [ ] **Step 1: 删除 MQTT 协议文件**

  ```bash
  rm main/protocols/mqtt_protocol.cc main/protocols/mqtt_protocol.h
  ```

- [ ] **Step 2: 删除 OLED 显示文件**

  ```bash
  rm main/display/oled_display.cc main/display/oled_display.h
  ```

- [ ] **Step 3: 删除 LED 文件**

  ```bash
  rm main/led/single_led.cc main/led/single_led.h
  rm main/led/circular_strip.cc main/led/circular_strip.h
  rm main/led/gpio_led.cc main/led/gpio_led.h
  ```

- [ ] **Step 4: 删除无用音频编解码器文件**

  ```bash
  rm main/audio/codecs/es8374_audio_codec.cc main/audio/codecs/es8374_audio_codec.h
  rm main/audio/codecs/es8388_audio_codec.cc main/audio/codecs/es8388_audio_codec.h
  rm main/audio/codecs/es8389_audio_codec.cc main/audio/codecs/es8389_audio_codec.h
  rm main/audio/codecs/dummy_audio_codec.cc main/audio/codecs/dummy_audio_codec.h
  rm main/audio/codecs/no_audio_codec.cc main/audio/codecs/no_audio_codec.h
  ```
  如果 Task 3 Step 1 确认 es8311_audio_codec 也不被 BoxAudioCodec 依赖：
  ```bash
  rm main/audio/codecs/es8311_audio_codec.cc main/audio/codecs/es8311_audio_codec.h
  ```

- [ ] **Step 5: 删除无用 boards/common 文件**

  ```bash
  rm main/boards/common/ml307_board.cc main/boards/common/ml307_board.h
  rm main/boards/common/dual_network_board.cc main/boards/common/dual_network_board.h
  rm main/boards/common/axp2101.cc main/boards/common/axp2101.h
  rm main/boards/common/esp32_camera.cc main/boards/common/esp32_camera.h
  rm main/boards/common/knob.cc main/boards/common/knob.h
  rm main/boards/common/lamp_controller.h
  rm main/boards/common/afsk_demod.cc main/boards/common/afsk_demod.h
  rm main/boards/common/camera.h
  ```

  > **注意**: `sy6970.cc/h` 和 `adc_battery_monitor.cc/h` 暂时保留——EchoEar 有充电 IC（I2C 0x55），可能依赖这些通用模块。

- [ ] **Step 6: 删除其他板子的 sdkconfig.defaults**

  ```bash
  rm sdkconfig.defaults.esp32 sdkconfig.defaults.esp32c3 sdkconfig.defaults.esp32c6 sdkconfig.defaults.esp32p4
  # 保留: sdkconfig.defaults, sdkconfig.defaults.esp32s3
  ```

- [ ] **Step 7: 删除过时的文档**

  ```bash
  rm main/boards/README.md  # 上游多板指南，已不适用
  ```

- [ ] **Step 8: 编译验证**

  ```bash
  idf.py build
  ```

  预期：编译成功。如果有错误，检查是否有遗漏的 #include 引用被删文件的头文件。

- [ ] **Step 9: 提交**

  ```bash
  git add -A
  git commit -m "slim: delete all unused source files for non-echoear hardware"
  ```

---

## Task 8: 最终验证与清理

**Files:**
- All modified files

**目标:** 全面验证瘦身后项目能正常编译，记录瘦身效果。

- [ ] **Step 1: 完整 clean build**

  ```bash
  rm -rf build managed_components dependencies.lock
  idf.py set-target esp32s3
  idf.py build
  ```

  预期：编译成功。

- [ ] **Step 2: 记录瘦身效果**

  ```bash
  # 统计文件数量
  find main/ -name '*.cc' -o -name '*.c' -o -name '*.h' | wc -l

  # 统计 managed components 数量
  ls managed_components/ | wc -l

  # 统计编译产物大小
  ls -lh build/*.bin
  ```

  对比瘦身前的数据，记录在 commit message 中。

- [ ] **Step 3: 最终提交**

  ```bash
  git add -A
  git commit -m "slim: echoear-only build complete

  Removed: 22 managed components, 70+ board configs, 20 unused languages,
  MQTT protocol, OLED display, LED strip drivers, 5 unused audio codecs,
  and 8 unused board common modules."
  ```

---

## 风险矩阵

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 删 managed component 后编译失败 | 中 | 低 | 按批次删除，每批后编译验证 |
| board.cc 的 OledDisplay dynamic_cast | 低 | 低 | 删除 OLED 编译前先改 board.cc |
| BoxAudioCodec 依赖 es8311_audio_codec | 低 | 中 | Task 3 Step 1 先确认再删 |
| led_strip 组件被其他代码间接引用 | 低 | 低 | grep 全局搜索确认 |
| 未来想加回某个板子支持 | 低 | 低 | 用 git 历史恢复被删文件 |

## 预期瘦身效果

| 指标 | 瘦身前 | 瘦身后 | 变化 |
|------|--------|--------|------|
| managed_components 数量 | 46 | ~20 | -57% |
| idf_component.yml 依赖数 | 37 | ~14 | -62% |
| Kconfig 板型选项 | ~80 | 1 | -99% |
| 语言支持 | 22 | 2 | -91% |
| 源码文件数 | ~80 | ~50 | -38% |
| CMakeLists.txt 行数 | 820 | ~400 | -51% |
| 首次编译时间 | 基线 | 预计减少 40-50% | — |

> **注意**: Flash 占用主要由 esp-sr (211MB on disk)、lvgl (157MB)、xiaozhi-fonts (53MB) 三个大组件决定。这些是 EchoEar 必需的，无法进一步减少。瘦身的收益主要体现在编译时间和代码维护复杂度上。
