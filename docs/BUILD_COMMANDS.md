# ESP-IDF 构建与刷写命令备忘

## 环境要求

- ESP-IDF SDK: `E:\Code\ESP32\SDK\v5.4.2\esp-idf`
- Python 虚拟环境: `E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env`
- ESP-IDF 工具链: `E:\Code\ESP32\SDK\.espressif`
- 项目路径: `E:\Code\ESP32\SF\xiaozhi-esp32`
- 项目分支: `feature/echoear-slim`
- 目标芯片: ESP32-S3 (16MB Flash)

## Python 依赖 (首次运行需安装)

在 ESP-IDF Python 环境中安装必要的包：

```powershell
# 安装到 ESP-IDF venv (idf5.4_py3.11_env)
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m pip install numpy Pillow
```

## 构建命令

### 1. 激活 ESP-IDF 环境

**关键**: 必须先获取激活脚本路径，然后执行它。不能直接调用 `idf.py`。

```powershell
# 获取激活脚本 (PowerShell)
$activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1

# 设置环境变量
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"

# 执行激活 (注意 "." 或 "& " 后面要有空格)
. $activateScript
# 或者
& $activateScript
```

**或者一行命令版本**:
```powershell
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"; $env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"; $activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1; & $activateScript
```

### 2. 设置表情资源路径 (构建带 EAF 动画资源的固件)

```powershell
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"
```

**说明**: `BOARD_RES_PATH` 必须指向包含 `.eaf` 动画文件和 `.bin` 图标文件的 `emoji_large` 目录。设置后构建系统会自动打包 EAF 动画到 SPIFFS assets 分区。不设置则只会打包默认的 PNG 资源，导致表情动画不显示。

### 3. 完整构建命令

```powershell
# 1. 设置环境变量
$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"

# 2. 激活 ESP-IDF
$activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1
& $activateScript

# 3. 清理并构建 (首次或依赖变更时 clean)
idf.py clean
idf.py build

# 4. 仅构建 (增量编译)
idf.py build
```

### 4. 刷写到设备

确保设备连接到 COM3 (或对应端口):

```powershell
idf.py -p COM3 flash
```

### 5. 监控串口输出

```powershell
idf.py -p COM3 monitor
```

退出 Monitor: `Ctrl+]`

### 6. 刷写 + 监控 (连续命令)

```powershell
idf.py -p COM3 flash monitor
```

## 常见问题

### Q: `idf.py: command not found` 或 `idf.py` 弹窗问"如何打开此文件"
**A**: 没有在激活的 ESP-IDF 环境中运行。必须先执行 `activate.ps1` 脚本。

### Q: `ModuleNotFoundError: No module named 'numpy'`
**A**: ESP-IDF Python 虚拟环境缺少依赖。运行:
```powershell
& "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" -m pip install numpy Pillow
```

### Q: `BOARD_RES_PATH` 应该设置为什么值？
**A**: 指向包含 EAF 动画文件的目录:
```
E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large
```

这个目录下应该有: `Happy.eaf`, `Sad.eaf`, `neutral.eaf`, `icon_mic.bin`, `battery_level*.bin` 等文件。

### Q: 表情动画不显示，提示 "No emoji data found"
**A**: `BOARD_RES_PATH` 未设置或设置错误。确认环境变量值指向包含 `.eaf` 文件的目录，然后重新 `idf.py clean && idf.py build`。

### Q: 表情动画不显示，提示 "No icon data found for icon_Battery"
**A**: 图标文件命名不匹配。`icon_Battery` 是 EmoteDisplay 代码中使用的名字，但 `emoji_large/` 目录下的文件叫 `battery_level*.bin`。这是已知的微小问题，不影响核心功能。

## 完整的一次性构建脚本

```powershell
# E:\Code\ESP32\SF\xiaozhi-esp32\build_and_flash.ps1

$env:IDF_TOOLS_PATH = "E:\Code\ESP32\SDK\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\Code\ESP32\SDK\.espressif\python_env\idf5.4_py3.11_env"
$env:BOARD_RES_PATH = "E:\Code\ESP32\SF\xiaozhi-esp32\managed_components\espressif2022__esp_emote_gfx\emoji_large"

$activateScript = python "E:\Code\ESP32\SDK\v5.4.2\esp-idf\tools\activate.py" --export 2>&1 | Where-Object { $_ -match 'activate.*\.ps1$' } | Select-Object -Last 1
& $activateScript

Set-Location "E:\Code\ESP32\SF\xiaozhi-esp32"

idf.py clean
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor
```
