import AppKit
import SwiftUI

struct PluginFaultLogView:View {
  @EnvironmentObject var store:WorkspaceStore
  let onBack:()->Void
  var initialFaultID:Int64?=nil
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
        }.font(.system(.body, design: .monospaced)).textSelection(.enabled)
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
      if r.ok {faults=r.data ?? [];if !faults.contains(where:{$0.id==selected}) {selected=faults.first(where:{$0.id==initialFaultID})?.id ?? faults.first?.id}}
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
      ScrollView {Text(text).font(.system(.callout, design: .monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}
      HStack {Text(error).foregroundStyle(.orange);Spacer();Button("取消"){dismiss()};Button("保存 JSON…") {save()}.buttonStyle(.borderedProminent)}
    }.padding(24).frame(width:780,height:600)
  }
  private func save() {
    let panel=NSSavePanel();panel.nameFieldStringValue="dunlin-plugin-fault.json"
    guard panel.runModal() == .OK,let url=panel.url else{return}
    do {try Data(text.utf8).write(to:url,options:.atomic);dismiss()}catch{self.error=error.localizedDescription}
  }
}

struct PluginFaultDetail: View {
  @EnvironmentObject var store: WorkspaceStore
  let row: ExtensionRow
  let close: () -> Void
  @State private var fault: ExtensionFault?
  @State private var error = ""
  @State private var checked = false
  @State private var active = false
  @State private var working = false
  @State private var confirming = false
  @State private var logs = false

  var body: some View {
    VStack(alignment:.leading,spacing:18) {
      if logs {
        PluginFaultLogView(onBack:{logs=false},initialFaultID:fault?.id)
      } else {
        Text(row.selected?.manifest.displayName ?? row.package).font(.title2.weight(.semibold))
        Text("插件故障详情 · " + row.version).foregroundStyle(.secondary)
        VStack(alignment:.leading,spacing:6) {
          Text(active ? "已重新启用" : checked ? "检查通过，等待手动启用" : "已自动停用，避免继续出错").font(.headline)
          Text(active ? "当前版本已成功激活。" : checked ? "版本内容和依赖已核对。" : extensionReason(fault?.error ?? "extension_quarantined")).foregroundStyle(.secondary)
        }.frame(maxWidth:.infinity,alignment:.leading).padding(16)
          .background((active || checked ? store.themeTint : Color.orange).opacity(0.10),in:RoundedRectangle(cornerRadius:10))
        VStack(spacing:0) {
          line("故障编号",fault.map{"#"+String($0.id)} ?? "记录暂不可用");Divider()
          line("发生时间",fault.map{Date(timeIntervalSince1970:Double($0.created)).formatted()} ?? "未读取");Divider()
          line("影响范围",fault.map{phaseName($0.phase)} ?? "未读取");Divider()
          line("运行状态",active ? "已启用" : "已停用");Divider()
          line("来源",row.source)
        }.padding(.horizontal,16).background(store.themeSurface,in:RoundedRectangle(cornerRadius:10))
        if !error.isEmpty {Text(error).foregroundStyle(.orange).textSelection(.enabled)}
        Text("重新启用会运行这个版本的激活逻辑。已经失败的命令不会自动重试。").font(.callout).foregroundStyle(.secondary)
        HStack {
          Button("返回插件列表",action:close).disabled(working)
          Button("查看错误日志…"){logs=true}.disabled(fault==nil || working).accessibilityIdentifier("plugin.fault.log")
          Spacer()
          if working {ProgressView().controlSize(.small)}
          if !active {
            Button(checked ? "启用此版本…" : "重新检查版本") {
              if checked {confirming=true} else {check()}
            }.buttonStyle(.borderedProminent).disabled(working)
              .accessibilityIdentifier(checked ? "plugin.fault.enable":"plugin.fault.recheck")
          }
        }
      }
    }.padding(24).frame(width:660).interactiveDismissDisabled(working)
      .onAppear(perform:loadFault)
      .alert("启用这个插件版本？",isPresented:$confirming) {
        Button("取消",role:.cancel){}
        Button("信任并启用",action:enable)
      } message:{
        Text(row.package+" · "+row.version+" 将以当前系统用户权限运行，能够使用文件、网络和进程。")
      }
  }
  private func phaseName(_ phase:String)->String {
    switch phase {
    case "command": return "插件命令"
    case "client", "render": return "插件界面"
    case "startup", "activation": return "启动与激活"
    case "recovery": return "上次未结算的调用"
    case "terminal": return "插件终端"
    default: return phase
    }
  }
  private func line(_ title:String,_ detail:String)->some View {
    HStack {Text(title).fontWeight(.medium);Spacer();Text(detail).foregroundStyle(.secondary).textSelection(.enabled)}
      .padding(.vertical,10)
  }
  private func loadFault(){
    store.extensionRequest(["op":"extensions.faults"],as:[ExtensionFault].self){reply in
      fault=reply.data?.first{$0.source==row.source && $0.package==row.package && $0.version==row.version}
      if !reply.ok {error=extensionReason(reply.error ?? "storage_error")}
    }
  }
  private func change(_ op:String,enable:Bool) {
    guard !working else {return}
    working=true;error=""
    var request:[String:Any]=["op":op,"source":row.source,"package":row.package,"version":row.version]
    if enable {request["enabled"]=true;request["acceptHostPermissions"]=true}
    store.extensionRequest(request,as:[ExtensionRow].self){reply in
      working=false
      if reply.ok,let updated=reply.data?.first(where:{$0.id==row.id && $0.version==row.version}) {
        checked=updated.selected?.quarantined==false;active=updated.active
      }else{
        error=extensionReason(reply.error ?? "extension_version_changed")
        checked=false;active=false;loadFault()
      }
      store.refreshThemes()
    }
  }
  private func check(){change("extensions.recover",enable:false)}
  private func enable(){change("extensions.bind",enable:true)}
}
