import AppKit
import SwiftUI
import CDesktop

struct CompatibilityStatus:Decodable {
  let enabled:Bool;let state:String;let node:String;let generation:Int64;let error:String;let healthy:Bool;let baseline:String
}
struct ExtensionEngines:Decodable {let host:String?;let dunlin:String?;let dsh:String?}
struct ExtensionClientManifest:Decodable {let slots:[String]}
struct ExtensionManifest:Decodable {
  let contextStrategy:String?;let packageId:String;let version:String;let upstreamVersion:String?;let adapterRevision:Int?;let displayName:String?;let kind:String;let engines:ExtensionEngines;let entry:String?;let client:ExtensionClientManifest?;let capabilities:[String]
}
struct ExtensionVersion:Decodable,Identifiable {
  var id:String{version};var label:String{guard let upstream=manifest.upstreamVersion,let revision=manifest.adapterRevision else{return version};return "\(upstream) · 适配 \(revision)"};let version:String;let digest:String;let manifest:ExtensionManifest;let quarantined:Bool;let reason:String
}
struct ExtensionRow:Decodable,Identifiable {
  var id:String{source+":"+package};let source:String;let package:String;let version:String;let enabled:Bool;let active:Bool;let runtimeID:String;let versions:[ExtensionVersion]
  var selected:ExtensionVersion?{versions.first{$0.version==version}}
}
struct ExtensionFault:Decodable,Identifiable {let id:Int64;let source:String;let package:String;let version:String;let phase:String;let error:String;let created:Int64;let diagnostic:String?}
struct ExtensionRestoreReport:Decodable {let restored:Int;let failed:Int;let errors:[String]}
struct PythonStatus:Decodable {let healthy:Bool;let executable:String}
struct NativeContribution:Decodable,Identifiable {
  let id:String;let plugin:String;let kind:String;let title:String?;let command:String?;let name:String?;let scheme:String?;let tokens:[String:String]?;let body:String?;let actions:[ContributionAction]?;let sections:[ContributionSection]?
}
// Every callback is delivered only to the identity which initiated it.
extension WorkspaceStore {
  func preparePluginTermination(_ completion:@escaping(Bool)->Void){
    guard !busy,!terminating else{completion(false);return}
    terminating=true;extensionHealthTimer?.invalidate();extensionHealthTimer=nil
    guard handle != nil else{queue.async{DispatchQueue.main.async{completion(true)}};return}
    extensionRequest(["op":"extensions.quiesce"],as:JSONValue.self){result in
      if !result.ok {self.terminating=false;self.pluginFaultNotice=result.error ?? "插件尚未确认停止";self.beginExtensionHealthWatch()}
      completion(result.ok)
    }
  }
  func extensionRequest<T:Decodable>(_ body:[String:Any],as:T.Type,cancel:Callbacks?=nil,event:((String)->Void)?=nil,completion:@escaping(WorkspaceReply<T>)->Void){
    if terminating && body["op"] as? String != "extensions.quiesce" {completion(WorkspaceReply(ok:false,error:"application_terminating",data:nil));return}
    guard let handle,let data=try? JSONSerialization.data(withJSONObject:body),let raw=String(data:data,encoding:.utf8) else{return}
    let epoch=identityEpoch,box=cancel ?? Callbacks()
    box.approve={_,text in DispatchQueue.main.sync {
      guard self.identityEpoch==epoch,!box.isCancelled(),let bytes=text.data(using:.utf8),let value=(try? JSONSerialization.jsonObject(with:bytes)) as? [String:Any],let question=value["question"] as? [String:Any],let title=question["question"] as? String,let detail=question["detail"] as? String,let options=question["options"] as? [[String:Any]],options.count==2 else{return 0}
      let alert=NSAlert();alert.messageText="插件操作需要确认";alert.informativeText="\(value["package"] as? String ?? "") · 仅对本次操作有效。请检查下面的变更。"
      let scroll=NSScrollView(frame:NSRect(x:0,y:0,width:620,height:260));scroll.hasVerticalScroller=true;scroll.borderType = .bezelBorder
      let text=NSTextView(frame:scroll.bounds);text.isEditable=false;text.isSelectable=true;text.font = .systemFont(ofSize:13);text.string=title+"\n\n"+detail;text.textContainerInset=NSSize(width:10,height:10);text.isVerticallyResizable=true;text.autoresizingMask = [.width];text.textContainer?.widthTracksTextView=true;scroll.documentView=text;alert.accessoryView=scroll
      alert.addButton(withTitle:options[0]["label"] as? String ?? "仅允许一次");alert.addButton(withTitle:options[1]["label"] as? String ?? "取消");alert.buttons[0].keyEquivalent="";alert.window.defaultButtonCell=nil
      return alert.runModal() == .alertFirstButtonReturn && !box.isCancelled() ? 1:0
    }}
    box.event={ [weak box] topic,text in
      DispatchQueue.main.async{if self.identityEpoch==epoch {event?(text)}}
      if topic == "compat.progress" { box?.waitForDownload() }
    }
    queue.async {
      var hooks=dc_hooks(user:Unmanaged.passUnretained(box).toOpaque(),event:{ctx,topic,text in guard let ctx,let topic,let text else{return};Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().event(String(cString:topic),String(cString:text))},approve:{ctx,name,text in guard let ctx,let name,let text else{return 0};let box=Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue();return box.isCancelled() ? 0:box.approve(String(cString:name),String(cString:text))},cancelled:{ctx in guard let ctx else{return 1};return Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().isCancelled() ? 1:0})
      let result=self.decode(dc_extensions_call(handle.raw,raw,&hooks),as:WorkspaceReply<T>.self) ?? WorkspaceReply(ok:false,error:"扩展服务未返回有效结果",data:nil)
      DispatchQueue.main.async{if self.identityEpoch==epoch {completion(result)}}
    }
  }
  func beginExtensionHealthWatch() {
    guard extensionHealthTimer==nil else{return}
    let timer=Timer(timeInterval:5,repeats:true) {[weak self] timer in
      guard let self else {timer.invalidate();return}
      Task { @MainActor [weak self] in self?.pollExtensionHealth() }
    }
    extensionHealthTimer=timer;RunLoop.main.add(timer,forMode:.common)
    pollExtensionHealth()
  }
  func pollExtensionHealth() {
    startNativePluginJobs()
    if extensionHealthEpoch != identityEpoch {
      extensionHealthEpoch=identityEpoch;extensionHealthReading=false;extensionLastFault=nil;pluginFaultNotice=""
    }
    guard handle != nil,!extensionHealthReading,!(busy && agentWorkers.isEmpty) else{return}
    extensionHealthReading=true
    extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self) {inventory in
      guard inventory.ok else{self.extensionHealthReading=false;return}
      self.extensionRequest(["op":"extensions.faults"],as:[ExtensionFault].self) {result in
        self.extensionHealthReading=false
        guard result.ok,let faults=result.data else{return}
        let newest=faults.first?.id ?? 0
        if let previous=self.extensionLastFault,newest>previous,let fault=faults.first {
          let name=inventory.data?.first{$0.source==fault.source && $0.package==fault.package}?.selected?.manifest.displayName ?? fault.package
          self.pluginFaultNotice="插件“\(name)”发生异常，已停用。"
          self.refreshPluginPages();self.refreshThemes()
        }
        self.extensionLastFault=newest
      }
    }
  }
  func configureExtensionAssets(){
    guard let resources=Bundle.main.resourceURL else{return}
    extensionRequest(["op":"extensions.assets","path":resources.appendingPathComponent("ExtensionAssets").path],as:CompatibilityStatus.self){assets in
      guard assets.ok else {self.beginExtensionHealthWatch();return}
      self.extensionRequest(["op":"extensions.restore"],as:ExtensionRestoreReport.self) {result in
        if !result.ok || (result.data?.failed ?? 0)>0 {self.error="部分扩展未能恢复，请在设置中的插件与兼容环境页面查看状态。"}
        self.refreshThemes();self.beginExtensionHealthWatch()
      }
    }
  }
}
private struct CompatibilityDetection: Decodable { let action: String; let node: String; let baseline: String }
struct CompatibilityView:View {
  @EnvironmentObject var store:WorkspaceStore
  @State private var status:CompatibilityStatus?
  @State private var node=""
  @State private var busy=false;@State private var phase="";@State private var error=""
  @State private var cancel:Callbacks?
  @State private var upgradeFailure: String?
  @State private var received:Int64=0
  @State private var total:Int64=0
  @State private var plugins:[ExtensionRow]=[]
  @State private var shop=false
  @State private var downloadPaused = false
  @State private var preparingUpgrade = false
  @State private var seenPhases: [String] = []
  private var fault: String? {
    guard !busy, status?.healthy != true, upgradeFailure == nil else { return nil }
    if status?.state == "faulted" { return status?.error.isEmpty == false ? status?.error : "compatibility_prepare_failed" }
    return nil
  }
  @State private var detected: CompatibilityDetection?
  @State private var showUpgrade = false
  var body:some View {
    VStack(alignment: .leading, spacing: 18) {
      if let detected {
        notice("这套环境可以直接使用", "版本与服务检查通过，无需重复下载。", tint: store.themeTint)
        VStack(spacing: 0) {
          fact("环境位置", detected.node); Divider()
          fact("兼容基线", detected.baseline); Divider()
          fact("来源与完整性", "已验证")
        }.padding(.horizontal,20).background(store.themeSurface)
        HStack {
          Button("返回") { self.detected = nil }
          Button("选择其他位置…") { if chooseNode() { self.detected = nil; inspect() } }
          Spacer()
          Button("复用并开启") { node = detected.node; self.detected = nil; prepare(false) }
            .buttonStyle(.borderedProminent).accessibilityIdentifier("compat.reuse")
        }
      } else if showUpgrade {
        Text("升级兼容环境").font(.title3.weight(.semibold))
        Text("保留现有环境，准备一套独立的新环境。").foregroundStyle(.secondary)
        VStack(spacing: 0) {
          fact("已选引擎", node.isEmpty ? "本机检测" : node); Divider()
          fact("目标兼容基线", status?.baseline ?? "等待检测"); Divider()
          fact("已安装 DSH 插件", "\(plugins.count) 个")
        }.padding(.horizontal,20).background(store.themeSurface)
        ForEach(plugins) { plugin in
          fact(plugin.selected?.manifest.displayName ?? plugin.package,
               plugin.selected?.reason.isEmpty == false ? extensionReason(plugin.selected?.reason ?? "") : "候选环境仍需验证")
        }
        Text("安装在应用专用目录。升级可能影响依赖和插件数据，切回原环境也不会撤销插件已产生的外部操作。").foregroundStyle(.secondary)
        HStack {
          Button("取消") { showUpgrade = false }.accessibilityIdentifier("compat.upgrade.cancel")
          Spacer()
          Button("升级并验证") { showUpgrade = false; prepare(true) }
            .buttonStyle(.borderedProminent).accessibilityIdentifier("compat.upgrade.confirm")
        }
      } else if let upgradeFailure {
        notice("升级未完成", extensionReason(upgradeFailure), tint: .orange)
        VStack(spacing: 0) {
          fact("原环境状态", status?.healthy == true ? "仍可使用" : label(status?.state ?? "off")); Divider()
          fact("新环境状态", "未启用"); Divider()
          fact("插件配置", "已保留")
        }.padding(.horizontal,20).background(store.themeSurface)
        HStack {
          Button("返回原环境") { self.upgradeFailure = nil }
          Spacer()
          Button("重试升级") { self.upgradeFailure = nil; showUpgrade = true }.buttonStyle(.borderedProminent)
        }
      } else {
    VStack(alignment:.leading,spacing:36) {
      if !busy && fault == nil && status?.state != "outdated" { HStack(spacing:24) {
        VStack(alignment:.leading,spacing:6) {
          Text("启用 DSH 兼容").font(.body.weight(.medium))
          Text(status?.enabled == true ? "停用会停止接收新调用，并保留插件配置。" : "开启后检查现有环境，仅下载缺失的受支持组件。").font(.callout).foregroundStyle(.secondary)
        }
        Spacer()
        Toggle("启用 DSH 兼容",isOn:Binding(get:{status?.enabled == true},set:{if $0 {inspect()}else{disable()}}))
          .labelsHidden().toggleStyle(.switch).disabled(busy || status == nil).accessibilityIdentifier("compat.enabled")
      }.padding(.horizontal,20).frame(minHeight:65).background(store.themeSurface) }
      VStack(alignment: .leading, spacing: 10) {
        Text(busy ? (preparingUpgrade ? "正在升级兼容环境" : label(phase)) :
             fault != nil ? "兼容环境准备失败" :
             status?.healthy == true ? "兼容环境已就绪" : "当前状态：" + label(status?.state ?? "off"))
          .font(.headline.weight(.semibold))
        Text(fault.map(extensionReason) ?? (busy && preparingUpgrade ? "正在验证候选环境，原环境保留。" :
          status?.healthy == true ? "版本与服务检查通过，可安装兼容插件。" : "普通聊天、项目操作和内置功能可以正常使用。"))
          .foregroundStyle(.secondary).textSelection(.enabled)
        if busy {
          HStack {
            ProgressView().controlSize(.small)
            Text(label(phase)).foregroundStyle(.secondary)
            Spacer()
            if phase == "downloading" {
              Button(downloadPaused ? "继续下载" : "暂停") {
                downloadPaused.toggle();cancel?.setDownloadPaused(downloadPaused)
              }.accessibilityIdentifier("compat.download.pause")
            }
            Button(preparingUpgrade ? "取消本次升级" : "取消") { cancel?.cancel() }.disabled(cancel == nil)
          }
          if total > 0 && phase == "downloading" {
            ProgressView(value: Double(received), total: Double(total))
            Text("\(ByteCountFormatter.string(fromByteCount: received, countStyle: .file)) / \(ByteCountFormatter.string(fromByteCount: total, countStyle: .file))").font(.caption)
          }
        }
      }.frame(maxWidth: .infinity, alignment: .leading).padding(20)
        .background((fault != nil || status?.state == "outdated" ? Color.orange : store.themeTint).opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
      if busy && cancel != nil {
        VStack(spacing: 0) {
          ForEach(["probing", "downloading", "verifying", "starting"], id: \.self) { step in
            let finished = seenPhases.contains(step) && phase != step
            let reused = ["downloading", "verifying"].contains(step) && phase == "starting" && !seenPhases.contains(step)
            fact(label(step), phase == step ? (step == "downloading" && downloadPaused ? "已暂停" : "进行中") : finished ? "已完成" : reused ? "复用本机组件" : "等待")
            if step != "starting" { Divider() }
          }
        }.padding(.horizontal, 20).background(store.themeSurface)
      } else if fault != nil {
        VStack(spacing: 0) {
          fact("环境状态", "不可用"); Divider()
          fact("DSH 插件安装", "已暂停"); Divider()
          fact("普通聊天与项目", "可继续使用")
        }.padding(.horizontal, 20).background(store.themeSurface)
      } else if status?.healthy == true {
        VStack(spacing:0) {
          fact("来源",status?.node ?? "");Divider()
          fact("兼容基线",status?.baseline ?? "");Divider()
          fact("版本与服务检查","通过");Divider()
          fact("插件状态","\(plugins.filter{$0.selected?.reason.isEmpty == true}.count) 个可用 · \(plugins.filter{$0.selected?.reason.isEmpty == false}.count) 个受限")
        }.padding(.horizontal,20).background(store.themeSurface)
      } else if status?.state == "outdated" && !busy {
        VStack(spacing: 0) {
          fact("检测结果", "所选引擎不满足兼容基线"); Divider()
          fact("现有环境", "保留"); Divider()
          fact("建议方式", "准备独立的新环境，再切换")
        }.padding(.horizontal,20).background(store.themeSurface)
      } else if !busy {
        VStack(alignment:.leading,spacing:24) {
          Text("启用时会发生什么").font(.headline.weight(.semibold))
          Text("1  检查本机是否已有完整、受支持的兼容环境。")
          Text("2  满足要求时直接复用；缺失组件时自动下载并验证。")
          Text("3  准备完成后允许安装兼容插件，插件仍可单独开关。")
        }.font(.body).padding(.horizontal,20)
      }
      if fault == nil && !error.isEmpty {Label(extensionReason(error),systemImage:"exclamationmark.triangle").foregroundStyle(.orange).textSelection(.enabled)}
      DisclosureGroup("选择引擎与诊断信息") {
        VStack(alignment:.leading,spacing:12) {
          HStack {TextField("Node 路径（留空自动检测，缺失时下载）",text:$node).textFieldStyle(.roundedBorder).accessibilityIdentifier("compat.node");Button("选择…"){chooseNode()}}.disabled(busy)
          Text("检查系统和 Homebrew 的已安装引擎。缺失时下载固定版本到本应用目录；版本不符时先展示兼容风险。系统引擎不会被替换。").font(.caption).foregroundStyle(.secondary)
          if let fault { Text(fault).font(.caption.monospaced()).textSelection(.enabled) }
          if let status,status.generation>0 {LabeledContent("环境代次",value:String(status.generation)).font(.caption)}
        }.padding(.top,12)
      }
      if !busy { HStack {
        if status?.healthy == true {Button("检查环境…"){inspect()}.disabled(busy)}
        if fault != nil { Button("取消启用") { disable() } }
        Spacer()
        if status?.healthy == true {Button("浏览 DSH 插件"){shop=true}.buttonStyle(.borderedProminent)}
        else {Button(fault != nil ? "重试准备" : status?.state == "outdated" ? "查看升级风险…" : status?.enabled == true ? "检测并恢复":"开启并准备兼容环境"){if status?.state == "outdated" {showUpgrade=true}else{inspect()}}.buttonStyle(.borderedProminent).disabled(busy).accessibilityIdentifier("compat.prepare")}
      } }
      Text("每个插件仍单独检查接口、依赖和界面能力；不兼容的包会显示原因。").font(.caption).foregroundStyle(.secondary)
    }
      }
    }.onAppear(perform:reload).onDisappear{cancel?.cancel()}
      .onChange(of:store.pluginFaultNotice){_,_ in reload()}
      .onChange(of:store.identityEpoch){_,_ in cancel?.cancel();cancel=nil;status=nil;plugins=[];node="";busy=false;phase="";error="";upgradeFailure=nil;detected=nil;showUpgrade=false;preparingUpgrade=false;shop=false;reload()}
      .sheet(isPresented:$shop,onDismiss:reload) {
        VStack {HStack {Text("插件商城").font(.title2);Spacer();Button("完成"){shop=false}};ScrollView {CatalogView(initialKind:"plugin",closeMarketplace:{shop=false})}}.padding(26).frame(width:950,height:680)
      }
  }
  private func notice(_ title: String, _ detail: String, tint: Color) -> some View {
    VStack(alignment:.leading,spacing:8) {
      Text(title).font(.headline).foregroundStyle(tint)
      Text(detail).foregroundStyle(.secondary).textSelection(.enabled)
    }.frame(maxWidth:.infinity,alignment:.leading).padding(20)
      .background(tint.opacity(0.08),in:RoundedRectangle(cornerRadius:12))
  }
  private func fact(_ title:String,_ value:String)->some View {
    HStack(spacing:24) {Text(title).font(.body.weight(.medium));Spacer();Text(value).font(.body).foregroundStyle(.secondary).lineLimit(2).textSelection(.enabled)}.frame(minHeight:65)
  }
  private func reload(){store.extensionRequest(["op":"compat.status"],as:CompatibilityStatus.self){r in status=r.data;if let status,node.isEmpty{node=status.node};if let error=r.error{self.error=error}};store.extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self){r in plugins=(r.data ?? []).filter{$0.selected?.manifest.engines.host == "node-dsh"}}}
  private func inspect() {
    guard !busy else { return }
    busy = true; preparingUpgrade = false; error = ""; phase = "probing"; seenPhases = ["probing"]
    received = 0; total = 0
    let box = Callbacks(); cancel = box
    store.extensionRequest(["op":"compat.detect", "node":node], as:CompatibilityDetection.self, cancel:box) { reply in
      busy = false; cancel = nil
      if reply.ok, let candidate = reply.data {
        switch candidate.action {
        case "reuse": detected = candidate
        case "download": prepare(false)
        case "outdated": if status?.healthy == true { showUpgrade = true }; reload()
        default: error = "compatibility_prepare_failed"
        }
      } else {
        error = reply.error == "cancelled" ? "" : reply.error ?? "compatibility_prepare_failed"
        reload()
      }
    }
  }
  private func prepare(_ confirmed:Bool){
    busy=true;downloadPaused=false;preparingUpgrade=confirmed;error="";phase="probing";seenPhases=["probing"];received=0;total=0;let box=Callbacks();cancel=box
    store.extensionRequest(["op":"compat.enable","node":node,"confirmUpgrade":confirmed],as:CompatibilityStatus.self,cancel:box,event:{text in if let data=text.data(using:.utf8),let value=(try? JSONSerialization.jsonObject(with:data)) as? [String:Int64]{received=value["received"] ?? 0;total=value["total"] ?? 0}else if ["probing","downloading","verifying","starting"].contains(text){phase=text;if !seenPhases.contains(text){seenPhases.append(text)}}}){r in
      busy=false;cancel=nil
      if r.ok{status=r.data;error="";store.refreshThemes();reload()}
      else if r.error=="engine_upgrade_confirmation_required" || r.error=="environment_switch_confirmation_required" {showUpgrade=true;reload()}
      else {error=r.error ?? "准备未完成";if error == "cancelled" {error=""};if confirmed && !error.isEmpty {upgradeFailure=error};reload()}
    }
  }
  private func disable(){busy=true;store.extensionRequest(["op":"compat.disable"],as:CompatibilityStatus.self){r in busy=false;status=r.data;error=r.error ?? "";store.refreshThemes();reload()}}
  @discardableResult private func chooseNode()->Bool {let panel=NSOpenPanel();panel.canChooseDirectories=false;panel.canChooseFiles=true;panel.allowsMultipleSelection=false;if panel.runModal() == .OK,let url=panel.url{node=url.path;return true};return false}
  private func label(_ state:String)->String {switch state{case "outdated":return "引擎版本不匹配";case "off":return "未开启";case "stopped":return "等待恢复";case "probing":return "检测环境…";case "downloading":return "下载受支持引擎…";case "verifying":return "验证下载内容…";case "starting":return "验证兼容服务…";case "ready":return "已就绪";case "faulted":return "准备未完成";default:return state}}
}
private struct ExtensionBindingRequest {let row:ExtensionRow;let version:String}
struct ExtensionsView:View {
  var onLogs: () -> Void = {}
  @EnvironmentObject var store:WorkspaceStore
  @State private var rows:[ExtensionRow]=[]
  @State private var error="";@State private var shop=false
  @State private var contributions:[NativeContribution]=[]
  @State private var versionRow:ExtensionRow?
  @State private var configurationRow:ExtensionRow?
  @State private var detailRow: ExtensionRow?
  @State private var faultRow: ExtensionRow?
  @Environment(\.openWindow) private var openWindow
  @State private var requestedBinding:ExtensionBindingRequest?
  @State private var command:ExtensionCommandSelection?
  @State private var python:PythonStatus?;@State private var pythonPath="";@State private var busy=false
  var body:some View {
    VStack(alignment:.leading,spacing:22){
      HStack {Text("已下载插件").font(.headline);Spacer();Button("错误日志",action:onLogs);Menu("添加插件"){Button("打开插件商城"){shop=true};Button("从本地目录导入…",action:importLocal)}}
      if rows.isEmpty {ContentUnavailableView("本地库还没有插件",systemImage:"puzzlepiece.extension",description:Text("DSH 插件需先开启兼容环境。Python 使用独立解释器和同一 C 通信接口。"))}
      ForEach(rows){row in
        GroupBox {
          VStack(alignment:.leading,spacing:12){
            HStack {VStack(alignment:.leading,spacing:5){Text(row.selected?.manifest.displayName ?? row.package).font(.headline);Text(row.selected?.manifest.engines.host ?? "").font(.caption).foregroundStyle(.secondary)};Spacer();Text(row.active ? "已启用":row.enabled ? "等待重新启用":"已停用").font(.caption).foregroundStyle(.secondary);Toggle("启用",isOn:Binding(get:{row.active},set:{bind(row,version:row.version,enable:$0)})).toggleStyle(.switch).labelsHidden().disabled(busy || (!row.active && !(row.selected?.reason.isEmpty ?? false)))}
            Button("兼容详情…") { detailRow = row }.accessibilityIdentifier("plugin.compat." + row.package)
            HStack {Button("版本 · " + (row.selected?.label ?? row.version)) {versionRow=row}.disabled(busy).accessibilityIdentifier("plugin.versions." + row.package);Spacer();Button("配置…"){configurationRow=row}.disabled(busy || row.active).help("先停用插件，再编辑该版本的配置");Button("更新…"){shop=true};Button("卸载…",role:.destructive){uninstall(row)}.disabled(busy);if row.enabled && !row.active && row.selected?.reason.isEmpty == true {Button("重新启用"){bind(row,version:row.version,enable:true)}};if row.selected?.quarantined == true {Button("故障详情…"){faultRow=row}.accessibilityIdentifier("plugin.fault."+row.package)}}
            if let selected=row.selected,!selected.reason.isEmpty {Label(extensionReason(selected.reason),systemImage:"exclamationmark.circle").font(.caption).foregroundStyle(.orange)}
            if row.active && (row.selected?.manifest.client?.slots.contains("dunlin.workbench") ?? false) {Button("打开工作台…"){store.pluginPageID="react:"+row.runtimeID;store.route="plugin"}}
            if row.active && row.package=="dsh-catppuccin" {Button("主题设置…"){store.settingsSection="外观"}}
            let tools=contributions.filter{$0.plugin==row.runtimeID && $0.kind=="command" && !($0.command?.hasPrefix("route:") ?? false) && !($0.command?.hasPrefix("ws:") ?? false)}
            if !tools.isEmpty {DisclosureGroup("插件工具（\(tools.count)）") {
              VStack(alignment:.leading,spacing:8){
                Text("模型调用遵循当前聊天的工具策略，并逐次请求授权。").font(.caption).foregroundStyle(.secondary)
                ForEach(tools){item in Button(item.title ?? item.command ?? "插件工具"){command=ExtensionCommandSelection(source:row.source,package:row.package,command:item.command ?? "",title:item.title ?? row.package,version:row.version,instance:store.pluginClients.first(where:{$0.runtimeID==row.runtimeID})?.instance)}.disabled(!row.active)}
              }.padding(.vertical,8)
            }}
            let diagnostics=contributions.filter{$0.plugin==row.runtimeID && ($0.command?.hasPrefix("route:")==true || $0.command?.hasPrefix("ws:")==true)}
            if !diagnostics.isEmpty {DisclosureGroup("高级兼容诊断") {
              VStack(alignment:.leading,spacing:8){
                Text("用于开发与故障定位；执行前请检查参数和当前会话。").font(.caption).foregroundStyle(.secondary)
                ForEach(diagnostics){item in Button(item.title ?? item.command ?? "兼容接口"){command=ExtensionCommandSelection(source:row.source,package:row.package,command:item.command ?? "",title:item.title ?? row.package,version:row.version,instance:store.pluginClients.first(where:{$0.runtimeID==row.runtimeID})?.instance)}}
              }.padding(.vertical,8)
            }}
            Text("来源：\(row.source)").font(.caption2).foregroundStyle(.secondary).textSelection(.enabled)
          }.padding(12)
        }
      }
      GroupBox("Python 插件环境") {
        VStack(alignment:.leading,spacing:14){HStack {Text(python?.healthy == true ? "已就绪":"未开启");Spacer();if python?.healthy == true {Button("停用"){store.extensionRequest(["op":"python.disable"],as:NoData.self){r in error=r.error ?? "";reload()}}}}
          HStack {TextField("Python 3 解释器绝对路径",text:$pythonPath).textFieldStyle(.roundedBorder);Button("选择…"){let p=NSOpenPanel();p.canChooseDirectories=false;if p.runModal() == .OK,let u=p.url{pythonPath=u.path}};Button("验证并开启"){configurePython()}.disabled(pythonPath.isEmpty||busy)}
          Text("独立进程以当前系统用户运行，能直接访问该用户的文件与网络。解释器不会链接进 C 核心，安装插件时不执行依赖脚本。").font(.caption).foregroundStyle(.secondary)
        }.padding(12)
      }
      if busy {ProgressView()}
      if !error.isEmpty{Text(extensionReason(error)).foregroundStyle(.red).textSelection(.enabled)}
    }.onAppear(perform:reload).onChange(of:store.pluginFaultNotice){_,_ in reload()}.onChange(of:store.identityEpoch){_,_ in rows=[];contributions=[];command=nil;requestedBinding=nil;configurationRow=nil;detailRow=nil;faultRow=nil;versionRow=nil;python=nil;pythonPath="";reload()}
      .sheet(isPresented:$shop,onDismiss:reload){VStack {HStack{Text("插件商城").font(.title2);Spacer();Button("完成"){shop=false}};ScrollView{CatalogView(initialKind:"plugin",closeMarketplace:{shop=false})}}.padding(26).frame(width:950,height:680)}
      .sheet(item:$command,onDismiss:reload){selection in ExtensionCommandSheet(selection:selection)}
      .sheet(item:$faultRow,onDismiss:reload) { row in
        PluginFaultDetail(row:row,close:{faultRow=nil})
      }
      .sheet(item: $detailRow) { row in
        if let current = rows.first(where: { $0.id == row.id }) {
          PluginCompatibilityDetail(row: current, contributions: contributions.filter { $0.plugin == current.runtimeID },
            close: { detailRow = nil }, openSurface: { client, chat in
              detailRow = nil
              if client.slots?.contains("dunlin.workbench") == true {
                store.pluginPageID = "react:" + client.runtimeID
                store.route = "plugin"
              } else if let chat { guard store.openChat(chat) else { return } }
              openWindow(id: "workspace")
            })
        } else { ContentUnavailableView("插件已移除", systemImage: "puzzlepiece.extension") }
      }
      .sheet(item:$versionRow) { row in
        VStack(alignment: .leading, spacing: 18) {
          Text((row.selected?.manifest.displayName ?? row.package) + " · 版本").font(.title2.weight(.semibold))
          Text("当前范围：" + (store.currentIdentity?.name ?? "") + "的插件").foregroundStyle(.secondary)
          ScrollView {
            VStack(spacing: 0) {
              ForEach(row.versions) { candidate in
                HStack {
                  VStack(alignment: .leading, spacing: 4) {
                    Text(candidate.label).font(.headline)
                    Text(candidate.reason.isEmpty ? "已下载" : extensionReason(candidate.reason)).foregroundStyle(.secondary)
                  }
                  Spacer()
                  if row.version == candidate.version { Text("当前版本").foregroundStyle(.secondary) }
                  else {
                    Button("使用此版本") {
                      versionRow=nil
                      bind(row,version:candidate.version,enable:row.active)
                    }.disabled(candidate.quarantined || !candidate.reason.isEmpty)
                      .accessibilityIdentifier("plugin.use." + candidate.version)
                  }
                }.padding(.vertical,12)
                Divider()
              }
            }
          }
          HStack {
            Button("查看可用更新…") {versionRow=nil;DispatchQueue.main.async {shop=true}}
            Spacer()
            Button("完成") {versionRow=nil}
          }
        }.padding(28).frame(width:620,height:420)
      }
      .sheet(item:$configurationRow,onDismiss:reload){row in PluginConfigurationSheet(row:row)}
      .alert("启用插件？",isPresented:Binding(get:{requestedBinding != nil},set:{if !$0 {requestedBinding=nil}}),presenting:requestedBinding){request in
        Button("信任并启用"){bind(request.row,version:request.version,enable:true,confirmed:true)}
        Button("取消",role:.cancel){requestedBinding=nil}
      } message:{request in Text("\(request.row.package) · \(request.version)\n该扩展以当前系统用户的权限独立运行，可直接读写文件、联网和启动进程。仅启用你信任的版本。停用不会撤销已产生的操作。") }
  }
  private func reload(){reloadContributions();store.extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self){r in rows=r.data ?? [];if let error=r.error{self.error=error}};store.extensionRequest(["op":"python.status"],as:PythonStatus.self){r in python=r.data;if let p=r.data,!p.executable.isEmpty{pythonPath=p.executable}}}
  private func reloadContributions(){store.refreshPluginPages();store.extensionRequest(["op":"extensions.contributions"],as:[NativeContribution].self){r in contributions=r.data ?? []}}
  private func importLocal(){let p=NSOpenPanel();p.canChooseDirectories=true;p.canChooseFiles=false;p.allowsMultipleSelection=false;guard p.runModal() == .OK,let url=p.url else{return};store.extensionRequest(["op":"extensions.import","path":url.path],as:[ExtensionRow].self){r in if r.ok {rows=r.data ?? [];error=""}else{error=r.error ?? "导入失败"}}}
  private func configurePython(){busy=true;store.extensionRequest(["op":"python.configure","executable":pythonPath],as:NoData.self){r in busy=false;error=r.error ?? "";reload()}}
  private func uninstall(_ row:ExtensionRow){
    let alert=NSAlert();alert.messageText="卸载“\(row.selected?.manifest.displayName ?? row.package)”？";alert.informativeText="将停用插件并移除所有已下载版本的安装记录及配置。历史错误与执行记录保留；插件已产生的外部效果需要单独处理。";alert.addButton(withTitle:"卸载");alert.addButton(withTitle:"取消");alert.buttons[0].keyEquivalent="";guard alert.runModal() == .alertFirstButtonReturn else{return}
    busy=true;store.extensionRequest(["op":"extensions.uninstall","source":row.source,"package":row.package],as:[ExtensionRow].self){r in busy=false;error=r.error ?? "";reload();store.refreshThemes();store.refreshPluginPages()}
  }
  private func bind(_ row:ExtensionRow,version:String,enable:Bool,confirmed:Bool=false){
    if enable,!confirmed,let selected=row.versions.first(where:{$0.version==version}),selected.manifest.engines.host != "native-theme" {
      requestedBinding=ExtensionBindingRequest(row:row,version:version);return
    }
    busy=true;store.extensionRequest(["op":"extensions.bind","source":row.source,"package":row.package,"version":version,"enabled":enable,"acceptHostPermissions":enable],as:[ExtensionRow].self){r in busy=false;if r.ok {rows=r.data ?? [];error=""}else{error=r.error ?? "操作失败";reload()};reloadContributions();store.refreshThemes()}
  }
}
func extensionReason(_ code:String)->String {
  switch code {
  case "dsh_environment_not_ready":return "DSH 兼容环境尚未就绪，请先手动开启"
  case "python_environment_not_ready":return "需要先配置并验证 Python 环境"
  case "unresolved_extension_effect_unknown":return "存在结果未确认的插件调用，请先核对外部效果；此版本继续停用"
  case "extension_quarantined","plugin_unhandled_error":return "插件异常，已主动停用并记录日志"
  case "extension_integrity_failed","unsafe_extension_directory":return "插件内容不完整或包含不受支持的链接、路径或超限文件"
  case "engine_upgrade_confirmation_required":return "需要确认独立环境版本变更"
  case "compatibility_prepare_failed":return "准备未完成，请检查兼容环境后重试"
  case "engine_archive_invalid":return "下载的组件无法解包，请重新下载"
  case "compatibility_assets_invalid","compatibility_assets_missing":return "兼容组件完整性检查未通过，请重新构建或安装客户端"
  case "runtime_directory_unavailable","compatibility_directory_unavailable":return "无法使用兼容环境目录，请检查目录类型与访问权限后重试"
  case "engine_staging_failed","download_staging_failed":return "无法准备临时文件，请检查磁盘空间与访问权限后重试"
  case "engine_download_or_digest_failed":return "引擎下载或摘要校验失败，原环境保留"
  case "compatibility_handshake_failed","compatibility_version_mismatch":return "候选兼容环境未通过启动检查"
  case "client_surface_failed":return "插件界面发生错误，已停用并记录日志"
  case "host_exited":return "插件运行进程已退出，相关插件已停用；请查看错误日志"
  case "extension_configuration_requires_disable":return "请先停用插件，再编辑配置"
  case "extension_configuration_changed":return "配置已在其他操作中改变，请重新读取"
  case "extension_version_changed":return "插件版本已改变，未执行旧版本的调用"
  case "extension_chat_workspace_unavailable":return "当前聊天工作区不可用，请先检查项目目录"
  case "native_helper_context_budget_exceeded":return "任务说明超过此模型的上下文预算，请缩短说明或选择窗口更大的模型"
  case "native_helper_model_or_effort_invalid":return "标题模型或推理档位已不可用，请重新选择已配置的模型"
  case "native_helper_request_invalid":return "标题任务的模型、说明或所属会话无效，请重新打开任务编辑界面"
  case "native_effect_receipt_unknown","helper_handoff_receipt_unknown":return "本次插件操作的结果未能确认，已停用相关插件并保留日志。请核对实际结果，系统不会自动重试"
  case "environment_shutdown_receipt_unknown","environment_job_revocation_unknown":return "兼容环境关闭后的记录未能确认，相关插件已停用并保留日志。请先检查，系统不会自动重试原操作"
  case "invalid_extension_configuration":return "插件配置必须是有效的 JSON 对象，最大 32 KiB"
  case "extension_client_unsupported":return "此插件使用的界面插槽尚不兼容"
  case "extension_client_invalid":return "插件界面入口声明无效"
  case "extension_activation_failed":return "插件激活失败，已停用并保留错误记录"
  case "dsh_version_unsupported","extension_service_unsupported","extension_host_unsupported":return "此插件需要的宿主或服务尚不兼容"
  case "extension_effect_unknown":return "插件执行结果未知，已隔离宿主；请核对外部效果，系统不会自动重试"
  case "immutable_extension_version_changed":return "相同版本的插件内容已改变，请使用新版本号"
  default:return catalogReason(code)
  }
}

