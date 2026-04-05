# 小智 AI 唤醒词配置指南

## 概述

小智 AI 支持两种唤醒词模式：

| 模式 | 芯片 | 模型 | 说明 |
|------|------|------|------|
| WakeNet（预设） | C3/C6 → WakeNet9s<br>S3/P4 → WakeNet9 | 内置模型 | 快速、低功耗，但不能自定义 |
| MultiNet（自定义） | 仅 ESP32-S3 | mn6_cn / mn7_cn (中文)<br>mn6_en / mn7_en (英文) | 可自定义中英文命令词 |

## 当前项目的唤醒词配置

### 配置文件

唤醒词相关的关键文件：

- `main/protocols/config.h` - 服务器地址和端口配置
- `main/boards/echoear/` - 板级配置目录
- `managed_components/` - 唤醒词模型文件

### 预设唤醒词列表

#### WakeNet9 (ESP32-S3)

| 唤醒词 | 模型名称 | 说明 |
|--------|---------|------|
| Hi,乐鑫 | wn9_hilexin | |
| Hi,ESP | wn9_hiesp | |
| 你好小智 | wn9_nihaoxiaozhi_tts | |
| Hi,Jason | wn9_hijason_tts2 | |
| 小爱同学 | wn9_xiaoaitongxue | |
| 嗨小欧 | wn9_hai1xiao3ou1_tts3 | |
| 你好小瑞 | wn9_ni3hao3xiao3rui4_tts3 | |

#### WakeNet9s (ESP32-C3/C6)

| 唤醒词 | 模型名称 | 说明 |
|--------|---------|------|
| Hi,乐鑫 | wn9s_hilexin | |
| Hi,ESP | wn9s_hiesp | |
| 你好小智 | wn9s_nihaoxiaozhi | |
| Hi,Jason | wn9s_hijason_tts2 | |

## 自定义唤醒词（MultiNet）

### 使用 xiaozhi-assets-generator 在线生成

访问 https://github.com/78/xiaozhi-assets-generator

1. 选择芯片型号：**ESP32-S3**
2. Tab 1 选择"自定义唤醒词"
3. 输入命令词（拼音格式）
4. 设置参数：
   - **Threshold**（阈值）：0-100，默认 20
   - **Duration**（超时）：单位 ms
5. 点击生成 assets.bin

### 中文命令词格式

使用拼音输入，词之间用空格分隔：

| 目标词 | 拼音格式 |
|--------|---------|
| 你好零一 | ni hao ling yi |
| 小智小智 | xiao zhi xiao zhi |
| 嗨小欧 | hai xiao ou |

### MultiNet 模型版本

| 模型 | 语言 | 说明 |
|------|------|------|
| mn6_cn | 中文 | 推荐，中文命令词 |
| mn7_cn | 中文 | 最新版，中文命令词 |
| mn6_en | 英文 | 英文单词 |
| mn7_en | 英文 | 最新版，英文单词 |

## 项目中的唤醒词模型文件

唤醒词模型文件通常位于：
- `share/wakenet_model/` - WakeNet 模型
- `share/multinet_model/` - MultiNet 模型

生成后的 `srmodels.bin` 包含唤醒词模型配置。

## index.json 中的唤醒词配置

```json
{
    "srmodels": "srmodels.bin",
    "multinet": {
        "model": "mn6_cn",
        "command": "ni hao xiao zhi",
        "threshold": 20,
        "duration": 3000
    }
}
```

## 更换唤醒词的步骤

### 方案一：使用在线工具重新生成 assets.bin

1. 访问 https://github.com/78/xiaozhi-assets-generator
2. 配置芯片型号、屏幕分辨率
3. Tab 1 选择自定义唤醒词，输入拼音
4. 其他配置保持或按需调整
5. 生成并下载 assets.bin
6. 使用 esptool 烧录到 0x800000 分区

```bash
esptool.py --chip esp32s3 -b 460800 write_flash 0x800000 assets.bin
```

### 方案二：修改源码中的唤醒词路径

如果只想更换唤醒词模型文件：

1. 获取新的 `srmodels.bin`
2. 替换 `main/boards/echoear/` 下的对应文件
3. 重新构建

## 唤醒词相关日志

设备启动时可以看到唤醒词加载日志：

```
I (xxx) MCP: WakeNet version: xxx
I (xxx) MCP: WakeNet model: wn9_nihaoxiaozhi_tts
```

## 注意事项

1. **仅 ESP32-S3 支持 MultiNet 自定义唤醒词**
2. **自定义唤醒词需要使用拼音格式输入**
3. **Threshold 越低越灵敏，但可能误触发**
4. **Duration 设置过长会增加功耗**

## "你好零一" 唤醒词

- **拼音**：ni hao ling yi
- **模型**：mn6_cn 或 mn7_cn（中文 MultiNet）
- **芯片**：仅 ESP32-S3 支持
- **建议参数**：threshold=20, duration=3000

可以通过 xiaozhi-assets-generator 在线生成包含此唤醒词的 assets.bin。
