import AppKit
import SwiftUI
import CDesktop

struct CatalogConfiguration:Decodable {let path:String}
struct CatalogVersion:Decodable,Identifiable {
  var id:String{version}
  let version:String;let digest:String;let installable:Bool;let installed:Bool;let reason:String
  let engines:ExtensionEngines?
  let platforms:[String]?
  let capabilities:[String]?

  private enum CodingKeys:String,CodingKey {case version,digest,installable,installed,reason,engines,platforms,capabilities}
  init(from decoder:Decoder)throws {
    let c=try decoder.container(keyedBy:CodingKeys.self)
    version=try c.decode(String.self,forKey:.version)
    digest=try c.decode(String.self,forKey:.digest)
    installable=try c.decode(Bool.self,forKey:.installable)
    installed=try c.decode(Bool.self,forKey:.installed)
    reason=try c.decode(String.self,forKey:.reason)
    engines=try? c.decode(ExtensionEngines.self,forKey:.engines)
    platforms=try? c.decode([String].self,forKey:.platforms)
    capabilities=try? c.decode([String].self,forKey:.capabilities)
  }
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
  init(scope:String="global",scopeID:String="",initialKind:String="skill",closeMarketplace:(()->Void)?=nil,onTitleChange:((String?)->Void)?=nil){self.onTitleChange=onTitleChange;self.scope=scope;self.scopeID=scopeID;self.closeMarketplace=closeMarketplace;_kind=State(initialValue:initialKind)}
  @State private var query=""
  @State private var title="";@State private var url="";@State private var key=""
  @State private var adding=false;@State private var error="";@State private var status=""
  @State private var busy=false;@State private var cancel:Callbacks?
  @State private var managingSources=false
  @State private var detail:CatalogSelection?
  var onTitleChange:((String?) -> Void)?
  private var environmentTitle:String {
    guard let compatibility else{return "正在检查兼容环境"}
    if compatibility.healthy{return "DSH 兼容环境已就绪"}
    if compatibility.state=="outdated"{return "DSH 兼容环境版本过低"}
    return compatibility.state=="off" ? "DSH 兼容尚未开启":"DSH 兼容环境未就绪"
  }
  var body:some View {
    VStack(alignment:.leading,spacing:20) {
      if let detail, let source=sources.first(where:{$0.id==detail.source}),
         let entry=source.entries.first(where:{$0.id==detail.package}) {
        CatalogPackageDetail(source:source,entry:entry,initialVersion:detail.version,scope:scope,scopeID:scopeID,
          busy:busy,showTitle:onTitleChange == nil,back:{self.detail=nil},
          install:{version,offline,destination in install(source,entry,version,offline:offline,destination:destination)})
      } else {
      if managingSources {
        HStack {if onTitleChange == nil {Text("商店来源").font(.title2.weight(.semibold))};Spacer()
          Button("添加来源…"){adding=true}.accessibilityIdentifier("catalog.add")
          Button("返回商城"){managingSources=false}.accessibilityIdentifier("catalog.back")
        }
      } else {
      HStack {
        Picker("内容",selection:$kind){Text("Skills 商店").tag("skill");Text("插件商城").tag("plugin");Text("主题").tag("theme")}.pickerStyle(.segmented).frame(maxWidth:400)
        Spacer();Button("管理来源"){managingSources=true}.accessibilityIdentifier("catalog.sources")
      }
      TextField("搜索名称、说明或发布者",text:$query).textFieldStyle(.roundedBorder)
      if kind == "plugin" {
        HStack(spacing:20) {
          VStack(alignment:.leading,spacing:6) {
            Label(environmentTitle,systemImage:"puzzlepiece.extension").font(.headline)
            Text(compatibility?.healthy == true ? "支持的插件可安装，未验证的版本请谨慎安装。" : compatibility?.state == "outdated" ? "升级前可查看兼容性风险。完成升级并验证后可安装支持的插件。" : "开启并验证兼容环境后可安装 DSH 插件。Python 插件单独检查解释器。").font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          Button("查看兼容环境") {closeMarketplace?();store.settingsSection="DSH 兼容环境";openSettings()}
        }.padding(18).background((compatibility?.state=="outdated" ? Color.orange : store.themeTint).opacity(0.09),in:RoundedRectangle(cornerRadius:12))
      }

      }
      if managingSources {
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
      }
      if managingSources {
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
          }.padding(12)
        }
      }
      } else {
        let packages = sources.filter(\.enabled).flatMap { source in
          source.entries.compactMap { entry in
            entry.kind == kind && (query.isEmpty || (entry.displayName + (entry.description ?? "") + entry.publisher).localizedCaseInsensitiveContains(query))
              ? CatalogListing(source:source, entry:entry) : nil
          }
        }
        LazyVGrid(columns:kind == "plugin" ? [GridItem(.flexible())] : [GridItem(.flexible(),spacing:16),GridItem(.flexible())],alignment:.leading,spacing:16) {
          ForEach(packages) { item in
            CatalogPackageRow(entry:item.entry,sourceTitle:item.source.title,busy:busy,
              showDetail:{version in detail=CatalogSelection(source:item.source.id,package:item.entry.id,version:version)},
              install:{version,offline in install(item.source,item.entry,version,offline:offline,destination:nil)})
          }
        }
        if packages.isEmpty && !sources.isEmpty && kind != "plugin" {
          ContentUnavailableView(query.isEmpty ? "当前来源尚无此类内容" : "没有找到相关条目",systemImage:"magnifyingglass")
        }
      }
      if sources.isEmpty && (kind != "plugin" || managingSources) {ContentUnavailableView("尚未添加来源",systemImage:"storefront",description:Text("使用来源地址和发布者公钥添加目录，或导入同协议的签名目录文件。"))}
      if !managingSources && kind == "plugin" {DiscoveryView(canInstall:compatibility?.healthy == true)}
      }
      if busy {HStack {ProgressView().controlSize(.small);Text("正在下载并验证…");Spacer();Button("取消"){cancel?.cancel()}}}
      if !status.isEmpty {Text(status).font(.callout).foregroundStyle(.secondary)}
      if !error.isEmpty {Text(error).foregroundStyle(.red).textSelection(.enabled)}
    }.onAppear(perform:reload)
      .onChange(of:pageTitle,initial:true){_,title in onTitleChange?(title)}
      .onChange(of:store.identityEpoch){_,_ in detail=nil;managingSources=false;sources=[];configurationPath="";renaming=nil;compatibility=nil;error="";status="";busy=false;cancel?.cancel();reload()}
      .onChange(of:kind){_,_ in reload()}
      .onChange(of:store.pluginFaultNotice){_,_ in reload()}
      .onDisappear {cancel?.cancel();onTitleChange?(nil)}
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
  private var pageTitle:String {
    if let detail,let source=sources.first(where:{$0.id==detail.source}),
       let entry=source.entries.first(where:{$0.id==detail.package}) {return entry.displayName}
    if managingSources {return "商店来源"}
    return kind=="plugin" ? "插件商城" : kind=="theme" ? "皮肤" : "商店"
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
  private func install(_ source:CatalogSource,_ entry:CatalogEntry,_ version:CatalogVersion,offline:Bool,destination:CatalogDestination?){
    var request:[String:Any]=["op":"catalog.install","source":source.id,"package":entry.packageId,"version":version.version]
    if offline {guard let raw=readJSON(max:12582912) else{return};request["archive"]=raw}
    busy=true;error="";let box=Callbacks();cancel=box
    store.catalogRequest(request,as:NoData.self,cancel:box){r in busy=false;cancel=nil;if r.ok {
      status="已安装 \(entry.displayName) \(version.version)，内容保存在本地库。"
      if let destination,entry.kind=="skill" {store.workspaceRequest(["op":"skills.bind","scope":destination.scope,"scopeID":destination.id,"source":"catalog:"+source.id,"package":entry.packageId,"version":version.version,"mode":"enable"],as:NoData.self){r in if !r.ok{error=reason(r.error ?? "")}else{status="已安装并启用 "+entry.displayName+" · "+destination.name}}}
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
private struct CatalogDestination:Hashable {
  let scope:String
  let id:String
  let name:String
}
private struct CatalogSelection:Equatable {
  let source:String
  let package:String
  let version:String
}
private struct CatalogListing:Identifiable {
  struct ID:Hashable {let source:String;let package:String}
  let source:CatalogSource
  let entry:CatalogEntry
  var id:ID { ID(source:source.id,package:entry.packageId) }
}
private struct CatalogPackageRow:View {
  @EnvironmentObject var store:WorkspaceStore
  let entry:CatalogEntry
  let sourceTitle:String
  let busy:Bool
  let showDetail:(String)->Void
  let install:(CatalogVersion,Bool)->Void
  @State private var version=""
  var selected:CatalogVersion? {entry.versions.first(where:{$0.version==version}) ?? entry.versions.first}
  var body:some View {
    VStack(alignment:.leading,spacing:12) {
      HStack(alignment:.top,spacing:14) {
        Image(systemName:entry.kind == "skill" ? "square.stack.3d.up" : entry.kind == "theme" ? "paintpalette" : "puzzlepiece.extension")
          .font(.title2).foregroundStyle(store.themeTint).frame(width:44,height:44)
          .background(store.themeTint.opacity(0.09),in:RoundedRectangle(cornerRadius:11))
        VStack(alignment:.leading,spacing:5) {
          Text(entry.displayName).font(.headline)
          if let summary=entry.description,!summary.isEmpty {Text(summary).font(.callout).foregroundStyle(.secondary)}
          Text(sourceTitle+" · "+entry.publisher).font(.caption).foregroundStyle(.secondary)
        }
      }
      if let selected {
        if !selected.reason.isEmpty {
          Label(catalogReason(selected.reason),systemImage:"exclamationmark.circle").font(.callout).foregroundStyle(.secondary)
        } else if entry.kind != "skill" {
          Label("未验证的版本请谨慎安装。",systemImage:"info.circle").font(.caption).foregroundStyle(.secondary)
        }
        HStack {
          if entry.versions.count>1 {
            Picker("版本",selection:Binding(get:{self.selected?.version ?? ""},set:{version=$0})) {
              ForEach(entry.versions){Text($0.version).tag($0.version)}
            }.labelsHidden().fixedSize()
          } else {Text(selected.version).font(.caption.monospacedDigit()).foregroundStyle(.secondary)}
          Spacer()
          Button(entry.kind=="skill" ? "查看并安装":"详情…"){showDetail(selected.version)}.accessibilityIdentifier("catalog.detail."+entry.packageId)
          if entry.kind != "skill" {
            Menu(selected.installed ? "已安装":"安装") {Button("下载并安装"){install(selected,false)};Button("从文件验证安装…"){install(selected,true)}}.disabled(busy || !selected.installable)
          }
        }
      }
    }.padding(20).frame(maxWidth:.infinity,minHeight:entry.kind=="plugin" ? 95:140,alignment:.topLeading)
      .background(store.themeSurface,in:RoundedRectangle(cornerRadius:12))
  }
}

private struct CatalogPackageDetail:View {
  @EnvironmentObject var store:WorkspaceStore
  let source:CatalogSource
  let entry:CatalogEntry
  let initialVersion:String
  let scope:String
  let scopeID:String
  let busy:Bool
  let showTitle:Bool
  let back:()->Void
  let install:(CatalogVersion,Bool,CatalogDestination?)->Void
  @State private var version=""
  @State private var destination:CatalogDestination?
  private var destinations:[CatalogDestination] {
    var choices=[CatalogDestination(scope:"global",id:"",name:"全局启用")]
    choices += store.projects.map{CatalogDestination(scope:"project",id:$0.id,name:"项目 · "+$0.name)}
    if let chat=store.selectedChat {choices.append(CatalogDestination(scope:"chat",id:chat.id,name:"聊天 · "+chat.title))}
    return choices
  }
  private var selected:CatalogVersion? {entry.versions.first{$0.version==(version.isEmpty ? initialVersion:version)}}
  var body:some View {
    VStack(alignment:.leading,spacing:28) {
      if showTitle {Text(entry.displayName).font(.title.weight(.semibold));Divider()}
      if let selected {
        if entry.kind=="skill" {
          VStack(alignment:.leading,spacing:10) {
            Text(entry.description ?? entry.displayName).font(.headline)
            Text(source.title+" · "+entry.publisher+" · "+entry.license).foregroundStyle(.secondary)
          }.frame(maxWidth:.infinity,alignment:.leading).padding(22).background(store.themeSurface,in:RoundedRectangle(cornerRadius:12))
          HStack(spacing:24) {
            VStack(alignment:.leading) {
              Text("安装到").font(.callout)
              Picker("安装到",selection:$destination){
                Text("仅下载到本地库").tag(nil as CatalogDestination?)
                ForEach(destinations,id:\.self){Text($0.name).tag(Optional($0))}
              }.labelsHidden()
            }
            VStack(alignment:.leading) {
              Text("版本").font(.callout)
              Picker("版本",selection:Binding(get:{selected.version},set:{version=$0})){
                ForEach(entry.versions){Text($0.version).tag($0.version)}
              }.labelsHidden()
            }
          }
          VStack(alignment:.leading,spacing:12) {
            Text("使用要求").font(.headline)
            if let platforms=selected.platforms {Text("适用平台 · "+platforms.joined(separator:" / "))}
            if let capabilities=selected.capabilities,!capabilities.isEmpty {Text("声明的能力 · "+capabilities.joined(separator:"、"))}
            Text("启用 Skill 后，文件、命令和账号授权仍由权限设置管理。")
            Text(selected.installable ? "来源签名、包摘要和兼容条件会在安装时校验。" : catalogReason(selected.reason))
            Text("SHA-256 · "+selected.digest).font(.caption.monospaced()).textSelection(.enabled)
          }.font(.callout).frame(maxWidth:.infinity,alignment:.leading).padding(22).background(store.themeSurface,in:RoundedRectangle(cornerRadius:12))
        } else {
          let gate=selected.reason
          let blocked = ["dsh_version_unsupported","extension_service_unsupported","extension_host_unsupported","platform_or_architecture_unsupported","unknown_dunlin_abi"].contains(gate)
          VStack(alignment:.leading,spacing:10) {
            Text(blocked ? "当前不兼容" : (selected.installable || gate == "dsh_environment_not_ready") ? "支持的适配档位" : "安装条件尚未满足").font(.headline)
            Text(entry.description ?? entry.displayName)
          }.padding(20).frame(maxWidth:.infinity,alignment:.leading)
            .background((blocked ? Color.orange:store.themeTint).opacity(0.10),in:RoundedRectangle(cornerRadius:12))
          VStack(spacing:0){
            detailRow("插件类型",selected.engines?.host=="node-dsh" ? "DSH / TypeScript":selected.engines?.host ?? "未声明")
            Divider()
            detailRow("兼容基线",selected.engines?.dsh ?? selected.engines?.dunlin ?? "未声明")
            Divider()
            detailRow("能力检查",selected.installable ? "符合当前安装条件":catalogReason(gate))
          }.padding(.horizontal,20).background(store.themeSurface)
          VStack(alignment:.leading,spacing:12) {
            Text(source.title+" · "+entry.publisher+" · "+entry.license)
            Text("安装在商城中进行，实际启用时检查运行环境与依赖。")
            Text("SHA-256 · "+selected.digest).font(.caption.monospaced()).textSelection(.enabled)
          }.foregroundStyle(.secondary)
        }
        Spacer(minLength:20)
        HStack {
          if entry.kind == "skill" {Button("返回商城",action:back).accessibilityIdentifier("catalog.detail.back")}
          Spacer()
          if entry.kind != "skill" {Button("返回商城",action:back).accessibilityIdentifier("catalog.detail.back")}
          if entry.kind=="skill" {
            Button("从文件安装…"){install(selected,true,destination)}.disabled(busy || !selected.installable)
            Button(destination==nil ? "下载到本地库" : "下载并启用"){install(selected,false,destination)}
              .buttonStyle(.borderedProminent).disabled(busy || !selected.installable)
          }
        }
      }
    }.frame(minHeight:460,alignment:.topLeading)
      .onAppear{destination=scope=="global" ? nil : destinations.first{$0.scope==scope && $0.id==scopeID}}
  }
  private func detailRow(_ title:String,_ value:String)->some View {
    HStack{Text(title).fontWeight(.medium);Spacer();Text(value).foregroundStyle(.secondary)}
      .frame(minHeight:58)
  }
}
