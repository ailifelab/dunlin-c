import AppKit
import SwiftUI

extension WorkspaceStore {
  func reloadIndexStatus() {
    workspaceRequest(["op":"index.status"],as:HistoryIndexStatus.self) {result in
      if result.ok {self.indexStatus=result.data}
    }
  }
  func indexOperation(_ op:String,value:Any? = nil) {
    var body:[String:Any]=["op":op]
    if let value {body[op == "index.quota" ? "bytes":"value"]=value}
    workspaceRequest(body,as:HistoryIndexStatus.self) {result in
      if result.ok {self.indexStatus=result.data;self.indexWarning="";self.prepareIndex()}
      else {self.indexWarning="索引操作未完成："+(result.error ?? "本地服务错误")}
    }
  }
}
struct IndexSettingsView: View {
  @EnvironmentObject var store:WorkspaceStore
  @State private var quota=64
  var body:some View {
    Form {
      Section("本地索引") {
        LabeledContent("处理方式",value:"本地 · 后台异步 · 默认不调用模型")
        Text("索引用于定位原文，不生成长期记忆。每条文本最多保留 4 个原文跨度用于整理候选；候选不代表完整语义。").font(.caption).foregroundStyle(.secondary)
        if let status=store.indexStatus {
          Toggle("暂停后台整理",isOn:Binding(get:{status.paused},set:{store.indexOperation("index.pause",value:$0)})).accessibilityIdentifier("index.pause")
          LabeledContent("状态",value:!status.available ? "索引不可用，聊天仍可使用":status.fault == 2 ? "缓存配额已满":status.fault != 0 ? "整理未完成":status.paused ? "已暂停":status.pending>0 ? "正在整理":"已整理")
          LabeledContent("已索引文本",value:"\(max(0,status.documents)) 条")
          LabeledContent("待处理文本",value:"\(max(0,status.pending)) 条")
          LabeledContent("已准备原文跨度",value:"\(max(0,status.prepared)) 条文本")
          if status.partial>0 {Text("\(status.partial) 条文本的候选跨度为部分覆盖；这不影响完整原文保存。").font(.caption).foregroundStyle(.secondary)}
          LabeledContent("索引数据库空间",value:ByteCountFormatter.string(fromByteCount:status.bytes,countStyle:.file))
          Text("此处只计算派生索引数据库；聊天原文、SQLite 日志和重建时的临时空间另计。").font(.caption).foregroundStyle(.secondary)
        }
      }
      Section("缓存配额") {
        HStack {
          TextField("最大索引空间（MiB）",value:$quota,format:.number)
          Button("应用配额") {store.indexOperation("index.quota",value:Int64(quota)*1048576)}.disabled(quota<1||quota>4096)
        }
        Text("达到配额后保留待处理队列，聊天仍然保存。可以提高配额或重建；配额不代表整个应用的磁盘占用。").font(.caption).foregroundStyle(.secondary)
      }
      Section("整理与恢复") {
        HStack {
          Button("继续整理索引",action:store.prepareIndex)
          Button("重建派生索引…") {
            let alert=NSAlert();alert.messageText="重新建立本地检索索引？"
            alert.informativeText="原始聊天与项目文件保留。检索结果会在后台重新生成，期间可能暂不完整。"
            alert.addButton(withTitle:"重建索引");alert.addButton(withTitle:"取消")
            if alert.runModal() == .alertFirstButtonReturn {store.indexOperation("index.rebuild")}
          }.accessibilityIdentifier("index.rebuild")
        }
        if !store.indexWarning.isEmpty {Text(store.indexWarning).foregroundStyle(.orange)}
      }
    }.formStyle(.grouped).onAppear {store.reloadIndexStatus()}
      .onChange(of:store.indexStatus?.quota) {_,bytes in if let bytes {quota=Int(bytes/1048576)}}
  }
}
