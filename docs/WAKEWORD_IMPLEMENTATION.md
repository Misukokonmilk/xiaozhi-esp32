# 自定义唤醒词「你好零一」实现记录

> **项目**: xiaozhi-esp32 `feature/echoear-slim` 分支
> **硬件**: EchoEar (ESP32-S3, 16MB QSPI Flash, 320x240 RGB565 屏幕)
> **目标**: 为 EchoEar 固件添加自定义唤醒词「你好零一」，同时保留 EAF 动画表情系统
> **日期**: 2026 年 4 月

---

## 一、概述

EchoEar 是基于 ESP32-S3 的语音交互设备，自带屏幕可以显示表情动画。出厂固件使用乐鑫预设唤醒词，我们的需求很明确: 换成自定义唤醒词「你好零一」，同时保留设备上已有的 EAF 格式表情动画。

听起来简单，实际踩了三个坑: 表情消失、唤醒词失效、误触发。本文记录完整的排查和解决过程。

---

## 二、背景知识

开始之前，需要理解几个关键概念。

### ESP-SR 唤醒词体系

乐鑫的 ESP-SR 提供两条路线:

| | WakeNet | MultiNet |
|---|---|---|
| 用途 | 预设唤醒词（"Hi 乐鑫"等） | 自定义唤醒词 |
| 芯片要求 | ESP32/ESP32-S2/S3 均可 | 仅 ESP32-S3 |
| 灵活性 | 低，词表固定 | 高，可自定义命令文本 |

我们要做的「你好零一」属于自定义唤醒词，只能走 MultiNet 路线，幸好 EchoEar 用的正是 ESP32-S3。

### assets.bin 的格式

表情、唤醒词模型、字体等资源全部打包成一个 `assets.bin` 文件，采用 mmap_assets 二进制格式:

```
+------------------+
| Header (12 字节)  |  total_files(4B) + checksum(4B) + combined_len(4B)
+------------------+
| Entry 表         |  每条 44 字节，结构: name(32B) + size(4B) + offset(4B) + width(2B) + height(2B)
+------------------+
| 文件数据          |  每个文件以 0x5A5A 前缀开头，后跟原始文件内容
+------------------+
```

固件启动时通过 mmap 将这个文件映射到内存，按名字索引读取各个资源。

### index.json: 串联一切的核心配置

assets.bin 里有一个 `index.json` 文件，它是整个资源系统的"目录"。固件代码通过解析这个 JSON 来知道:

- 唤醒词有哪些命令（`multinet_model.commands`）
- 置信度阈值是多少（`multinet_model.threshold`）
- 表情动画文件在哪里（`emoji_collection`）
- 图标资源怎么组织（`icon_collection`）

可以说，index.json 是理解整个资源系统的钥匙。

### 两套构建管线

项目中存在两套 assets 构建管线，这是后续问题的根源:

1. **xiaozhi-assets-generator**: 支持 PNG 表情 + 自定义唤醒词，输出的是 Twemoji/PNG 格式表情
2. **esp_emote_gfx**: 支持 EAF 动画表情（带帧动画），但没有唤醒词配置能力

我们的需求同时涉及这两者，但任何一套管线都无法单独满足。

---

## 三、问题一: 自定义唤醒词成功，但表情消失了

### 操作

使用 `xiaozhi-assets-generator` 工具，输入命令 `ni hao ling yi` 生成新的 assets.bin。刷入设备后测试。

### 现象

唤醒词「你好零一」正常工作，但屏幕上的表情完全消失了，只剩空白。

### 排查过程

EmoteDisplay 是 EchoEar 的表情渲染模块，使用 `CONFIG_USE_EMOTE_MESSAGE_STYLE` 配置。它需要从 assets 中加载 EAF 格式的动画文件。

关键代码在 `assets.cc` 第 275-313 行，`emoji_collection` 的解析逻辑:

```cpp
// assets.cc - 简化的解析流程
auto emoji_collection = index_json["emoji_collection"];
for (auto& [name, info] : emoji_collection.items()) {
    // 检查是否有 "eaf" 字段
    if (info.contains("eaf")) {
        auto eaf_obj = info["eaf"];
        // 读取 loop 和 fps 参数
        bool loop = eaf_obj.value("loop", false);
        int fps = eaf_obj.value("fps", 15);
        // 调用 AddEmojiData 注册表情
        AddEmojiData(name, file_data, loop, fps);
    }
}
```

问题清楚了: `xiaozhi-assets-generator` 生成的 index.json 中，`emoji_collection` 里只有 PNG 图片信息，没有 `eaf` 对象。`AddEmojiData()` 永远不会被调用，`emoji_data_map_` 是空的，自然没有表情可显示。

### 根因

