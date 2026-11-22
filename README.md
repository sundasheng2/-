# STM32 串口调试助手

一个 Windows 桌面工具，用于调试 STM32 等设备的串口通信，也可在同一窗口内打开 SSH 会话。程序使用 C、CUI 和 Win32 API。

## 快速开始

下载仓库根目录的 [stm32_serial_console_settings.exe](./stm32_serial_console_settings.exe)，双击运行。

### 串口

1. 点击标签栏的 `+` 新建串口会话，选择 COM 口、波特率等参数。点击标签上的圆圈连接或断开。
2. 在底部输入命令。`Enter` 发送，`Shift+Enter` 在输入框内换行。支持文本和十六进制发送，可选择发送时附加的行尾，也可以保存常用命令。
3. 「日志工具」提供搜索、筛选、滚动、复制和清空功能。接收区支持文本和十六进制显示，文本支持自动识别、UTF-8、GBK 和 ANSI 颜色。可以拖选日志，复制时只取接收正文。

### 日志记录

点击右上角齿轮，进入「日志记录设置」选择默认文件夹。回到串口会话，点击「记录接收数据」，程序会按 COM 口和本机日期保存日志。例如，COM3 的日志写入 `默认文件夹\COM3\2026-11-22.txt`。过午夜后自动切换到下一天的 TXT 文件。

记录按会话启停，切换标签后原会话继续记录。未设置默认文件夹时，启动记录会弹出保存对话框，可选文本、十六进制或原始二进制格式。

### SSH

点击 `+` 新建 SSH 会话，填入主机名或 IP、远程用户名和端口（默认 `22`），然后点击「连接」。登录提示和远程终端的输入输出都在当前标签内，按提示直接输入即可。此功能需要 Windows OpenSSH 客户端和支持 ConPTY 的 Windows。

右上角齿轮的「主题」菜单提供「经典深色」「明亮」和「米白橄榄」三种配色。

## 从源码构建

在 Windows 上准备 CMake 3.20 或更新版本、Ninja 和 MinGW-w64 x64，将这些工具加入 `PATH`。在仓库根目录打开 PowerShell，运行以下命令。构建目录位于仓库外的临时目录下。

```powershell
$buildDir = Join-Path $env:TEMP 'stm32-serial-console-build'
cmake -S . -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build $buildDir
& (Join-Path $buildDir 'stm32_serial_console.exe')
```

## 工程结构

| 路径 | 用途 |
| --- | --- |
| `CMakeLists.txt` | 构建入口 |
| `src/` | 主界面、串口通信、SSH、日志归档和资源文件 |
| `third_party/CUI/` | 随仓库提供的 CUI 源码 |
| `stm32_serial_console_settings.exe` | 可直接运行的 Windows 程序 |
