# 固定 DSH 插件验收 · 2026-11-09

本轮在隔离的 macOS ARM64 环境验证维护清单中的六个真实 npm 发布包。版本、归档摘要
见 `catalogs/dsh-test-pins.json`，更新输入见 `catalogs/dsh-update-test-pins.json`。
固定 DSH 基线为 `0.2.1-rc.3`，Cordis 为 `4.0.5`，测试 Node 为 `26.13.0`。
这份说明只覆盖下面的兼容能力，不代表完整 v5.1 产品或任意 DSH 插件验收通过。

## 已通过的行为

每个包均通过生产 C 安装入口下载旧版与新版，验证 SHA-512，默认不启用；随后显式启用、
切换版本并执行真实上游入口、停用、重新启用、模拟独立宿主丢失、自动禁用和版本日志、
卸载所有版本并清理无引用安装缓存。卸载后再次执行被拒绝，历史日志保留。
没有运行 npm lifecycle scripts，也没有用模拟插件代替六个上游发布包。

| 插件 | 已观察的实际效果 |
|---|---|
| dsh-better-sidebar | C 校验的当前工作区创建和读取文件；实际 WebSocket 文件变更通知 |
| dsh-catppuccin | 原始 React 设置、四种配色、辉光和玻璃控件；原生选择器同步、重开及重启持久化、停用回退 |
| dsh-image-gen | 真实 PNG 保存和规范化、附件存储统计、实际 SDK 画布状态工具 |
| dshmarket | 上游目录读取、收藏写入及持久化 |
| dsh-project-manager | 看板、分组和卡片写入及持久化；SDK 工具注册 |
| dsh-checkpoint-rewind | 按 C 会话位置捕获文件快照；拒绝回退不改文件；手工命令确认后恢复真实文件内容 |

原生窗口逐个检查六项启用、宿主异常后关闭开关、按插件保留日志和卸载。
下载安装与历史版本选择使用 Catppuccin 验证。配置页检查版本化 JSON 编辑入口，
实际 SDK 工具结果页检查执行结果。原始主题页额外验证四种配色、玻璃和辉光，
以及与内置“科技白”“科技黑”入口协作。旧版主题的存储接口和根级作用域也有回归。

模型工具通过 C 的工具服务接入。确定性 HTTP 模型测试验证实际 SDK schema 出现在请求中、
每次副作用调用必须授权、拒绝不产生文件效果、只读/禁用工具策略不公开未知副作用工具。
手工测试入口验证真正文件写入；模型命令没有自动批准。进程中断恢复后，插件版本、配置或
启用状态变化会在再次请求模型或执行工具之前阻止旧快照继续运行。

检查点确认暂停同一次上游调用，不重新执行；C 在回答前写入决策审计。拒绝、没有原生
回答者或审计失败均不允许恢复。SDK 工具意外异常会禁用插件，日志不包含原始私有载荷。

## 可复验入口

在独立构建目录准备锁定依赖和 C 桌面库后运行；路径参数必须指向本次构建产物。
带 ASan 的嵌入式 Python 库应使用匹配的 Python/ASan 启动器，子进程测试同时设置
`DUNLIN_TEST_PYTHON`。所有测试使用自己的临时 HOME、数据目录和工作区。

```sh
python3 -B scripts/prepare-real-dsh-tests.py
python3 -B tests/desktop_all_npm_lifecycle.py LIB ASSETS catalogs/dsh-update-test-pins.json RESULTS
python3 -B tests/desktop_real_dsh.py LIB ASSETS INPUTS RESULTS
python3 -B tests/dsh_real_services.py INPUTS RESULTS
python3 -B tests/desktop_extension_model_tools.py LIB ASSETS
python3 -B tests/desktop_extension_resume.py LIB ASSETS
python3 -B tests/desktop_real_checkpoint.py LIB ASSETS
python3 -B tests/desktop_plugin_configuration.py LIB ASSETS
node scripts/build-dsh-web.mjs
node tests/dsh_web_runtime.mjs
node tests/dsh_catppuccin_ui.mjs
```

归档准备脚本拒绝覆盖已有输入，应在全新验收目录/checkout 首次运行。
另外执行了归档恶意路径/链接/超限负例、2048 文件导入与启用完整性检查、2049 文件及
单文件超限拒绝、客户端投影、配置恢复、调用审计、宿主健康检测和 macOS 动态库闭包检查。
2048 文件、16 MiB 单文件、96 MiB 展开内容和 32 MiB 归档是桌面安装边界，不是性能指标。

开发过程中发现并修复了依赖缺失、导入和再次验证限额不一致、主题默认服务不完整、
旧版主题 store 接口、重新启用丢失配置和版本菜单内同步弹窗等问题。此前失败日志在本地
验收目录保留；修复后的结果分别记录，没有删除失败断言或降低预期来获取通过。

## 保留的边界

- 其他五项的完整上游 React 工作台和任意私有 UI 插槽尚未适配；清单标为部分兼容。
- 图像服务生成/编辑和依赖外部模型的自动任务，需要用户配置模型后继续真实服务测试。
- SDK 业务数据与原生项目/聊天没有完整双向同步；SDK 会话分叉不等于原生聊天回滚。
- 音视频保持未开发完成的试验性 TODO。Linux、Windows 和 ESP32 不计为本轮通过。
- 原生应用是本地签名验收构建；没有进行公证、商店分发或证书签名验收。

公开仓库仅同步源码、锁文件、固定公开归档身份和说明文档。运行数据、聊天、模型配置、
凭据、证书、依赖缓存、构建产物及本地原始验收日志不在发布内容中。
