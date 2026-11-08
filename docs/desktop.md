# Dunlin Copilot 桌面客户端

当前确认稿位于 `dunlin-copilot/macos/design/2026-11-07-v5.1/`。总体项目为
Dunlin C，轻量运行核心为 Dunlin Compact，命令行外壳为 Dunlin Command，
各端客户端为 Dunlin Copilot。当前实现和验收仍在进行中，完整产品验收以
`validation/requirements.json` 为准；当前证据在 `reports/v51-current/`。

## 目录与边界

| 目录 | 职责 |
| --- | --- |
| `dunlin-compact/{kernel,runtime,plugins,platform}` | C11 内核、Agent 运行、模型协议和平台工具；无 SwiftUI、Node 或 Python 依赖。 |
| `dunlin-command/src` | 命令行输入、输出与应用装配。 |
| `dunlin-copilot/macos` | SwiftUI、AppKit、AVFoundation、Keychain、Xcode 工程和 macOS 打包。 |
| `dunlin-copilot/esp32` | 设备端装配；不装配复杂桌面服务。 |
| `dunlin-copilot/{linux,windows}` | 分平台客户端目录；目录存在不代表原生界面已经实现。 |
| `desktop` | C 应用编排、权威会话、项目文件与命令、配置、扩展管理及恢复。 |
| `include/dunlin` | 独立 C 模块和桌面接口；`compact/` 为轻量核心接口。 |
| `modules/identity`, `modules/knowledge` | 身份路由、隔离存储、图语义记忆、来源和本地文本 RAG。 |
| `modules/history_index`, `modules/context` | 本地异步词项索引、来源检查、上下文预算与压缩。索引不等于人物记忆。 |
| `modules/agent_scheduler`, `modules/agent_mailbox` | 持久有界调度、工作区互斥、子任务和作用域收件箱。 |
| `modules/grants` | 单次工具审计和可插拔撤销租约；本地失效与外部撤销结果分开。 |
| `modules/media` | 媒体生命周期、WebSocket 和实时服务协议；设备采集由平台提供。 |
| `modules/skills`, `modules/catalog`, `modules/content` | 不可变版本、三级作用域、统一目录和内容完整性。 |
| `modules/extensions` | 独立进程和 RPC；不绑定具体语言。 |
| `compat/dsh`, `hosts/{node-dsh,python}` | 可移除的固定 DSH 适配与语言宿主。默认不开启 DSH 环境。 |
| `modules/tool_records`, `modules/trajectory`, `modules/file_history` | 工具事实、可选模型轨迹、文件前后快照与回退。 |
| `config` | 不含密钥的示例配置。 |

## 执行与并发

每个 C store 由自己的串行队列使用。主 store 持有进程独占锁并负责启动恢复；
`dc_open_agent_worker` 创建独立 SQLite 连接、复制同一进程锁描述符，不执行启动恢复。
桌面协作队列最多运行两个模型任务；同一聊天与同一工作目录的写任务仍由 C 调度器互斥。
父任务在子任务委派边界停放租约，再恢复原续体；子任务不继承工具批准。

原生界面冻结任务模型配置，分别持有取消通道，按顺序展示工具审批。运行中的只读页面
通过 WAL 快照读取，不阻塞模型请求。只有全部任务结算后才能切换身份或执行破坏性管理。
关键词索引在源记录提交后异步处理，错误不能反向删除原始会话。

## 数据与配置

默认数据根为 `~/Library/Application Support/Dunlin C/`，权限 0700。
身份目录分别保存会话、设置和派生缓存。原生验收包通过 Info.plist 的
`DunlinAcceptanceDataDirectory` 使用独立测试目录，不复制用户密钥或聊天。

- `settings.json`：当前身份的界面、设备、上下文策略等设置。
- `conversations.sqlite`：权威聊天、项目、模型配置、Skills、工具记录、授权、媒体文本、
  关键帧、记忆和持久索引任务。图语义使用 SQLite 索引节点与边实现。
- `history.sqlite`：可重建的本地词项索引。
- `trajectory.sqlite`：可关闭的本地运行轨迹；不是工具执行成功的替代证据。
- `Compatibility/`：用户开启后准备的独立兼容环境；满足要求时可复用系统 Node。
- API 密钥：系统 Keychain。C 请求只借用本次解析的密钥，不修改进程环境或写入日志。

聊天删除以事务清除源与引用，并保留持久派生缓存清理任务。文件、命令及远端服务的
已发生效果不会因删除聊天而自动回滚。

## 构建与验收