private struct PluginConfiguration:Decodable {let config:JSONValue;let hash:String;let active:Bool}
private struct PluginConfigurationSheet:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  let row:ExtensionRow
  @State private var text="{}"
  @State private var hash=""
  @State private var error=""
  @State private var busy=false
  var body:some View {
    VStack(alignment:.leading,spacing:16){
      Text("插件配置").font(.title2.bold())
      Text("\(row.selected?.manifest.displayName ?? row.package) · \(row.selected?.label ?? row.version)").foregroundStyle(.secondary)
      Text("配置按版本保存在当前身份的本地数据目录。保存不会启用插件；启用时由插件校验具体设置。").font(.callout).foregroundStyle(.secondary)
      TextEditor(text:$text).font(.system(.body, design: .monospaced)).frame(minHeight:300).border(.quaternary).disabled(busy)
      if !error.isEmpty {Text(error).foregroundStyle(.red).textSelection(.enabled)}
      HStack{Button("重新读取",action:load).disabled(busy);Spacer();Button("取消"){dismiss()}.disabled(busy);Button("保存",action:save).buttonStyle(.borderedProminent).disabled(busy || hash.isEmpty)}
    }.padding(26).frame(width:720,height:520).onAppear(perform:load).onChange(of:store.identityEpoch){_,_ in dismiss()}
  }
  private func reference(_ op:String)->[String:Any]{["op":op,"source":row.source,"package":row.package,"version":row.version]}
  private func load(){busy=true;store.extensionRequest(reference("extensions.config.get"),as:PluginConfiguration.self){r in busy=false;if r.ok,let value=r.data {text=value.config.formatted;hash=value.hash;error=value.active ? "请先停用插件。":""}else{hash="";error=extensionReason(r.error ?? "配置读取失败")}}}
  private func save(){
    guard text.utf8.count<=32768,let bytes=text.data(using:.utf8),let config=(try? JSONSerialization.jsonObject(with:bytes)) as? [String:Any] else{error="请输入有效 JSON 对象，最大 32 KiB。";return}
    var body=reference("extensions.config.save");body["config"]=config;body["expectedHash"]=hash;busy=true
    store.extensionRequest(body,as:PluginConfiguration.self){r in busy=false;if r.ok{dismiss()}else{error=extensionReason(r.error ?? "配置保存失败")}}
  }
}

