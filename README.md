# llawsxxDSP

`llawsxxDSP` 是一个 Windows x64 VST2 音频效果插件，面向 OBS 和其他支持
VST2 的宿主。处理链顺序为：

```text
四段均衡器 -> 卷积混响 -> 响度标准化 -> 前瞻限制器
```

四个 DSP 模块默认全部关闭，可以从插件界面顶部的一行总开关分别启用。

## 功能

- 四段参数均衡器：中心频率、增益、Q 值。
- 卷积混响：Room、Decay、Damping、Mix。
- BS.1770 风格响度标准化：目标 LUFS、目标 LRA、True Peak 上限。
- 立体声联动前瞻限制器：输入增益、阈值、释放、Ceiling、Lookahead、
  Adaptive Release。
- 输入和输出实时 dB 电平条。
- 输入和输出 1.5 秒峰值保持及峰值 dB 数字。
- 输入和输出波形，可显示最近 1024、2048、4096、8192 或 16384 个采样点。
- EQ、Reverb、Loudness、Limiter 参数分别放在独立 Tab 中。
- 参数支持宿主自动化、滑块调整和手动输入。
- 编辑界面会随 Windows 系统缩放和显示器 DPI 等比例缩放。

## 操作界面

点击参数右侧的深色数值框即可手动输入实际数值。输入时不要求填写单位：

- 频率输入 Hz，例如 `1000`。
- 增益、阈值、Ceiling 和响度目标输入 dB，例如 `-14`。
- Room、Damping、Mix 输入百分比，例如 `75`。
- Decay 输入秒，例如 `1.5`。
- Release 和 Lookahead 输入毫秒，例如 `100` 或 `5`。
- Q 和 LRA 直接输入数值。

按 Enter 提交，按 Esc 取消；输入框失去焦点时也会提交。超出合法范围的值会
自动限制到该参数的最小值或最大值。

## 源码结构

```text
simpledsp_vst2.cpp       VST2 接口、参数映射和 DSP 处理链
simpledsp_editor.cpp     Win32 插件编辑器
simpledsp_editor.h       编辑器与 DSP 的数据接口
convolution_reverb.c     卷积混响
convolution_reverb.h
tests/vst2_smoke.cpp     DLL、音频处理、窗口和交互测试
third_party/VST_SDK_2.4  VST2 SDK
```

## 构建

需要 Visual Studio 2022、Desktop development with C++ 工作负载和 CMake。

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
ctest --test-dir build-vs -C Release --output-on-failure
```

生成文件：

```text
build-vs/Release/llawsxxDSP.dll
```

SDK 默认从 `third_party/VST_SDK_2.4` 或 `third_party/vst2sdk` 查找，也可以指定：

```powershell
cmake -S . -B build-vs -DVST2_SDK="D:\path\to\VST_SDK_2.4"
```

## OBS 使用

将 `llawsxxDSP.dll` 复制到 OBS 可以访问的 VST2 目录，重新启动 OBS，然后在
音频源的“滤镜”中添加 VST 2.x 插件。

OBS 会把 29 个标准化参数保存到当前场景集合的 `chunk_data` 中。Windows 下的
场景集合通常位于：

```text
%APPDATA%\obs-studio\basic\scenes\
```

替换 DLL 后应重新启动 OBS，因为正在运行的进程会继续使用已经加载到内存中的
旧 DLL。

## 测试

`vst2_smoke` 会验证：

- VST2 入口和插件信息。
- 双声道音频处理输出。
- 默认关闭时的逐样本透明直通。
- 均衡器目标频率增益、响度提升、限制器上限和卷积混响尾音。
- 四个 DSP 默认关闭。
- 编辑器创建、关闭及重新打开。
- 模块开关、Tab、滑块和手动输入框。

## 第三方代码

VST2 SDK 保存在 `third_party/VST_SDK_2.4` 并作为普通仓库文件跟踪。使用、修改
或分发 SDK 与插件时，请遵守该 SDK 目录内附带的许可条款。
