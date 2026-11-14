import AppKit
import SwiftUI

extension WorkspaceStore {
  func confirmIndexRebuild() {
    let alert = NSAlert()
    alert.messageText = "重新建立本地检索索引？"
    alert.informativeText = "原始聊天与项目文件保留。重建会恢复后台整理。"
    alert.addButton(withTitle: "重建索引")
    alert.addButton(withTitle: "取消")
    if alert.runModal() == .alertFirstButtonReturn { indexOperation("index.rebuild") }
  }
  func reloadIndexStatus() {
    workspaceRequest(["op":"index.status"],as:HistoryIndexStatus.self) {result in
      if result.ok {self.indexStatus=result.data} else {self.indexWarning="索引状态暂不可读，请重试。"}
    }
  }
  func indexOperation(_ op:String,value:Any? = nil,completion:@escaping ()->Void = {}) {
    var body:[String:Any]=["op":op]
    if let value {body[op == "index.quota" ? "bytes":"value"]=value}
    workspaceRequest(body,as:HistoryIndexStatus.self) {result in
      if result.ok {self.indexStatus=result.data;self.indexWarning="";self.prepareIndex()}
      else {self.indexWarning="索引操作未完成："+(result.error ?? "本地服务错误")}
      completion()
    }
  }
}
struct IndexSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openWindow) private var openWindow
  @Environment(\.dismiss) private var dismiss
  @State private var quota = 64
  @State private var editingQuota = false
  @State private var action: IndexReview?
  @State private var saving = false

  private enum IndexReview: String, Identifiable {
    case rebuild, clear
    var id: String { rawValue }
  }
  private func change(_ op:String,value:Any?=nil) {
    guard !saving else {return}
    saving=true
    store.indexOperation(op,value:value){saving=false}
  }
  private func row<Control:View>(_ name:String,_ description:String,@ViewBuilder control:()->Control)->some View {
    HStack(spacing:18) {
      VStack(alignment:.leading,spacing:4) {
        Text(name).font(.body.weight(.medium))
        Text(description).font(.callout).foregroundStyle(.secondary)
      }
      Spacer(minLength:14);control()
    }.padding(.horizontal,20).padding(.vertical,12).frame(minHeight:64)
  }
  private func statusText(_ status:HistoryIndexStatus)->String {
    if !status.available {return "索引不可用"}
    if status.fault==2 {return "空间上限已满"}
    if status.fault != 0 {return "整理未完成"}
    if status.paused {return "已暂停 · 待整理 \(status.pending) 段"}
    return status.pending>0 ? "\(status.pending) 段内容等待整理" : "已整理 \(status.documents) 段内容"
  }
  var body: some View {
    VStack(alignment:.leading,spacing:22) {
      if let status=store.indexStatus {
        VStack(spacing:0) {
          row("本地聊天索引","每轮结束后在后台整理，不调用 AI 服务。") {
            Toggle("启用本地聊天索引",isOn:Binding(get:{!status.paused},set:{change("index.pause",value:!$0)}))
              .toggleStyle(.switch).labelsHidden().accessibilityIdentifier("index.enabled")
          }
          Divider()
          row("检索范围","家庭空间仅包含明确共享的内容。") {Text("当前身份允许访问的聊天").foregroundStyle(.secondary)}
          Divider()
          row("整理状态","新消息可以立即查看，索引会在后台更新。") {
            Text(statusText(status)).foregroundStyle(status.fault != 0 ? Color.orange:.secondary)
              .accessibilityIdentifier("index.status")
          }
        }.background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
        VStack(alignment:.leading,spacing:8) {
          Text("存储与处理").font(.headline)
          VStack(spacing:0) {
            row("处理方式","不等待索引完成即可继续聊天。") {Text("后台低优先级 · 本地分批处理").foregroundStyle(.secondary)}
            Divider()
            row("空间上限","达到上限时暂停整理，原始聊天保留。") {
              Menu(status.automaticQuota ? "自动管理" : "\(status.quota/1_048_576) MiB") {
                Button("自动管理（64 MiB）") {change("index.quota",value:0)}
                Button("自定容量…") {quota=Int(status.quota/1_048_576);editingQuota=true}
              }.fixedSize().accessibilityIdentifier("index.quota")
            }
            Divider()
            row("本地语义检索","可选增强需要本地语义模型。当前使用原文匹配。") {
              Toggle("本地语义检索暂未安装",isOn:.constant(false)).toggleStyle(.switch).labelsHidden().disabled(true)
            }
          }.background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
        }
        HStack {
          Button(status.paused ? "继续整理" : "暂停整理") {change("index.pause",value:!status.paused)}
            .accessibilityIdentifier("index.pause")
          Button("重建索引…"){action = .rebuild}.accessibilityIdentifier("index.rebuild")
          Button("清除索引…"){action = .clear}.accessibilityIdentifier("index.clear")
          Spacer()
          Button("搜索旧聊天…"){
            store.route="search";dismiss()
            if let window=NSApp.windows.first(where:{$0.identifier?.rawValue.contains("workspace")==true}) {window.makeKeyAndOrderFront(nil)}
            else {openWindow(id:"workspace")}
          }.buttonStyle(.borderedProminent)
        }
        Text("清除索引只移除可重建的检索数据。已索引 \(status.documents) 段，已准备原文跨度 \(status.prepared) 段，派生数据库占用 \(ByteCountFormatter.string(fromByteCount:status.bytes,countStyle:.file))。")
          .font(.caption).foregroundStyle(.secondary)
        if status.partial>0 {Text("\(status.partial) 段文本的候选跨度只覆盖部分原文。").font(.caption).foregroundStyle(.secondary)}
      } else if store.indexWarning.isEmpty {
        ProgressView("正在读取本地索引…")
      }
      if !store.indexWarning.isEmpty {
        HStack {Text(store.indexWarning).foregroundStyle(.orange);Button("重新读取"){store.reloadIndexStatus()}}
      }
    }.disabled(saving).onAppear(perform:store.reloadIndexStatus)
      .onChange(of:store.identityEpoch){_,_ in saving=false;editingQuota=false;action=nil}
      .sheet(isPresented:$editingQuota) {
        VStack(alignment:.leading,spacing:14) {
          Text("索引空间上限").font(.title2.weight(.semibold))
          TextField("容量（MiB）",value:$quota,format:.number).textFieldStyle(.roundedBorder)
          Text("可设置 1 至 4096 MiB。仅计算派生索引数据库的空间。").foregroundStyle(.secondary)
          HStack {Button("取消"){editingQuota=false};Spacer();Button("应用配额"){editingQuota=false;change("index.quota",value:Int64(quota)*1_048_576)}.disabled(!(1...4096).contains(quota))}
        }.padding(24).frame(width:430)
      }
      .alert(item:$action) { choice in
        let rebuilding=choice == .rebuild
        return Alert(title:Text(rebuilding ? "重新建立本地检索索引？":"清除当前身份的聊天索引？"),
          message:Text(rebuilding ? "原始聊天与项目文件保留。重建会恢复后台整理。" : "清除派生索引并暂停整理。原始聊天保留，可随时重建。"),
          primaryButton:rebuilding ? .default(Text("重建索引")){change("index.rebuild")} : .destructive(Text("清除索引")){change("index.clear")},secondaryButton:.cancel(Text("取消")))
      }
  }
}
