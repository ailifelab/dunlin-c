# DSH 真实插件兼容记录

状态：开发中。音视频保持 TODO。本表记录明确版本的测试结果，不代表完整 DSH 兼容。

| npm 包 | 测试版本 | 当前结果 |
|---|---|---|
| dsh-better-sidebar | 0.30.1 | 超出当前包文件布局/大小限制，导入未通过 |
| dsh-catppuccin | 0.2.6 | 宿主状态 JSON 路由运行通过；原始客户端主题界面尚未适配 |
| dsh-image-gen | 0.11.0 | 超出当前包文件布局/大小限制，导入未通过 |
| dshmarket | 1.73.15 | 缺少运行依赖；激活失败并自动停用、保留日志 |
| dsh-project-manager | 0.57.0 | 超出当前包文件布局/大小限制，导入未通过 |
| dsh-checkpoint-rewind | 0.6.34 | 缺少运行依赖；激活失败并自动停用、保留日志 |

Catppuccin 另验证了 0.2.3 → 0.2.6 的实际下载与切换。完整主题配色尚不能视为通过。

重验入口：`scripts/prepare-real-dsh-tests.py` 固定 npm 版本、检查 SHA-512，不执行脚本；
`tests/desktop_real_dsh.py` 运行实际发布包入口；`tests/desktop_npm_install.py` 通过 C 安装器下载并运行。
输入配置在 `catalogs/dsh-test-pins.json`。GitHub 源码 commit 与 npm tarball 为分别记录的输入身份。
测试只使用独立临时 HOME / 数据目录；公开仓库不携带本机日志、状态、包缓存或凭据。
