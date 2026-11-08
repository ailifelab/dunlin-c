# Agent Mailbox

独立 C11/SQLite 收件箱，服务于项目内显式共享和父子 Agent 的消息。
接口位于 `include/dunlin/agent_mailbox.h`，没有 Swift、语言引擎、线程、网络或
文件系统实现依赖；调用方借用连接并认证身份及发送者，模块不把 ID 当作登录凭据。

消息有项目、发送者、接收者、去重ID、可选回复ID、精确UTF-8正文和内容哈希。
独立信封哈希还覆盖身份和路由字段，不能只保留正文哈希就把消息改归另一个发送者
或接收者。当前开发格式v2拒绝先前未接入客户端的仅正文哈希原型，不静默升级。
项目成员必须显式加入；停用后不再准入新发送或待处理读取。待处理读取采用一致的
SQLite快照，分页游标不等于已消费；重启后应从头检查尚未确认的消息。
哈希和正文在读取时再次核对，失败不返回部分消息。

消息确认只能在调用方已有的写事务中进行。宿主须核对实际规范消息序号/哈希，
在同一事务写聊天来源并调用 `dl_agent_mailbox_ack`；失败必须整体回滚。
确认本身不会唤醒模型、启动工具或产生隐含授权。

独立构建：

```sh
cmake -S modules/agent_mailbox -B build/mailbox -DBUILD_TESTING=ON
cmake --build build/mailbox
ctest --test-dir build/mailbox --output-on-failure
```

这是协作基础存储，不是完整多 Agent 功能。实际监督器、spawn/followup/wait/cancel、
权限继承、任务与文件租约、C聊天提交接入和项目UI仍需独立完成。当前 macOS/Linux/
Windows 等价执行没有从C11声明推断；平台执行证据见相应验证报告。