在 Xcode 打开 `dunlin-copilot/macos/DunlinCopilot.xcodeproj`，选择
`DunlinCopilot` scheme。工程内可调整签名团队、Bundle ID、版本、entitlements 和 Archive。
SwiftPM 开发构建使用 `scripts/build-macos.sh`，产物为
`dist/macos/Dunlin Copilot.app`。两条构建路径共享 C 服务、图标和隐私用途声明。

目标系统为 macOS 26+；当前机器为 macOS 27.2 ARM64。依赖构建固定版本的
json-c、OpenSSL 和支持 WebSocket 的 curl；产物依赖闭包另行检查。
本地签名不是开发者分发签名，也不意味着完成公证或商店审核。

`build-desktop-test-macos.py` 生成 C 测试库与输入哈希，`--sanitize` 启用 ASan/UBSan。
本地 HTTP/WebSocket 模拟服务能验证真实 C 执行链、协议和受控工具效果，不能证明外部
模型服务兼容性、实际麦克风/摄像头质量或其他平台等价。

## 尚待完成的产品验收

原始录音录像与本地 ASR 的真实设备验收、更多外部权限适配、DSH 服务与依赖能力、
全部设计页面和状态、实际设备及外部模型服务验证仍需分别完成。插件不满足所声明的服务、
平台、版本或界面档位时应阻止安装/启用并展示原因；不承诺任意 Electron/React 插件无改造运行。

## 本地输入转写

macOS 可显式开启 Speech 本机输入转写，并选择中文或英文；检测到
`supportsOnDeviceRecognition` 且授权通过才创建请求，每个请求固定
`requiresOnDeviceRecognition=true`。输入音频仍发送到用户配置的 AI 通话服务，
这里的“本地”仅指输入文字识别，不代表整场通话离线。服务端输入 ASR 与本地
输入 ASR 二选一；AI 输出的服务端文本仍正常保存。

适配器只保留有界内存 PCM，按服务端输入提交边界识别，单段最多 60 秒，
最多两个待识别段；网络时延可能使边界包含尾部静音。仅提交最终识别文本，
失败/超时/排队溢出记录缺口，停止后最多等待五秒结算再取消。长期原文、
来源标签、保留与身份隔离仍由独立 C 媒体服务负责。系统 Speech 权限撤销
进入手动系统设置流程，不把本地租约失效冒充系统授权已撤销。

当前仅有 SDK 编译及 C 来源/权限边界测试；本机识别可用性、实际语言质量、
麦克风硬件、实时服务和声音播放需要单独设备验收。原始录音录像已有独立 C 容器、存储和原生回放实现，当前验证使用隔离合成媒体，见下文。
参考 Apple 官方 supportsOnDeviceRecognition、requiresOnDeviceRecognition、
SFSpeechRecognizer.requestAuthorization 与 endAudio 文档。

### 已启用扩展的启动恢复

客户端装配扩展资源后调用独立 C 服务 `extensions.restore`。它只复用之前明确
启用的 Node/Python 可执行路径，不下载缺失引擎、不确认升级，也不启动默认关闭
的环境。启用状态、精确版本和配置在本地持久化；恢复前重新校验包摘要与兼容
条件。已停用、被隔离和有未知执行结果的包不会自动恢复；重复调用不重新激活
已运行的实例。某个宿主恢复失败不会阻断另一个宿主，界面提示检查扩展设置。
配置是本地插件配置，不是凭据保险库；密钥仍应使用宿主提供的凭据引用。

原生客户端每五秒检查可选宿主健康状态，单个身份最多一个待完成检查；普通
串行模型执行期间延后检查。宿主退出由 C 层停用包并登记故障，界面显示日志
入口。检查本身不发模型请求，也不访问插件市场。

扩展的激活与停用同样写入持久调用记录，成功返回前完成结算。存储不可写时不进入
插件代码；进程中断、宿主传输中断或结算失败保留未知边界，并隔离插件。启动恢复
会产生可见的 recovery 故障条目，而不会静默重新执行存在未知效果的激活。

授权页检查已失效的历史记录不会停止当前任务。活动记录按其聊天及任务父链定位
承载 worker；找不到归属时提示重新核对，不回退到停止全部任务。系统权限支持
确认范围后的逐项批量撤销，每项分别展示成功、需用户处理、重试或未知状态，
停止后续项目不会撤回已经完成的操作。此入口仍只覆盖已接入并记录过的权限适配。

### Optional DSH React client surface (v5.1 implementation)

`compat/dsh/web` is an independent browser adapter pinned to the same DSH rc.3
source as the Node host. It bundles one React module table and the unmodified
upstream slot/renderer sources; building verifies both UI and Cordis pins. It is
packaged as `ExtensionAssets/web/runtime.js` and `runtime.css`, without installing or enabling the
optional Node engine at application startup.