两套构建管线的表情格式不兼容。`xiaozhi-assets-generator` 输出 PNG/Twemoji 格式，而 EmoteDisplay 需要 EAF 动画格式，且必须在 index.json 中包含 `eaf` 对象（含 `loop` 和 `fps` 字段）。

---

## 四、问题二: 修好了表情，唤醒词却失效了

### 操作

既然单独用哪套管线都不行，我们尝试"缝合": 用 esp_emote_gfx 管线构建带 EAF 动画的 assets，然后把 xiaozhi-assets-generator 生成 assets.bin 中的 `srmodels.bin`（语音模型文件）合并进去。

### 现象

表情动画回来了，但唤醒词完全没反应，设备对「你好零一」毫无响应。

### 排查过程

首先确认 srmodels.bin 本身没问题。对比发现两套管线生成的 srmodels.bin **完全相同**，都是约 3.7MB 的 MultiNet6 模型文件。模型没问题，那问题出在哪？

关键发现来自 `CustomWakeWord::ParseWakenetModelConfig()` 的代码逻辑: 这个函数从 `index.json` 读取 `multinet_model` 节点来注册唤醒词命令。esp_emote_gfx 管线生成的 index.json 里**根本没有** `multinet_model` 这个段落。

```cpp
// CustomWakeWord 解析逻辑（简化）
auto multinet = index_json["multinet_model"];
auto commands = multinet["commands"];
for (auto& cmd : commands) {
    RegisterCommand(cmd["command"], cmd["text"], cmd["action"]);
}
```

没有 `multinet_model`，就没有 commands 数组，`RegisterCommand` 永远不会被调用。

### 根因

**唤醒词配置不在 srmodels.bin 里，而是在 index.json 的 `multinet_model.commands` 数组中。** srmodels.bin 只是 MultiNet6 的推理模型，真正定义"听什么词、触发什么动作"的是 index.json。

这是一个颠覆性的认知: 之前一直以为语音模型文件包含了唤醒词信息，实际上它只是个通用的声学模型，唤醒词的具体文本配置完全在 JSON 层面。

---

## 五、问题三: 误触发严重

### 现象

唤醒词和表情都搞定后，发现设备会随机进入监听模式。没人说话，它自己就醒了。环境中的普通噪音、空调声、甚至走路声都会触发。

### 排查

查看 `xiaozhi-assets-generator` 生成的 index.json，发现 `multinet_model.threshold` 设为 `0.2`，也就是 20% 的置信度就认为匹配成功。这个值在安静实验室里可能没问题，但在真实环境中太激进了。

### 修复

在自定义的打包脚本中，将阈值提高到 `0.6`:

```json
"multinet_model": {
    "model_type": "mn6_cn",
    "threshold": 0.6,
    "commands": [...]
}
```

实际测试 0.6 是个合理的平衡点: 既不会频繁误触发，又保持了较好的唤醒灵敏度。

---

## 六、最终解决方案: pack_assets_direct.py

经过三个问题的教训，思路已经很清晰: 需要一个统一的打包脚本，把两种管线的产出合并到同一个 index.json 里。

### 脚本位置

`scripts/spiffs_assets/pack_assets_direct.py`

### 工作流程

脚本分五步走:

**第一步: 提取原始 assets.bin 中的关键文件**

从 xiaozhi-assets-generator 生成的 assets.bin（4,944,814 字节）中，提取:
- `srmodels.bin`（MultiNet6 声学模型）
- `font.bin`（字体文件）
- `index.json`（重点关注其中的 `multinet_model` 段落）

**第二步: 复制 EAF 动画文件**

从 `managed_components/espressif2022__esp_emote_gfx/emoji_large/` 目录复制所有 EAF 动画文件。这些文件是 EmoteDisplay 需要的帧动画数据。

**第三步: 复制图标文件**

复制所有 icon bin 文件到资源集合。

**第四步: 构建完整的 index.json**

这是最关键的一步。新构建的 index.json 必须包含所有段落:

```json
{
    "multinet_model": {
        "model_type": "mn6_cn",
        "threshold": 0.6,
        "commands": [
            {
                "command": "ni hao ling yi",
                "text": "零一",
                "action": "wake"
            }
        ]
    },
    "emoji_collection": {
        "neutral": {
            "type": "eaf",
            "file": "neutral.eaf",
            "eaf": { "loop": true, "fps": 15 }
        },
        "happy": {
            "type": "eaf",
            "file": "happy.eaf",
            "eaf": { "loop": true, "fps": 15 }
        }
    },
    "icon_collection": { ... },
    "layout": { ... }
}
```

注意 `emoji_collection` 中每个表情都有 `eaf` 对象，包含 `loop` 和 `fps`。表情定义来自 `main/boards/echoear/emote.json`（共 21 个表情，部分共用同一个 EAF 文件）。

