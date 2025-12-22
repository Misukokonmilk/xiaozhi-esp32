@echo off
setlocal enabledelayedexpansion

:: 1. Paths
if "%IDF_PATH%"=="" set IDF_PATH=C:\Users\administered\esp\v5.5\esp-idf
if "%IDF_TOOLS_PATH%"=="" set IDF_TOOLS_PATH=E:\Code\ESP32\SDK\.espressif
if not exist "%IDF_TOOLS_PATH%" set IDF_TOOLS_PATH=%USERPROFILE%\.espressif

:: 2. Find Python
set PYEXE=
for /d %%D in ("%USERPROFILE%\.espressif\python_env\*") do (
  if exist "%%D\Scripts\python.exe" (
    set PYEXE=%%D\Scripts\python.exe
    goto :has_py
  )
)
for /d %%D in ("%IDF_TOOLS_PATH%\python_env\*") do (
  if exist "%%D\Scripts\python.exe" (
    set PYEXE=%%D\Scripts\python.exe
    goto :has_py
  )
)
if "%PYEXE%"=="" set PYEXE=python

:has_py
if not "%1"=="" set TARGET=%1
if "%TARGET%"=="" set TARGET=esp32s3
if not "%2"=="" set VERSION=%2

:: Change directory to project root
pushd %~dp0\..

:: 3. Check build files
if not exist "build\bootloader\bootloader.bin" goto :missing_files
if not exist "build\partition_table\partition-table.bin" goto :missing_files
if not exist "build\ota_data_initial.bin" goto :missing_files
if not exist "build\xiaozhi.bin" goto :missing_files

set ASSET_BIN=main\assets\puhui\wn9_nihaoxiaozhi_tts-font_puhui_common_20_4-echoear.bin

echo Python: %PYEXE%
echo Merging for %TARGET%...

:: 4. Merge Command (Offsets matching your IDE)
"%PYEXE%" -m esptool --chip %TARGET% merge_bin -o build\merged.bin --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xD000 build\ota_data_initial.bin 0x20000 build\xiaozhi.bin 0x800000 %ASSET_BIN%

if errorlevel 1 (
    echo [ERROR] Merge failed.
    popd
    pause
    exit /b 1
)

:: 5. Rename output
if "%VERSION%"=="" set VERSION=unknown
set OUTFULL=build\xiaozhi-esp32_full_v%VERSION%_%TARGET%.bin
copy /y "build\merged.bin" "%OUTFULL%" >nul

echo ======================================================
echo  SUCCESS: %OUTFULL%
echo ======================================================
popd
endlocal
pause
exit /b 0

:missing_files
echo [ERROR] Bin files not found in build folder.
popd
pause
exit /b 1