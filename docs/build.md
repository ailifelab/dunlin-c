# 构建与开发

## Dunlin Copilot / macOS

从完整仓库打开 `dunlin-copilot/macos/DunlinCopilot.xcodeproj`，选择
`DunlinCopilot` Scheme。当前目标为 macOS 26+ / ARM64；SwiftUI 使用 Xcode 的系统 SDK。
构建还需 C 工具链、Python 3.9+、Node/npm（仅用于构建可选扩展界面资源）。
固定的 C 依赖由构建脚本下载并检查摘要，保存在 `build/`，首次构建需要网络。
构建阶段不会启用 DSH 环境或运行第三方插件。

```sh
xcodebuild -project dunlin-copilot/macos/DunlinCopilot.xcodeproj \
  -scheme DunlinCopilot -configuration Debug \
  -derivedDataPath build/xcode CODE_SIGNING_ALLOWED=NO build
```

共享 Scheme 的 Run 使用隔离开发数据目录。自行在应用中配置模型，勿将密钥写进代码。
在 Xcode 的 Signing & Capabilities 选择自己的 Team；仓库不含证书、私钥或 Team。
Product → Archive 负责归档。Developer ID 签名、公证、App Store 沙盒与审核仍需
单独完成，当前代码不代表可以直接上架。

## Dunlin Compact / Dunlin Command

C11 编译器、Make、json-c、libcurl 与 pkg-config 是桌面命令行构建依赖。

```sh
make BUILD=build/cli all
```

独立内核也可使用 CMake：

```sh
cmake -S . -B build/kernel
cmake --build build/kernel
ctest --test-dir build/kernel --output-on-failure
```

示例配置为 `config/example.json`、`config/multi-model.example.json`。
真实凭据通过环境变量或客户端凭据存储提供。工具执行需要宿主审批策略；不要为测试
自动允许模型命令。ESP32 的板级源码与配置位于 `dunlin-copilot/esp32/`，实机验证独立进行。

## 状态与验证边界

项目处于开发中。普通聊天、项目工具、模型切换、索引与扩展均需针对真实使用场景验收。
音视频是 **TODO / 暂未开发完成的试验性功能**，后续再进行真实设备和 AI 服务联调。
Linux/Windows GUI 目前仅预留；DSH 为固定版本的选定兼容子集，不宣称所有插件可运行。
公开仓库不包含本机历史验收数据、运行状态或模型配置；测试源码用于重新验证，不将
历史运行结果冒充当前通过证据。
