import AppKit
import SwiftUI
import CDesktop

struct CatalogConfiguration:Decodable {let path:String}
struct CatalogVersion:Decodable,Identifiable {
  var id:String{version}
  let version:String;let digest:String;let installable:Bool;let installed:Bool;let reason:String
}
struct CatalogEntry:Decodable,Identifiable {
  var id:String{packageId}
  let packageId:String;let kind:String;let displayName:String;let description:String?;let publisher:String;let license:String;let versions:[CatalogVersion]
}
struct CatalogSource:Decodable,Identifiable {
  let id:String;let url:String;let title:String;let publicKey:String;let catalogID:String;let revision:Int64;let verifiedAt:Int64;let expiresAt:Int64;let error:String;let enabled:Bool;let entries:[CatalogEntry]
}
extension WorkspaceStore {
  func catalogRequest<T:Decodable>(_ body:[String:Any],as:T.Type,cancel:Callbacks?=nil,completion:@escaping(WorkspaceReply<T>)->Void) {
    guard let handle,let data=try? JSONSerialization.data(withJSONObject:body),let raw=String(data:data,encoding:.utf8) else{return}
    let epoch=identityEpoch
    queue.async {
      let box=cancel ?? Callbacks()
      var hooks=dc_hooks(user:Unmanaged.passUnretained(box).toOpaque(),event:nil,approve:nil,cancelled:{ctx in
        guard let ctx else{return 1};return Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().isCancelled() ? 1:0
      })
      let result=self.decode(dc_catalog_call(handle.raw,raw,&hooks),as:WorkspaceReply<T>.self) ?? WorkspaceReply(ok:false,error:"商店服务未返回有效结果",data:nil)
      DispatchQueue.main.async {if self.identityEpoch==epoch {completion(result)}}
    }
  }
}
struct CatalogView:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  private let closeMarketplace:(()->Void)?
  @State private var compatibility:CompatibilityStatus?
  var scope="global";var scopeID=""
  @State private var sources:[CatalogSource]=[]
  @State private var configurationPath=""
  @State private var renaming:CatalogSource?
  @State private var renamedTitle=""
  @State private var kind="skill"
  init(scope:String="global",scopeID:String="",initialKind:String="skill",closeMarketplace:(()->Void)?=nil){self.scope=scope;self.scopeID=scopeID;self.closeMarketplace=closeMarketplace;_kind=State(initialValue:initialKind)}
  @State private var query=""
  @State private var title="";@State private var url="";@State private var key=""
  @State private var adding=false;@State private var error="";@State private var status=""
  @State private var busy=false;@State private var cancel:Callbacks?
  @State private var enableAfterInstall=false
  var body:some View {
    VStack(alignment:.leading,spacing:20) {
      HStack {
        Picker("内容",selection:$kind){Text("Skills 商店").tag("skill");Text("插件商城").tag("plugin");Text("主题").tag("theme")}.pickerStyle(.segmented).frame(maxWidth:400)
        Spacer();Button("添加来源…"){adding=true}.accessibilityIdentifier("catalog.add")
      }
      TextField("搜索名称、说明或发布者",text:$query).textFieldStyle(.roundedBorder)
      if kind == "plugin",compatibility?.healthy != true {
        HStack(spacing:20) {
          VStack(alignment:.leading,spacing:6) {
            Label(compatibility == nil ? "正在检查兼容环境":"DSH 兼容环境未就绪",systemImage:"puzzlepiece.extension").font(.headline)
            Text("可以浏览目录；DSH 插件需先开启并验证兼容环境才能安装。Python 插件单独检查解释器。").font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          Button("查看兼容环境") {closeMarketplace?();store.settingsSection="DSH 兼容环境";openSettings()}
        }.padding(18).background(store.themeTint.opacity(0.08),in:RoundedRectangle(cornerRadius:12))
      }

      if kind == "plugin" {DiscoveryView(canInstall:compatibility?.healthy == true)}
      GroupBox("本地商店来源") {
        VStack(alignment:.leading,spacing:10) {
          Text("自行添加采用统一 Dunlin Catalog v1 协议的可信目录。来源保存在当前身份的本地配置文件中；修改来源不会自动下载或启用插件。").font(.callout).foregroundStyle(.secondary)
          HStack {
            Text("catalog-sources.json").font(.system(.caption,design:.monospaced))
            Spacer()
            Button("在 Finder 中显示") {NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath:configurationPath)])}.disabled(configurationPath.isEmpty)
            Button("重新读取配置",action:reload).disabled(busy)
          }
        }.frame(maxWidth:.infinity,alignment:.leading).padding(10)
      }
      ForEach(sources){source in
        GroupBox {
          VStack(alignment:.leading,spacing:16){
            HStack {
              VStack(alignment:.leading,spacing:5){Text(source.title).font(.headline);Text(source.url).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)}
              Spacer()
              Button("重命名…") {renamedTitle=source.title;renaming=source}
              if source.enabled {
                Button("刷新"){refresh(source)};Button("导入目录…"){importCatalog(source)};Button("停用来源"){changeSource(source,false)}
              } else {Button("恢复来源"){changeSource(source,true)}}
            }
            HStack {
              Text(source.revision<0 ? "尚未下载目录":"已验签 · 修订 \(source.revision)")
              if source.expiresAt>0 {Text("有效期至 \(Date(timeIntervalSince1970:Double(source.expiresAt)).formatted(date:.abbreviated,time:.shortened))")}
            }.font(.caption).foregroundStyle(.secondary)
            if !source.error.isEmpty {Label(reason(source.error),systemImage:"exclamationmark.triangle").font(.caption).foregroundStyle(.orange)}
            ForEach(source.entries.filter{$0.kind==kind && (query.isEmpty || ($0.displayName+($0.description ?? "")+$0.publisher).localizedCaseInsensitiveContains(query))}){entry in
              Divider()
              CatalogPackageRow(source:source,entry:entry,busy:busy,install:{version,offline in install(source,entry,version,offline:offline)})
            }
          }.padding(12)
        }
      }
      if sources.isEmpty {ContentUnavailableView("尚未添加来源",systemImage:"storefront",description:Text("使用来源地址和发布者公钥添加目录，或导入同协议的签名目录文件。"))}
      if scope != "global" {Toggle("安装后为本\(scope == "project" ? "项目":"对话")启用 Skill",isOn:$enableAfterInstall)}
      if busy {HStack {ProgressView().controlSize(.small);Text("正在下载并验证…");Spacer();Button("取消"){cancel?.cancel()}}}
      if !status.isEmpty {Text(status).font(.callout).foregroundStyle(.secondary)}
      if !error.isEmpty {Text(error).foregroundStyle(.red).textSelection(.enabled)}
    }.onAppear(perform:reload).onChange(of:store.identityEpoch){_,_ in sources=[];configurationPath="";renaming=nil;compatibility=nil;error="";status="";busy=false;cancel?.cancel();reload()}
      .onChange(of:kind){_,_ in reload()}
      .onChange(of:store.pluginFaultNotice){_,_ in reload()}
      .onDisappear {cancel?.cancel()}
      .sheet(item:$renaming){source in
        VStack(alignment:.leading,spacing:18) {
          Text("重命名来源").font(.title2.weight(.semibold))
          TextField("来源名称",text:$renamedTitle)
          Text("目录地址和公钥保持不变。如需更换发布者，请添加新的来源。").font(.caption).foregroundStyle(.secondary)
          HStack {Spacer();Button("取消"){renaming=nil};Button("保存") {
            store.catalogRequest(["op":"catalog.rename","id":source.id,"title":renamedTitle],as:[CatalogSource].self){r in if r.ok {sources=r.data ?? [];renaming=nil;error=""}else{error=reason(r.error ?? "")}}
          }.disabled(renamedTitle.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty)}
        }.padding(24).frame(width:500)
      }
      .sheet(isPresented:$adding){
        VStack(alignment:.leading,spacing:20){
          Text("添加商店来源").font(.title2.weight(.semibold))
          Form {TextField("名称",text:$title);TextField("HTTPS 目录地址",text:$url);TextField("Ed25519 公钥（64 位十六进制）",text:$key)}
          Text("请从发布者的可信渠道核对公钥。信任来源只允许验证和浏览目录，插件运行权限仍需单独准入。").font(.callout).foregroundStyle(.secondary)
          HStack {Spacer();Button("取消"){adding=false};Button("信任并添加"){add()}.buttonStyle(.borderedProminent).disabled(title.isEmpty||url.isEmpty||key.count != 64)}
        }.padding(28).frame(width:650)
      }
  }
  private func reload(){store.catalogRequest(["op":"catalog.config"],as:CatalogConfiguration.self){r in configurationPath=r.data?.path ?? ""};store.extensionRequest(["op":"compat.status"],as:CompatibilityStatus.self){r in compatibility=r.data};store.catalogRequest(["op":"catalog.list"],as:[CatalogSource].self){r in if r.ok {sources=r.data ?? [];error=""} else {sources=[];error=reason(r.error ?? "")}}}
  private func add(){store.catalogRequest(["op":"catalog.add","id":UUID().uuidString,"title":title,"url":url,"publicKey":key.lowercased()],as:[CatalogSource].self){r in if r.ok {sources=r.data ?? [];adding=false;title="";url="";key="";error=""}else{error=reason(r.error ?? "")}}}
  private func refresh(_ source:CatalogSource){
    busy=true;error="";let box=Callbacks();cancel=box
    store.catalogRequest(["op":"catalog.refresh","id":source.id],as:[CatalogSource].self,cancel:box){r in busy=false;cancel=nil;if r.ok {sources=r.data ?? [];status="目录已验证"}else{error=reason(r.error ?? "");reload()}}
  }
  private func changeSource(_ source:CatalogSource,_ enabled:Bool){store.catalogRequest(["op":enabled ? "catalog.restore":"catalog.remove","id":source.id],as:[CatalogSource].self){r in if r.ok {sources=r.data ?? []}else{error=reason(r.error ?? "")}}}
  private func readJSON(max:Int)->String? {
    let panel=NSOpenPanel();panel.canChooseDirectories=false;panel.canChooseFiles=true;panel.allowsMultipleSelection=false
    guard panel.runModal() == .OK,let url=panel.url else{return nil}
    do {let size=(try url.resourceValues(forKeys:[.fileSizeKey])).fileSize ?? Int.max;guard size<=max else{error="文件超过允许大小";return nil};return try String(contentsOf:url,encoding:.utf8)}catch{self.error=error.localizedDescription;return nil}
  }
  private func importCatalog(_ source:CatalogSource){guard let raw=readJSON(max:4194304) else{return};store.catalogRequest(["op":"catalog.verify","id":source.id,"envelope":raw],as:[CatalogSource].self){r in if r.ok {sources=r.data ?? [];error=""}else{error=reason(r.error ?? "")}}}
  private func install(_ source:CatalogSource,_ entry:CatalogEntry,_ version:CatalogVersion,offline:Bool){
    var request:[String:Any]=["op":"catalog.install","source":source.id,"package":entry.packageId,"version":version.version]
    if offline {guard let raw=readJSON(max:12582912) else{return};request["archive"]=raw}
    busy=true;error="";let box=Callbacks();cancel=box
    store.catalogRequest(request,as:NoData.self,cancel:box){r in busy=false;cancel=nil;if r.ok {
      status="已安装 \(entry.displayName) \(version.version)，内容保存在本地库。"
      if enableAfterInstall&&entry.kind=="skill" {store.workspaceRequest(["op":"skills.bind","scope":scope,"scopeID":scopeID,"source":"catalog:"+source.id,"package":entry.packageId,"version":version.version,"mode":"enable"],as:NoData.self){r in if !r.ok{error=reason(r.error ?? "")}}}
      reload()
    }else{error=reason(r.error ?? "")}}
  }
  private func reason(_ code:String)->String {catalogReason(code)}
}
func catalogReason(_ code:String)->String {
  switch code {
  case "source_config_invalid":return "来源配置文件格式无效或超过大小限制，请修正后重新读取；文件未被覆盖"
  case "source_config_unreadable":return "来源配置文件无法读取，请检查权限或文件类型"
  case "source_identity_changed":return "已有来源的地址或公钥已改变，请恢复原值，或使用新标识添加来源"
  case "source_config_conflict":return "来源标识或地址存在冲突，请检查配置文件"
  case "source_config_write_failed":return "来源配置未能写入，修改未保存"
  case "source_config_commit_pending","source_config_durability_unknown":return "配置写入结果需要核对，请重新读取配置后确认"
  case "dsh_environment_not_ready":return "DSH 兼容环境尚未就绪，请先在设置中开启"
  case "dsh_version_unsupported":return "插件要求的 DSH 版本不受当前兼容环境支持"
  case "python_environment_not_ready":return "请先配置并验证此插件需要的 Python 环境"
  case "extension_service_unsupported":return "此插件需要的服务接口尚不兼容"
  case "extension_host_unsupported":return "此插件需要的运行环境尚不受支持"
  case "extension_host_not_ready":return "需要兼容环境，当前不能安装"
  case "theme_host_not_ready":return "此主题需要的宿主尚未就绪"
  case "source_disabled":return "来源已停用"
  case "version_withdrawn":return "发布者已撤回此版本，禁止启用"
  case "catalog_expired","cached_catalog_expired","catalog_missing_or_expired","invalid_or_expired_catalog":return "目录无效或已过期，请刷新签名目录"
  case "invalid_signature_or_envelope":return "目录签名未通过，请核对来源公钥"
  case "catalog_revision_rollback_or_conflict","immutable_catalog_version_changed":return "来源版本发生回退或相同版本内容被修改，已拒绝更新"
  case "package_digest_mismatch","manifest_contract_mismatch":return "包内容与已签署清单不一致，已拒绝安装"
  case "unsafe_or_incomplete_archive":return "包中有不安全路径、超限文件或不完整内容"
  case "platform_or_architecture_unsupported":return "不支持当前系统或处理器"
  case "dependencies_not_resolved":return "依赖尚未满足"
  case "unknown_dunlin_abi":return "未声明受支持的 Dunlin C 接口版本"
  case "catalog_download_failed","package_download_failed":return "下载未完成，已安装版本保留"
  case "cancelled":return "已取消"
  default:return code
  }
}
private struct CatalogPackageRow:View {
  let source:CatalogSource;let entry:CatalogEntry;let busy:Bool;let install:(CatalogVersion,Bool)->Void
  @State private var version=""
  var selected:CatalogVersion? {entry.versions.first(where:{$0.version==version}) ?? entry.versions.first}
  var body:some View {
    VStack(alignment:.leading,spacing:12){
      HStack {
        VStack(alignment:.leading,spacing:5){Text(entry.displayName).font(.headline);Text(entry.description ?? "").font(.callout).foregroundStyle(.secondary);Text("\(entry.publisher) · \(entry.license)").font(.caption).foregroundStyle(.secondary)}
        Spacer()
        if let selected {
          Picker("版本",selection:Binding(get:{self.selected?.version ?? ""},set:{version=$0})){ForEach(entry.versions){Text($0.version).tag($0.version)}}.fixedSize()
          Menu(selected.installed ? "已安装":"安装") {Button("下载并安装"){install(selected,false)};Button("从文件验证安装…"){install(selected,true)}}.disabled(busy || !selected.installable)
        }
      }
      if let selected,!selected.reason.isEmpty {Label(catalogReason(selected.reason),systemImage:"exclamationmark.circle").font(.caption).foregroundStyle(.orange)}
    }
  }
}
