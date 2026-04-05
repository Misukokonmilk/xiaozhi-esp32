# 小智 AI 表情配置指南

## 概述

小智 AI 支持两种表情格式：
1. **EAF 动画格式** - 来自 `esp_emote_gfx` 组件的动画文件（当前项目使用）
2. **PNG/GIF 格式** - 来自 `xiaozhi-assets-generator` 的图片格式

## 当前项目的表情配置

### 资源路径

`BOARD_RES_PATH` 环境变量指向：
```
E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large
```

该目录下包含 EAF 动画文件和图标文件：
- `Happy.eaf`, `Sad.eaf`, `angry.eaf`, `confused.eaf`, `cry.eaf`, `shocked.eaf`, `sleep.eaf`, `winking.eaf`, `neutral.eaf`, `listen.eaf`
- `battery_charge.bin`, `battery_level1~4.bin`
- `icon_mic.bin`, `icon_speaker.bin`, `icon_tips.bin`, `icon_WiFi_fail.bin`, `icon_wifi_ok.bin`

### 表情映射配置

文件：`main/boards/echoear/emote.json`

```json
{
  "emotes": [
    {"name": "happy", "file": "Happy.eaf", "loop": true, "fps": 20},
    {"name": "sad", "file": "Sad.eaf", "loop": true, "fps": 20},
    ...
  ],
  "icons": [
    {"name": "battery_charge", "file": "battery_charge.bin"},
    {"name": "icon_mic", "file": "icon_mic.bin"},
    ...
  ]
}
```

EmoteDisplay 代码中使用 `SetEmotion("happy")` 等调用，通过 emote.json 映射找到对应的 EAF 文件。

### 21 种表情

| 情绪名称 | 文件 | 说明 |
|---------|------|------|
| neutral | neutral.eaf | 默认/待机表情 |
| happy | Happy.eaf | 开心 |
| laughing | Happy.eaf | 大笑 |
| funny | Happy.eaf | 有趣 |
| loving | Happy.eaf | 喜爱 |
| embarrassed | Happy.eaf | 尴尬 |
| confident | Happy.eaf | 自信 |
| delicious | Happy.eaf | 美味 |
| sad | Sad.eaf | 悲伤 |
| crying | cry.eaf | 哭泣 |
| sleepy | sleep.eaf | 困倦 |
| silly | Happy.eaf | 愚蠢 |
| angry | angry.eaf | 生气 |
| surprised | Happy.eaf | 惊讶 |
| shocked | shocked.eaf | 震惊 |
| thinking | confused.eaf | 思考 |
| winking | neutral.eaf | 眨眼 |
| relaxed | Happy.eaf | 放松 |
| confused | confused.eaf | 困惑 |

### 代码中的表情调用

```cpp
// 设置表情
display->SetEmotion("idle");    // 待机
display->SetEmotion("happy");   // 开心
display->SetEmotion("sad");      // 悲伤
display->SetEmotion("neutral");  // 中性

// 设置图标
display->SetIcon("icon_mic");           // 麦克风图标
display->SetIcon("icon_speaker");       // 喇叭图标
display->SetIcon("icon_WiFi_fail");     // WiFi 失败图标
display->SetIcon("icon_wifi_ok");        // WiFi 正常图标
display->SetIcon("battery_charge");      // 充电图标
```

## 使用 xiaozhi-assets-generator 自定义表情

### 方式一：使用在线工具

访问 https://github.com/78/xiaozhi-assets-generator

1. 选择芯片型号和屏幕分辨率
2. Tab 3 选择"表情集合"
3. 选择预设表情包（Twemoji）或上传自定义图片
4. 点击生成 assets.bin

### 方式二：手动替换 EAF 文件

1. 准备 21 张尺寸统一的图片
2. 转换为 EAF 格式或 PNG 格式
3. 替换 `emoji_large/` 目录下的文件
4. 更新 `emote.json` 中的映射
5. 设置 `BOARD_RES_PATH` 重新构建

### 表情图片要求

- 尺寸：建议 64x64 或 128x128（根据屏幕分辨率调整）
- 格式：PNG（静态）或 GIF（动态）
- 必须包含 neutral.png 作为默认表情

## 图标文件说明

当前 `emoji_large/` 中的图标文件：

| 文件名 | 用途 |
|--------|------|
| battery_charge.bin | 充电中 |
| battery_level1~4.bin | 电量 1-4 级 |
| icon_mic.bin | 麦克风 |
| icon_speaker.bin | 喇叭 |
| icon_speaker_zzz.bin | 喇叭+睡眠 |
| icon_tips.bin | 提示 |
| icon_WiFi_fail.bin | WiFi 失败 |
| icon_wifi_ok.bin | WiFi 正常 |

**注意**：`icon_Battery` 是 EmoteDisplay 代码中使用的图标名称，但 `emoji_large/` 中对应的文件是 `battery_level*.bin`，存在命名不一致问题。

## 构建时打包表情资源

确保设置 `BOARD_RES_PATH`：

```powershell
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
idf.py clean
idf.py build
idf.py -p COM3 flash
```