**第五步: 打包成 mmap_assets 二进制格式**

按前文描述的二进制格式（12B header + 44B entries + 0x5A5A prefixed data）打包所有文件。

### 输出结果

```
assets_final.bin — 6,159,927 字节，包含 23 个文件
```

---

## 七、刷写流程

资源打包好之后，刷写也有坑。不能直接 `idf.py flash` 就完事。

### 第一步: 正常刷写固件

```bash
idf.py -p COM3 flash
```

这一步会编译并刷入固件代码。但有个问题: 构建系统会自动生成一个 `generated_assets.bin` 并刷入 assets 分区（偏移 `0x800000`，大小 8MB）。这个自动生成的 bin 文件**不包含** `multinet_model` 段落，所以唤醒词不能用。

### 第二步: 单独刷入资源文件

固件刷完后，必须用 esptool 单独把我们的 assets_final.bin 写入 assets 分区:

```bash
esptool.py -p COM3 -b 460800 write_flash 0x800000 assets_final.bin
```

偏移地址 `0x800000` 对应分区表中定义的 assets 分区起始地址。波特率 `460800` 用于加速传输（资源文件有 6MB）。

两步缺一不可。如果只做第一步，表情可能有（取决于构建系统生成了什么），但唤醒词一定不能用。

---

## 八、验证成功的启动日志

刷入后，通过串口监视器观察启动日志。以下关键行确认一切正常:

```
MODEL_LOADER: Successfully load srmodels
```

语音模型加载成功，srmodels.bin 数据完整。

```
EmoteDisplay: SetEmotion: idle → neutral
```

表情系统初始化成功，能正确显示 neutral 表情。说明 EAF 文件和 index.json 中的 `eaf` 配置都正确。

```
CustomWakeWord: Command: ni hao ling yi, Text: 零一, Action: wake
```

自定义唤醒词命令注册成功。`multinet_model.commands` 被正确解析。

```
set det threshold to 0.600000
```

检测阈值设为 0.6，确认我们的阈值配置生效了。

```
1 active speech commands: Command 1: ni hao ling yi
```

最终确认: 只有 1 个活跃的语音命令，就是我们自定义的「你好零一」。

---

## 九、经验总结

回顾整个过程，几个核心认知值得记录:

**唤醒词配置在 index.json 里，不在 srmodels.bin 里。**

srmodels.bin 是通用的 MultiNet6 声学模型，两套管线生成的完全一样（都是 3.7MB）。真正定义"听什么词"的是 index.json 中的 `multinet_model.commands` 数组。搞混这一点会导致大量无效的排查。

**EAF 表情和 MultiNet 唤醒词可以共存。**

它们并不冲突，关键是 index.json 必须同时包含 `multinet_model` 段落（唤醒词配置）和带 `eaf` 对象的 `emoji_collection`（表情配置）。两套构建管线各自只覆盖了一半，所以需要手动合并。

**默认阈值 0.2 在真实环境中太敏感。**

xiaozhi-assets-generator 默认的 0.2 阈值可能是为演示场景设计的。真实使用环境中，空调噪音、键盘敲击、脚步声都可能触发。0.6 是实测可用的平衡点。

**构建系统的 generated_assets.bin 不会包含 multinet_model。**

`idf.py flash` 会刷入构建系统自动生成的 assets bin，这个 bin 没有唤醒词配置。每次固件更新后，都需要重新刷入自定义的 assets_final.bin。如果将来能在构建系统中集成 pack_assets_direct.py，就能省掉第二步。

**pack_assets_direct.py 的存在意义。**

它是两套管线之间的桥梁: 从 xiaozhi-assets-generator 的产出中提取唤醒词配置和语音模型，从 esp_emote_gfx 的产出中获取 EAF 动画文件，统一打包成固件能识别的格式。没有它，每次更新资源都需要手动操作多个步骤。

---

## 附录: 关键文件路径速查

| 文件 | 路径 | 说明 |
|------|------|------|
| 打包脚本 | `scripts/spiffs_assets/pack_assets_direct.py` | 统一资源打包 |
| 表情配置 | `main/boards/echoear/emote.json` | 21 个表情定义 |
| EAF 动画 | `managed_components/espressif2022__esp_emote_gfx/emoji_large/` | 动画帧数据 |
| 表情渲染 | `emote_display.cc` | EmoteDisplay 实现，AddEmojiData() |
| 资源解析 | `assets.cc` (L275-313) | emoji_collection 解析，检查 eaf 键 |
| 唤醒词 | `CustomWakeWord` 类 | ParseWakenetModelConfig() 从 index.json 读配置 |
| 分区偏移 | `0x800000` | assets 分区，8MB |
