# Lumen Fusion：增量改进与来源

本文固定核对 Lumina 基线 [`b5dcbd8731ceeeed3878759703d157cd8c1a57d2`](https://github.com/jayl-dev/Lumina/commit/b5dcbd8731ceeeed3878759703d157cd8c1a57d2) 至 Lumen Fusion [`e9e527b0c9186c2eb2faa4b18b4653de33bacf0f`](https://github.com/ovlerfork/Lumen-Fusion/commit/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f) 的实际 Git 历史与差异。下文“已合入”仅指这个截止点，不涵盖后续开发，也不等于已在所有机器上验证。

本分支的特色在于把已有 macOS 串流能力整合成可安装的应用，并修正输入状态、编码输出时机、音频帧布局和权限生命周期中的具体问题。收益主要是减少错误等待与状态错配、改善输入细节和交付完整性；目前没有足够证据给出通用端到端延迟降幅。

## 1. 来源边界

| 能力 | 来源与归属 | 本分支的增量 |
| --- | --- | --- |
| ScreenCaptureKit 视频及原生系统音频 | Lumen/Lumina 已有；可追溯至 [Lumen macOS 集成](https://github.com/ovlerfork/Lumen-Fusion/commit/b322a7636a288d94c88b2758d1ea358225dcd537)，且基线已有对应实现 | 队列深度适配、立体声帧布局修正、权限重查 |
| macOS 零拷贝编码路径 | 继承 [Sunshine 零拷贝实现](https://github.com/LizardByte/Sunshine/commit/c56ad91693fceebbbd7aeb3a17d064e4e921d5ac) | 在既有路径上补齐 VT 输出完成时机 |
| HID 虚拟手柄、键鼠模拟、`gamepad.m` | 基线已存在；[Lumina 整合提交](https://github.com/jayl-dev/Lumina/commit/96abcdcfb82ad543a6fd73e94e7dcaa51e6dd42c) 明确保留相关能力 | 修正槽位契约、初始化失败处理及断连释放 |
| 虚拟显示器 | 已由 [Lumina 基线之前的提交](https://github.com/jayl-dev/Lumina/commit/575706f67a00da4ecb718ab665482553d3be5df1) 集成 | 本范围不将其列作新增功能 |
| PTS 映射及主机处理延迟统计 | Lewis Wood 的 [`d216c6c93dbc70a80d921feb4dc6ab0000af1403`](https://github.com/ovlerfork/Lumen-Fusion/commit/d216c6c93dbc70a80d921feb4dc6ab0000af1403)，已进入 Lumina 历史 | 沿用按 PTS 匹配提交与输出的测量基础，不认领为本分支发明 |
| `vt_max_frame_delay` / `MaxFrameDelayCount` | [Lumina 整合提交](https://github.com/jayl-dev/Lumina/commit/96abcdcfb82ad543a6fd73e94e7dcaa51e6dd42c) 已保留 | 新增的是原生完成调用，不是新增这个选项 |
| AVE 编码器选择 | Robert.Leake 的 [`8a8c807e2cec27b8f0f7decface695f83dba1fab`](https://github.com/cpt-robski/Lumen/commit/8a8c807e2cec27b8f0f7decface695f83dba1fab) | 本范围不认领 AVE 选择能力，也未合入新的 HEVC 自动选择策略 |

上述链接中的 Lumen Fusion 镜像用于固定历史对象位置，不改变原作者和上游归属。

## 2. 适配的供体改动

### CPT：ScreenCaptureKit 队列 5 → 4

供体为 cpt-robski/Lumen 的 [`54ca6606b89276fbe5e68c1dac6fa2e7f979f5ba`](https://github.com/cpt-robski/Lumen/commit/54ca6606b89276fbe5e68c1dac6fa2e7f979f5ba)，本分支落地于 [`6d173f95c43d388c70fb18597f5c2bc368158609`](https://github.com/ovlerfork/Lumen-Fusion/commit/6d173f95c43d388c70fb18597f5c2bc368158609)，修改 `src/platform/macos/sc_capture.m`。

`queueDepth` 从 5 改为 4，减少捕获侧允许缓冲的帧数。它约束积压空间，但不保证每次串流都减少一帧延迟；较浅队列也减少了承受消费端暂时停顿的余量。该提交没有新增专门的性能回归测试。

### 输入批处理的溢出判断修正：Sax 供体归属待核实

项目提供的信息将这项修正关联到 Sax，但本地 `saxlamen/main` 仍保留反向判断，现有历史也未找到对应供体修复。因而目前只能确认[本分支落地提交 `6d173f95`](https://github.com/ovlerfork/Lumen-Fusion/commit/6d173f95c43d388c70fb18597f5c2bc368158609)，供体归属待核实。原批处理逻辑来自 [Sunshine `62606a62f98295399577734adaa7e3b3aa99beb7`](https://github.com/LizardByte/Sunshine/commit/62606a62f98295399577734adaa7e3b3aa99beb7)；不能仅凭本地落地记录认定独立原创。

`src/input.cpp` 中相对鼠标、垂直滚动和水平滚动合并使用 `__builtin_add_overflow`。它返回真表示溢出，修正后仅在溢出时停止批处理，正常可表示的增量继续合并。原先反向判断会打断正常合并，并错误处理溢出情形。这是输入正确性修复，不能直接折算成 FPS 提升。

高分辨率滚动也有 [CPT 供体历史 `d50e10bc0326c05256247b6435a0215ed49ab73a`](https://github.com/cpt-robski/Lumen/commit/d50e10bc0326c05256247b6435a0215ed49ab73a)。本分支在已有像素转换之上增加余数保留，贡献边界见下一节。

## 3. 本分支的整合与正确性改进

### 输入槽位、滚动小数和断连清理

提交：[`6d173f95c43d388c70fb18597f5c2bc368158609`](https://github.com/ovlerfork/Lumen-Fusion/commit/6d173f95c43d388c70fb18597f5c2bc368158609)。文件：`src/platform/macos/input.mm`、`src/platform/macos/gamepad.m`。

- **槽位契约**：直接以 `id.globalIndex` 分配平台槽位，检查越界和重复分配；分配成功返回状态码 `0`，避免把槽位号混作成功状态。这样后续按全局编号更新和释放时能命中同一设备。
- **失败处理**：保留已有的 HID 优先、失败后键鼠模拟路径，增加模拟对象初始化失败检查。`CGEventSourceCreate` 失败时不再将模拟手柄标成已连接。
- **滚动余数**：将尚不足一个像素的缩放结果分别存入横、纵轴余数，下次输入继续累加，单位为 `1/WHEEL_DELTA`。细小滚动不会因每次整数除法而反复丢失；两轴互不借用余数。这不生成惯性，也不预测后续输入。
- **断连释放**：销毁时执行断连；在禁用状态更新之前提交零状态，释放模拟按键和扳机，减少断开后按键仍被按住的问题。

该提交未新增专用自动化测试，不能以代码存在代替多手柄、重连及实际触控板体验验证。

### 原生 VideoToolbox 完成调用

提交：[`d448950c651f89221a1b0ceb11af77fca9929cbe`](https://github.com/ovlerfork/Lumen-Fusion/commit/d448950c651f89221a1b0ceb11af77fca9929cbe)。文件：`src/platform/macos/vt_output_completion.{h,cpp}`、`src/video.cpp`。

VideoToolbox 异步接收图像，FFmpeg 随后轮询编码包时，当前提交的输出可能尚未就绪。本分支让 macOS 视频提交经过 `platf::vt::send_frame`，在线程局部作用域内拦截 `VTCompressionSessionEncodeFrame`，通过 `dlsym(RTLD_NEXT, ...)` 调用原函数。真实提交成功后，以该帧的数值 PTS 调用 `VTCompressionSessionCompleteFrames`，使原生完成发生在 FFmpeg 随后的取包之前。

这里使用指定 PTS 完成已提交帧，不把每帧提交变成 EOS 排空。作用域只为非空的 `_videotoolbox` 帧开启。若原生提交成功而完成调用失败，拦截函数仍保留原提交结果，由外层包装报告完成错误，避免 FFmpeg 再次释放已经交给 VT 的引用。

取舍是驱动内的同步等待：代码没有可取消超时，慢驱动可能阻塞提交线程。这减少异步输出等待的机会，同时改变流水线重叠程度，不能据此承诺所有编码器和负载都提升吞吐。新增硬件加速状态及 `EncoderID` 记录用于识别实际编码器，不等于新增 AVE 选择算法。

[原生集成测试](https://github.com/ovlerfork/Lumen-Fusion/blob/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f/tests/integration/test_vt_output_completion.cpp) 使用本机链接的 FFmpeg，对 H.264/HEVC 分别比较包装提交与原始 `avcodec_send_frame`。每组是 **30 帧合成图像，按 60 FPS 节奏提交**，覆盖逐帧输出可用性、PTS 对应、请求关键帧、EOS、重复 EOS 与未打开上下文错误。测试需要 macOS 硬件编码器；它没有覆盖真实游戏、网络或客户端显示。

### 立体声帧保持完整，再映射到协商声道

提交：[`9467e11e4549bc087134b858649dbb8e66a0660a`](https://github.com/ovlerfork/Lumen-Fusion/commit/9467e11e4549bc087134b858649dbb8e66a0660a)。文件：`src/stereo_pcm.h`、`src/platform/macos/microphone.mm`、`src/audio.cpp`。

SCK 路径明确请求双声道。读取时按“时间帧数 × 2”消费交错立体声数据，再映射到协商的 2、6 或 8 声道布局：前左、前右保留原样，其余声道补零。这样不会把相邻时刻的立体声样本误当成同一时刻的环绕声道，也不会按错误声道数消耗音频时间。

例如 48 kHz 下 5 ms 包含 240 个时间帧，即 480 个立体声样本；映射为六声道后是 1440 个样本，时间仍为 5 ms。代价是多声道缓冲和编码布局的成本；内容仍是立体声，不产生原生 5.1/7.1 空间信息。

[单元测试](https://github.com/ovlerfork/Lumen-Fusion/blob/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f/tests/unit/test_stereo_pcm.cpp) 覆盖立体声保持、5 ms 帧数、5.1/7.1 补零、非法形状、空块以及协商布局的 Opus 编解码往返。它验证帧布局和编码兼容性，不代替真实 SCK 采集试听。

### 权限在启动及编码器探测边界重新读取

提交：[`e9e527b0c9186c2eb2faa4b18b4653de33bacf0f`](https://github.com/ovlerfork/Lumen-Fusion/commit/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f)。文件：`src/platform/macos/misc.mm`、`src/video.cpp`。

移除进程内缓存的屏幕采集授权布尔值。启动时先检查，缺失则请求，然后再次读取实际授权；请求函数返回成功也不直接认定当前进程已获准。编码器探测入口同样重查，包括已有编码器结果、原本会跳过后续探测的情况，避免旧状态掩盖授权或撤销。

读取发生在启动和探测边界，不在逐帧热路径中。这也不意味着运行中的会话得到连续权限监控。系统要求重启应用时，仍需完整退出再打开。

[权限测试](https://github.com/ovlerfork/Lumen-Fusion/blob/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f/tests/unit/platform/test_macos_permissions.cpp) 覆盖授权变化、不重复请求已有授权、请求期间授权、请求成功但实际未授权，以及拒绝后再次读取。模拟测试无法证明真实 TCC 对跨版本应用身份的处理。

## 4. 原生应用与打包交付

以下都是本范围的整合工作，主要改善安装、启动和可搬移性，不应记作编码性能提升。

| 提交 | 作用与主要文件 |
| --- | --- |
| [`b873c939e323fc2c64e04262e030b6899c556f63`](https://github.com/ovlerfork/Lumen-Fusion/commit/b873c939e323fc2c64e04262e030b6899c556f63) | macOS 便携产物流水线；`.github/workflows/macos-artifact.yml`、`scripts/macos_build.sh`、安装/启动器脚本及打包配置 |
| [`d378dc2e97267f8c5d19dc2248c815a3e05dd394`](https://github.com/ovlerfork/Lumen-Fusion/commit/d378dc2e97267f8c5d19dc2248c815a3e05dd394) / [`fc129c306587ccebaac2a66c9493dbd0bb4daebe`](https://github.com/ovlerfork/Lumen-Fusion/commit/fc129c306587ccebaac2a66c9493dbd0bb4daebe) | 补齐 OpenSSL 头文件传递与 `third-party/nanors/rs.h` 自包含依赖，解决构建集成问题 |
| [`611495aad828eb3a7070bbdb105ca01ec02dbf06`](https://github.com/ovlerfork/Lumen-Fusion/commit/611495aad828eb3a7070bbdb105ca01ec02dbf06) | `src/platform/macos/av_audio.mm` 为 Core Audio Tap 创建、销毁加 macOS 14.2 可用性边界；不属于新增系统音频采集 |
| [`26d65a187156f38fc2dc4d40efcc714ec13dac91`](https://github.com/ovlerfork/Lumen-Fusion/commit/26d65a187156f38fc2dc4d40efcc714ec13dac91) | 将适用的 Lumina 测试接入 macOS CI；测试基础设施及原测试来源仍属继承 |
| [`3e7a894888de6cc0a6be5832014a50f1722a2558`](https://github.com/ovlerfork/Lumen-Fusion/commit/3e7a894888de6cc0a6be5832014a50f1722a2558) / [`3ea59dbb91c8d4686605ddc7bb799a27446b8e51`](https://github.com/ovlerfork/Lumen-Fusion/commit/3ea59dbb91c8d4686605ddc7bb799a27446b8e51) | 搬移后产物、安装启动器验证及 DMG 交付；`scripts/validate-macos-package.sh`、`cmake/packaging/macos.cmake` |
| [`37d95d94c4ec9a35064c3a906e3f7fc56aa8bc90`](https://github.com/ovlerfork/Lumen-Fusion/commit/37d95d94c4ec9a35064c3a906e3f7fc56aa8bc90) | 原生 `Lumen Fusion.app`、包内相对资源路径、辅助程序及 Qt 插件，搬移和裁剪后签名；`src_assets/macos/Info.plist.in` 及打包、资源查找代码 |
| [`2619ef9fcb0d1a807e4dff13d85dd88f28380631`](https://github.com/ovlerfork/Lumen-Fusion/commit/2619ef9fcb0d1a807e4dff13d85dd88f28380631) | `scripts/validate-macos-startup.py` 检查直接启动及 LaunchServices 启动、HTTPS 与静态资源、托盘、PID/监听归属和 SIGTERM 退出 |
| [`f20fc3f6a7b274337691597f30aad66f5c96506f`](https://github.com/ovlerfork/Lumen-Fusion/commit/f20fc3f6a7b274337691597f30aad66f5c96506f) | `src/system_tray.cpp` 在静态分配完成后初始化 macOS 托盘图标路径 |
| [`909e12b8f9f986732f0778f8eca3086d487d9f4d`](https://github.com/ovlerfork/Lumen-Fusion/commit/909e12b8f9f986732f0778f8eca3086d487d9f4d) / [`e0e024a91c21c09e2afb3a7cc08ecef961584aea`](https://github.com/ovlerfork/Lumen-Fusion/commit/e0e024a91c21c09e2afb3a7cc08ecef961584aea) | 统一应用主可执行文件布局，并在签名前保留规范 Qt framework 结构 |
| [`0da6604b04d8defd73858c6336315fc1f7466f75`](https://github.com/ovlerfork/Lumen-Fusion/commit/0da6604b04d8defd73858c6336315fc1f7466f75) | 声明应用最低 macOS 15.0，并核对捆绑二进制最低版本；源码局部兼容旧 API 不代表打包产物支持旧系统 |

**ad-hoc 身份限制**：当前包采用 ad-hoc 签名。应用更新后的身份可能无法复用系统设置中旧构建的授权条目，即使界面显示已开启，也不能保证当前进程可采集。权限重查让失败原因可见，但不提供 Developer ID、公证或跨构建授权延续保证。处理采集权限不应重置配对信息或应用配置。

这里的测试覆盖描述的是截止点已有的验证机制；实际通过状态需要对应构建和运行记录支持。

## 5. 性能证据：能说明什么

| 证据 | 可支持的判断 | 限制 |
| --- | --- | --- |
| 原生 30 帧合成 A/B | 测试代码对比有无 VT completion 时的包可用性，按 PTS 记录提交到取包时间及积压 | 30 帧不等于 30 FPS；本测试以 60 FPS 节奏运行。缺少可引用的已提交测量结果，尚不能给出经核实的降幅 |
| 此前用户 30 FPS 日志 | 属于实际串流场景，与合成编码实验是不同层次的证据 | 缺少可公开引用的摘要来核对样本、编码器和统计口径，因此不据此给出数值结论，也不外推到 60 FPS |
| 新的用户 60 FPS 反馈，当前 pacing jitter 约 6% | 记录为用户报告，说明仍存在需要观察的节奏波动 | 尚无新的 60 FPS 日志或可复核证明；窗口、指标定义及环境未在此核实，不能转写成丢帧率或已测得的延迟改善 |
| 仓库内更早的性能基线 | 已公开记录请求 60 FPS 的 1920×1080 HEVC 样本：新源帧 40.67 FPS，编码平均 57.85 ms、p95 82.88 ms；捕获队列、转换、广播队列、FEC 平均各低于 0.1 ms | 这是旧样本，不能当作本次用户 30 FPS 日志或新的 60 FPS 验证；原文也指出它不代表高运动游戏负载 |

旧样本及计时定义见[截止点的性能诊断文档](https://github.com/ovlerfork/Lumen-Fusion/blob/e9e527b0c9186c2eb2faa4b18b4653de33bacf0f/docs/streaming-performance-logging.md)。其中 `encode_ms` 按 PTS 匹配提交与编码包就绪，`pacing_ms` 是帧内主动限速等待；它们都不能自动等同于客户端显示延迟或用户报告的 pacing jitter。既有测量方法的来源仍归上游。

因此，目前可以描述具体修复机制与测试契约；端到端体验、持续 60 FPS 表现以及约 6% 抖动的成因，仍需要同条件下的实机证据。

## 6. 截止点内已合入与尚未合入

已合入：CPT 队列深度适配、输入溢出判断修正、输入槽位与滚动余数修正、断连清理、应用打包及验证机制、原生 VT completion、立体声帧布局修复、启动及探测权限重查。

以下方案**未合入 `e9e527b0`**，不能列为当前版本优化效果：

- 固定时钟调度方案。
- FFmpeg 最多两个在途帧的限制。
- 新的 HEVC 自动编码器选择策略。
- 主机端滚动惯性。

后续运行时结果应另行注明提交截止点、测试条件和证据来源，不反向并入本基线的性能结论。

## 7. 当前工作区候选改动：待独立验证

本节记录写作时工作区中尚未进入上述 `e9e527b0` 截止点的候选改动。它们没有发布版本、CI 通过次数或端到端性能结论；后续合入时应以实际提交和运行记录更新本节。除已有提交历史明确标注的来源外，以下候选改动的作者归属尚未核实，不在本文认领原创。

| 候选改动 | 目标 | 现有证据与边界 |
| --- | --- | --- |
| 捕获时间到 RTP 时间戳映射 | 对每个发送帧使用捕获时间生成 90 kHz RTP 时间戳；空闲重复帧用当前发送起点。内部未截断 tick 保持递增，序列化为 32 位 RTP 值时按协议正常回绕 | 单元测试可检查内部递增、缺失捕获时间和 32 位回绕边界。它不证明客户端显示节奏、网络延迟或 60 FPS 体验。 |
| macOS NV12 像素缓冲与图像池 | 原生 NV12 直接交给 VideoToolbox 时避免不必要的 CPU 基址锁定；替换帧缓冲前先取得新缓冲所有权；同步图像池的回收和重建 | 代码审查可检查引用释放与并发访问路径。仍需 macOS 真机下的 VideoToolbox、显示器变更和停止重启测试来确认没有生命周期回归。 |
| 可选流式分析器 | 将有限样本窗口移入会话，使用最多 512 个水塘样本给出 p50、p95、p99，并记录捕获、编码输出、发送完成的帧间隔及超帧预算次数 | 指标可帮助区分发送端相邻帧间隔与客户端统计；水塘抽样在窗口超过容量后是估计值。`encode_over_budget` 仅比较编码时间和目标帧间隔，不能解释或证明约 6% 的 pacing jitter 已解决。 |

这些候选改动的共同目的，是让时间戳、缓冲所有权和诊断口径更可检查。它们尚未形成同条件的真实游戏、网络和客户端显示测量。特别是，用户报告的约 6% pacing jitter 仍需要日志、统计窗口、编码器、显示器刷新率和客户端环境相同的实机数据，才能判断成因或效果。
