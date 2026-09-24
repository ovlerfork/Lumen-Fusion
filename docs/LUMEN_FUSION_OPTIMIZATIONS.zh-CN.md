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

## 7. 后续已提交的节拍与所有权修复

本节补充 `e9e527b0` 之后的增量，主要见 [`f0f67e9b`](https://github.com/ovlerfork/Lumen-Fusion/commit/f0f67e9b) 和 [`eeb8d7c0`](https://github.com/ovlerfork/Lumen-Fusion/commit/eeb8d7c0)。代码归属于本分支的实现与修复，不把系统 API 或上游零拷贝基础认作原创。各版本实际二进制、测试计数和验收范围见[发行页](https://github.com/ovlerfork/Lumen-Fusion/releases)。

| 改动 | 机制 | 证据与边界 |
| --- | --- | --- |
| RTP 时间线 | 新画面使用采集时间；空闲重复帧使用当前帧发送起点，而不是上一次发送的旧截止时间。每个会话独立维持递增的扩展 tick，在网络边界截断成 32 位，同帧所有 FEC 块共用一个时间戳 | 原始采集时间不被改写，避免通过修改统计来源制造低延迟。单元测试覆盖重复、新画面、倒退输入和回绕；不等于验证了所有客户端的呈现策略。 |
| NV12/P010 不做多余 CPU 映射 | 硬件路径只交付 `CVPixelBufferRef`，避免锁定不读取的 CPU 地址；兼顾 video-range 和 full-range。BGRA/软件访问继续锁定 | 原生缓冲区测试检查多种格式、像素内容、引用寿命和 A→B 替换；不声称硬件采集完全没有其他同步成本。 |
| 缓冲替换失败处理 | 先取得新引用，再释放旧引用；分配失败时释放新增 retain、保留旧帧 | 不再在失败路径泄漏新引用或提前丢失旧帧。 |
| 图像池并发 | 采集回调与退出检查取得空闲图像时，共享的选择、复用和裁剪过程受局部互斥保护 | 编码、发包和重试休眠不持该锁；不使用扩大队列掩盖争用。 |
| 分数帧率一致 | SCK 与 AVFoundation 使用和编码器相同的有理数帧间隔，而不是一边 59.94、另一边固定 60 | 保留整数初始化兼容入口。这修正配置不一致，不能用约 0.1% 的频率差直接解释 6% 的客户端指标。 |
| 有界节拍诊断 | 会话持有统计窗口，最多 512 个水塘样本覆盖整个窗口；均值和最大值包含全部观测。新增被使用画面的采集间隔、编码输出间隔、发包完成间隔、p99 和超预算计数 | 编译进应用，但运行时默认关闭。高于容量时分位数是估计值；被覆盖而未送出的源帧不包含在 `source_frames` 中。 |

### 如何理解动态测试

原生编码测量使用 30 帧预热和 180 帧测量，包含运动区域与场景切换。测试纹理在计时前预计算，随后按行复制到正确持有的原生缓冲区，避免把昂贵的软件图案生成误认为编码瓶颈。生成时间、提交间隔、提交到输出时间及输出间隔分别记录；冷启动、慢帧和错过时隙不会被隐藏。错过目标时隙时跳过输入时隙，不突发追赶。

这些是合成源到原生编码输出的测量，不包含真实 SCK 抓屏、网络或客户端显示。测试通过证明其正确性断言成立，不保证每个设备持续满 60 FPS。用户报告的约 6% pacing jitter 仍需同分辨率、刷新率、码率和客户端条件的实机前后对比；本分支未把它写成已测得的改善数字。

## 8. 登录自动启动与安全退出

实现见 [`9cf578bb`](https://github.com/ovlerfork/Lumen-Fusion/commit/9cf578bb)。菜单栏提供 **Launch at Login…**，打开原生设置面板；中文系统显示中文说明和复选框。

自动启动默认关闭，不使用配置文件中的布尔值冒充系统状态。用户勾选后，通过 `SMAppService.mainAppService` 注册。面板区分 `enabled`、`requiresApproval`、`notRegistered` 和失败；需要批准时提供打开系统登录项设置及取消入口。面板重新打开或重新激活时读取实际状态，重复启用/取消是幂等操作。系统中禁用后，应用不会在下一次启动时偷偷注册回来。

这是当前用户的下次登录启动，不是开机守护程序。应用必须实际位于 `/Applications` 或用户 `~/Applications` 内；DMG、临时路径和 App Translocation 不用于注册。系统批准及跨版本代码身份仍由 macOS 决定，ad-hoc 签名不保证升级后免重新授权。

另修正了 macOS 退出处理：POSIX 信号处理器仅通过非阻塞管道交接事件，在普通工作线程中执行日志和退出协调。独占信号注册、保存原处理方式、保持 `errno`、处理 EINTR、等待在途信号退出后关闭描述符，并在主线程执行显示设备清理。原来的直接启动及 LaunchServices 正常退出检查继续保留；不以强制杀进程通过验收。

原生登录项验收在一次性 macOS CI 用户中运行实际 `.app`：先确认没有既有注册，再执行显式启用、状态读取、取消及恢复检查。它不会替用户批准系统请求，也不模拟真实注销再登录。

官方机制：[SMAppService](https://developer.apple.com/documentation/servicemanagement/smappservice)、[注册与用户批准](https://developer.apple.com/documentation/servicemanagement/smappservice/register())。

## 9. 安装与诊断的收尾

DMG 提供原生 `.app`、Applications 快捷入口和中英安装说明。普通安装只需退出旧版、拖动替换、弹出镜像并打开应用，不需要 Homebrew、终端脚本或 sudo。原配置、证书和配对继续使用 `~/.config/lumina`。

22 种界面语言的 macOS 音频说明已同步为原生系统音频采集，移除“只能访问麦克风”的过期提示。HTTP 调试日志按大小写无关的字段名隐藏认证头、Cookie、会话密钥和配对秘密；它不是对所有个人信息的完整匿名化，日志仍可能含 IP、设备名和路径，分享前仍应检查。
