# 商店、社区索引与本地配置

## 开箱可同步的社区索引

插件商城包含两个数据源，首次浏览只创建本地配置，不自动联网：

- [DSHSeek 社区索引](https://dshseek.com/market/)：第三方维护；JSON 地址为 `https://dshseek.com/market/dsh-index.json`。
- [Dunlin 常用 DSH 插件清单](../catalogs/dsh-plugins.json)：由本项目维护；同步地址为 `https://raw.githubusercontent.com/ailifelab/dunlin-c/main/catalogs/dsh-plugins.json`。

点击来源的“同步”更新当前身份的本地缓存，展开后可搜索插件并打开上游项目。
断网或格式校验失败时保留上次成功同步的列表。同步不安装、不更新插件，也不执行
上游索引内的 `install` 命令。当前清单是候选项目集合，不代表 Dunlin 已支持这些插件。
未完成兼容验证的条目显示提示。维护清单内的 npm 包可显式下载安装测试；其他条目尚不支持下载安装。

## 配置文件

当前身份的数据目录中有 `plugin-index-sources.json`，客户端的“添加索引”和“移除”
会原子更新此文件。可手动编辑后点击“重新读取”。移除来源不会卸载插件。

```json
{
  "format": 1,
  "sources": [
    {
      "id": "dunlin-common",
      "title": "Dunlin 常用 DSH 插件清单",
      "url": "https://raw.githubusercontent.com/ailifelab/dunlin-c/main/catalogs/dsh-plugins.json"
    }
  ]
}
```

新增第三方使用同一 `format: 1`、`plugins` 数组协议。每项至少需要 `name` 和
`repository.url`（HTTPS）；`title`、`description` 可为字符串或 `zh` / `en` 文本对象。
C 模块只投影名称、说明、项目链接，不把上游命令、版本或安装声明视为可信执行数据。
请求不跟随重定向、不发送模型凭据；响应最多 4 MiB / 2000 条，连接超时 5 秒，
总超时 60 秒，支持取消。本地配置最多 64 个来源、256 KiB，拒绝符号链接与损坏 JSON。

## 可安装的验签目录

`catalog-sources.json` 保存另一类来源：Dunlin Catalog v1 签名安装目录。
客户端添加、重命名、停用和恢复来源会更新文件。来源 ID 的 URL 与发布者公钥固定，
更换发布者应添加新来源。手动省略的旧来源会停用，已缓存信任记录保留。
此类目录要求发布者公钥、签名、版本摘要和受支持的包清单；不能将普通社区 JSON
冒充签名目录。Skills、插件和主题安装均继续使用原有校验与兼容门槛。

## 维护项目清单

编辑 `catalogs/dsh-plugins.json`，保留格式版本与仓库链接，核对项目存在与来源。
描述使用本项目撰写的简短说明；未验证适配的项目保持 `compatibility: unverified`。
提交到 `main` 后，客户端下次点击“同步”即可获取。没有定时后台安装或远程命令执行。

## npm 下载与生命周期（开发中）

维护清单内的六个仓库映射到明确的 npm 包名。开启 DSH 环境后，可在商城读取
已发布版本并点击“下载安装”；C 服务通过 registry.npmjs.org 的 HTTPS 元数据和
SHA-512 校验归档，限量解压，不执行 npm 安装脚本，不自动安装依赖、不自动启用。
安装包上限 16 MiB、解压上限 8 MiB、单文件 1 MiB、最多 127 个原始文件。
PAX/GNU 长路径、链接和越界路径目前拒绝；这些限制会使部分真实插件无法安装。

已下载插件在设置中单独启用。版本选择器切换已安装版本；再次下载安装另一已发布
版本就是更新流程，旧版本保留供切换。卸载先停止插件，移除所有安装版本与配置，
清理没有其他引用的内容缓存，保留历史执行和故障日志。故障会自动停用并隔离版本。

当前真实验证使用清单中的 npm 发布包：Catppuccin 0.2.3 / 0.2.6 的宿主状态 JSON
路由实际执行、状态持久化、版本切换、停用及卸载已通过。这只覆盖宿主部分；
四种主题配色、原始 React 设置界面仍未完成适配。另五项未通过运行兼容测试；
清单记录具体缺失依赖或包限制。下载成功不代表能够启用运行。
