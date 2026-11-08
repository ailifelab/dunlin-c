import AppKit
import SwiftUI

struct PluginFaultLogView:View {
  @EnvironmentObject var store:WorkspaceStore
  let onBack:()->Void
  @State private var faults:[ExtensionFault]=[]
  @State private var packages:[ExtensionRow]=[]
  @State private var environment:CompatibilityStatus?
  @State private var selected:Int64?
  @State private var error=""
  @State private var export:TraceExport?
  private var fault:ExtensionFault? {faults.first{$0.id==selected} ?? faults.first}
  private var version:ExtensionVersion? {
    guard let fault else{return nil}
    return packages.first{$0.source==fault.source && $0.package==fault.package}?.versions.first{$0.version==fault.version}
  }
  var body:some View {
    VStack(alignment:.leading,spacing:28) {
      HStack {
        Button("返回插件",action:onBack)
        Spacer()
        if faults.count>1 {Picker("故障",selection:$selected) {ForEach(faults) {item in Text("\(item.package) · #\(item.id)").tag(Optional(item.id))}}.frame(maxWidth:420)}
        Button("刷新",action:reload)
      }
      if let fault {
        VStack(spacing:0) {
          row("插件版本",value:"\(fault.package) · \(fault.version)")
          Divider()
          row("包摘要",value:version?.digest ?? "本地版本已不可读取")
          Divider()
          row("当前兼容环境",value:version?.manifest.engines.host == "python" ? "独立 Python 宿主":environment?.baseline ?? "未读取")
          Divider()
          row("处理结果",value:version?.quarantined == true ? "已停用 · 等待手动恢复":"故障历史 · 当前状态请查看插件列表")
        }.padding(.horizontal,20).background(store.themeSurface)
        VStack(alignment:.leading,spacing:16) {
          Text("\(Date(timeIntervalSince1970:Double(fault.created)).formatted())  ·  #\(fault.id)")
          Text("\(fault.phase) · \(extensionReason(fault.error))")
          Text(fault.diagnostic?.isEmpty == false ? fault.diagnostic! : "没有可用的源码诊断；请核对错误分类与插件版本。")
        }.font(.system(size:13,design:.monospaced)).textSelection(.enabled)
          .foregroundStyle(Color(red:0.84,green:0.88,blue:0.94)).padding(24)
          .frame(maxWidth:.infinity,minHeight:220,alignment:.topLeading)
          .background(Color(red:0.12,green:0.14,blue:0.17),in:RoundedRectangle(cornerRadius:10))
        Text("诊断只记录异常类型和源码位置；未记录调用参数、异常消息或完整聊天。环境一栏是当前环境，不能据此推断故障发生时的宿主版本。").font(.caption).foregroundStyle(.secondary)
        HStack {Spacer();Button("预览并导出日志…") {export=TraceExport(text:exportText(fault))}}
      } else if error.isEmpty {ContentUnavailableView("还没有插件错误日志",systemImage:"checkmark.shield")}
      if !error.isEmpty {Text(error).foregroundStyle(.orange)}
    }.onAppear(perform:reload).sheet(item:$export) {value in PluginFaultExportView(text:value.text)}
  }
  private func row(_ title:String,value:String)->some View {
    HStack(alignment:.top,spacing:24) {Text(title).fontWeight(.medium);Spacer();Text(value).font(.callout).foregroundStyle(.secondary).textSelection(.enabled).multilineTextAlignment(.trailing).fixedSize(horizontal:false,vertical:true)}.padding(.vertical,20)
  }
  private func reload() {
    error=""
    store.extensionRequest(["op":"extensions.faults"],as:[ExtensionFault].self) {r in
      if r.ok {faults=r.data ?? [];if !faults.contains(where:{$0.id==selected}) {selected=faults.first?.id}}
      else {error="错误日志读取失败，请重试。"}
    }
    store.extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self) {r in packages=r.data ?? []}
    store.extensionRequest(["op":"compat.status"],as:CompatibilityStatus.self) {r in environment=r.data}
  }
  private func exportText(_ fault:ExtensionFault)->String {
    let data:[String:Any]=["fault":fault.id,"package":fault.package,"version":fault.version,"packageDigest":version?.digest ?? "","phase":fault.phase,"error":fault.error,"created":fault.created,"diagnostic":fault.diagnostic ?? ""]
    return (try? JSONSerialization.data(withJSONObject:data,options:[.prettyPrinted,.sortedKeys])).flatMap{String(data:$0,encoding:.utf8)} ?? "{}"
  }
}
private struct PluginFaultExportView:View {
  let text:String
  @Environment(\.dismiss) private var dismiss
  @State private var error=""
  var body:some View {
    VStack(alignment:.leading,spacing:20) {
      Text("插件日志导出预览").font(.title2.bold())
      Text("保存前检查插件标识和文件名是否适合分享。这里只导出选中的故障记录。").foregroundStyle(.secondary)
      ScrollView {Text(text).font(.system(size:12,design:.monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}
      HStack {Text(error).foregroundStyle(.orange);Spacer();Button("取消"){dismiss()};Button("保存 JSON…") {save()}.buttonStyle(.borderedProminent)}
    }.padding(24).frame(width:780,height:600)
  }
  private func save() {
    let panel=NSSavePanel();panel.nameFieldStringValue="dunlin-plugin-fault.json"
    guard panel.runModal() == .OK,let url=panel.url else{return}
    do {try Data(text.utf8).write(to:url,options:.atomic);dismiss()}catch{self.error=error.localizedDescription}
  }
}
