# 商店、社区索引与本地配置

## 开箱可同步的社区索引

插件商城包含两个数据源，首次浏览只创建本地配置，不自动联网：

- [DSHSeek 社区索引](https://dshseek.com/market/)：第三方维护；JSON 地址为 `https://dshseek.com/market/dsh-index.json`。
- [Dunlin 常用 DSH 插件清单](../catalogs/dsh-plugins.json)：由本项目维护；同步地址为 `https://raw.githubusercontent.com/ailifelab/dunlin-c/main/catalogs/dsh-plugins.json`。

点击来源的“同步”更新当前身份的本地缓存，展开后可搜索插件并打开上游项目。
断网或格式校验失败时保留上次成功同步的列表。同步不安装、不更新插件，也不执行
上游索引内的 `install` 命令。当前清单是候选项目集合，不代表 Dunlin 已支持这些插件。
未完成兼容验证的条目显示提示，安装按钮保持禁用。

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