struct PluginCompatibilityDetail: View {
  @EnvironmentObject var store: WorkspaceStore
  let row: ExtensionRow
  let contributions: [NativeContribution]
  let close: () -> Void
  let openSurface: (PluginClient, String?) -> Void

  var body: some View {
    let manifest = row.selected?.manifest
    let slots = manifest?.client?.slots ?? []
    let client = store.pluginClients.first { $0.runtimeID == row.runtimeID && $0.source == row.source && $0.version == row.version }
    let chat = store.selected ?? store.chats.first { !$0.archived }?.id
    VStack(alignment: .leading, spacing: 18) {
      Text("插件兼容详情").font(.title2.bold())
      Text([manifest?.displayName == row.package ? nil : manifest?.displayName, row.package, row.version].compactMap { $0 }.joined(separator: " · ")).foregroundStyle(.secondary)
      Label(row.active ? "插件正在运行" : row.selected?.reason.isEmpty == false ? extensionReason(row.selected?.reason ?? "") : "已停用",
            systemImage: row.active ? "checkmark.circle" : "info.circle")
        .padding(16).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
      ScrollView {
        VStack(alignment: .leading, spacing: 14) {
          GroupBox("界面贡献") {
            VStack(alignment: .leading, spacing: 8) {
              if slots.isEmpty { Text("此版本没有声明客户端插槽。").foregroundStyle(.secondary) }
              ForEach(slots, id: \.self) { slot in Text(slot).font(.system(.body, design: .monospaced)) }
              if slots.contains("conversation.chat.reasoning.body") {
                Text("展开已保存的公开推理时加载扩展。翻译由该插件的服务提供，可以随时切回原文。").foregroundStyle(.secondary)
              }
              ForEach(contributions.filter { ["sidebar","settings","theme"].contains($0.kind) }) { entry in
                Text([entry.kind, entry.title ?? entry.name ?? entry.id].joined(separator: " · "))
              }
            }.frame(maxWidth: .infinity, alignment: .leading).padding(10)
          }
          GroupBox("运行条件") {
            VStack(alignment: .leading, spacing: 8) {
              LabeledContent("宿主", value: manifest?.engines.host ?? "未声明")
              if let baseline = manifest?.engines.dsh { LabeledContent("DSH 基线", value: baseline) }
              if let capabilities = manifest?.capabilities, !capabilities.isEmpty {
                LabeledContent("申请能力", value: capabilities.joined(separator: "、"))
              }
              if let reason = row.selected?.reason, !reason.isEmpty { Text(extensionReason(reason)).foregroundStyle(.orange) }
            }.padding(10).frame(maxWidth: .infinity, alignment: .leading)
          }
          GroupBox("当前版本") {
            LabeledContent("客户端加载", value: client != nil ? "已加载" : (row.active ? "无活动界面" : "未加载")).padding(10).frame(maxWidth: .infinity, alignment: .leading)
          }
        }.textSelection(.enabled)
      }
      HStack {
        Button("返回插件列表", action: close).keyboardShortcut(.cancelAction)
        Spacer()
        if let client, row.active {
          let workbench = slots.contains("dunlin.workbench")
          Button("查看集成界面") { openSurface(client, chat) }
            .buttonStyle(.borderedProminent)
            .disabled(!workbench && (chat == nil || !slots.contains(where: { ["conversation.composer.dock","conversation.chat.reasoning.body"].contains($0) })))
            .accessibilityIdentifier("plugin.compat.open")
        }
      }
    }.padding(28).frame(width: 690, height: 580)
  }
}