A package may declare `client: {format: "dsh-lazy-cjs@1", entry: "client.js",
slots: ["conversation.composer.dock"]}`. The package ID must equal the lazy module
registration ID. The implemented slots are the session-scoped composer dock and
`settings.general.item` (used by the unchanged Catppuccin settings row). React,
ReactDOM, Cordis, slots, the fixed locale/theme runtimes and approved primitives
share one module table. Missing services/imports and root replacement fail
explicitly. Other slots, the full DSH remote codecs and arbitrary package
dependencies are not a complete compatibility contract.

C owns enabled/healthy/version state and immutable script verification through
`extensions.clients`. Native code receives bounded verified bytes, not arbitrary
filesystem paths. Each package/chat has its own nonpersistent WebKit context,
local resource allowlist and restrictive CSP. Owner-bound HTTP/WebSocket commands
and bounded C-managed client storage are exposed; no arbitrary native selector
or model credential bridge is available. Native theme preferences synchronize
with the original React controls. Client storage is identity/package scoped,
version checked and cleared by uninstall. A surface fault is bound to that package
and version, deactivates its host plugin, quarantines it and opens the existing
fault log. The current browser fault record stores a bounded classification;
source-level browser stack diagnostics remain unavailable. The default UI has no
web surface when no compatible enabled client plugin exists.

### Original call-media recording

Recording is off by default and opt-in per call. `modules/media/recording.c` is an
independent C11 packet codec with bounded payloads, per-track timestamps, CRC32
integrity checks and an explicit terminal record. Its `.dcr` container is local
Dunlin data, not an MP4/WAV export format. `desktop/media_recording.c` owns private
exclusive files, recording policy, partial/failed/interrupted states, expiry and
chat-deletion cleanup. Playback obtains an independent file handle after checking
the owning call and retention policy, then leaves the serialized store queue.

The macOS adapter records sent/received 24 kHz mono PCM audio and optional VGA
camera JPEG samples at up to 15 fps, separately from the model's lower-frequency
camera sampling. This is not the remote party's video: current Realtime adapters
do not provide that. The service audio track can include generated audio that was
interrupted before it played; original-media playback explicitly states this
limit and does not rewrite conversation history from that track.

Each call is bounded to 1 GB of original media and the existing one-hour call
policy. A bounded asynchronous writer keeps device callbacks off disk I/O; quota
or backlog failure stops recording and marks the saved portion incomplete while
the call may continue. An opt-in capture that cannot open its file prevents call
startup rather than silently dropping the requested recording. Native playback
bounds queued audio and image decoding, supports replay/stop, and shows partial
recordings explicitly. No raw-media bytes are uploaded by the storage/replay
module. Actual device/provider capture still requires separate integration
acceptance; synthetic file/PCM/JPEG playback is not evidence of live capture.

## 当前配置与存档边界

原生客户端只从 C 配置注册表和项目表加载模型、服务商与项目，不再自动导入
界面设置中的旧列表。`settings.json` 仅保存界面与设备等偏好；显式的配置文件
预览导入仍是独立功能。C 层的旧初始化入口也已移除；新配置一律通过
`config.save` 的版本检查与当前模型参数校验。已移除 16 字符存档指纹的执行回退，遇到该格式会在
模型请求前拒绝继续；当前格式的中断恢复、未知效果检查与批准流程保留。

DSH 环境页按确认稿分为启用开关、实际状态和环境检查结果，高级引擎选择与
诊断信息收在折叠区。商城在环境未就绪时显示设置入口；该提示不会替代 C 层
对每一个包的安装校验。未配置官方目录发布源时，不伪造官方商店或签名。

授权中心已分为“生效中”和“历史记录”；当前应用租约过期与操作系统权限
撤销分别呈现。历史记录中的批量撤销仍逐项保留成功、需手动处理或未知结果。

### Optional model-facing SDK tools

`desktop/extension_tools.c` decorates the desktop C tool service. The optional
host exports actual SDK schemas; C freezes full package/config identities into
the admitted run snapshot and uses digest-bound names. Ask policy requires
exact-argument approval for every extension tool. Read-only/no-tools omit these
unrestricted tools. Updates, configuration changes and disabled bindings block
an interrupted run before another model request; host loss/cancellation retains
unknown effects. This adapter is absent from the compact/ESP32 composition.

Plugin configuration is versioned and editable only while the target version is
stopped, using `extensions.config.get/save` with a compare-and-swap hash. A manual
re-enable with no config argument retains its saved configuration. Native plugin
questions suspend the same host invocation and resume after a durable C decision;
missing native approval or audit fails closed. See [fixed plugin capabilities](dsh-compatibility.md).
