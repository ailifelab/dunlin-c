import SwiftUI

private struct CommunityEntry:Decodable,Identifiable {
  var id:String {name+url}
  let name:String;let title:String;let description:String;let url:String;let compatibilityReason:String?
}
private struct CommunitySource:Decodable,Identifiable {
  let id:String;let title:String;let url:String;let updatedAt:Int64;let entries:[CommunityEntry]
}
/// Only presents C-owned data-only indexes. Upstream commands never enter execution.
struct DiscoveryView:View {
  var canInstall=false
  @EnvironmentObject var store:WorkspaceStore
  @State private var sources:[CommunitySource]=[]
  @State private var busy=false
  @State private var error=""
  @State private var adding=false
  @State private var title=""
  @State private var url=""
  @State private var query=""
  @State private var installing:CommunityEntry?
  @State private var preview:CommunityEntry?
  @State private var version=""
  @State private var versionError=""
  @State private var availableVersions:[String]=[]
  @State private var cancellation:Callbacks?
  var body:some View {
    GroupBox("社区插件索引") {
      VStack(alignment:.leading,spacing:14) {
        Text("同步 DSHSeek 第三方索引或 Dunlin 在 GitHub 维护的常用清单。同步仅更新本地列表。可安装有对应 npm 发布包的社区插件。未验证的版本请谨慎安装，下载后默认停用。").font(.callout).foregroundStyle(.secondary)
        HStack {
          Text("plugin-index-sources.json").font(.system(.caption,design:.monospaced))
          Spacer()
          Button("重新读取",action:reload).disabled(busy)
          Button("添加索引…"){adding=true}.disabled(busy)
        }
        TextField("搜索社区插件",text:$query).textFieldStyle(.roundedBorder)
        if !error.isEmpty {Label(error,systemImage:"exclamationmark.triangle").foregroundStyle(.orange).font(.caption)}
        if busy {HStack {ProgressView().controlSize(.small);Text("正在同步目录…");Button("取消"){cancellation?.cancel()}}}
        ForEach(sources) {source in
          DisclosureGroup {
            if source.entries.isEmpty {Text("尚未同步列表").foregroundStyle(.secondary).padding(.vertical,8)}
            ForEach(source.entries.filter {query.isEmpty || ($0.title+$0.description+$0.name).localizedCaseInsensitiveContains(query)}) {entry in
              HStack(alignment:.top,spacing:12) {
                VStack(alignment:.leading,spacing:5) {
                  Text(entry.title).font(.headline)
                  Text(entry.description).font(.caption).foregroundStyle(.secondary)
                  if let reason=entry.compatibilityReason,!reason.isEmpty { Text("来源说明："+reason).font(.callout).foregroundStyle(.secondary) }
                  Label("未验证的版本请谨慎安装",systemImage:"exclamationmark.circle").font(.caption).foregroundStyle(.orange)
                }
                Spacer()
                if let link=URL(string:entry.url),link.scheme=="https" {Link("查看项目",destination:link)}
                Button("详情…"){preview=entry}
                Button("下载安装…"){chooseVersion(entry)}.disabled(busy || !canInstall).help("从 npm 下载指定版本并验证摘要；启用后才能测试运行兼容性。")
              }.padding(.vertical,8)
              Divider()
            }
          } label: {
            VStack(alignment:.leading,spacing:6) {
              HStack {Text(source.title).font(.headline);Spacer();Button("同步"){request(["op":"catalog.community.sync","id":source.id])}.disabled(busy);Button("移除"){request(["op":"catalog.community.remove","id":source.id])}.disabled(busy)}
              Text(source.url).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
              Text(source.updatedAt==0 ? "尚未同步": "本地缓存 · \(source.entries.count) 项 · \(Date(timeIntervalSince1970:Double(source.updatedAt)).formatted())").font(.caption).foregroundStyle(.secondary)
            }
          }
        }
      }.padding(12)
    }.onAppear(perform:reload).onDisappear{cancellation?.cancel()}
      .onChange(of:store.identityEpoch){_,_ in cancellation?.cancel();busy=false;sources=[];error="";preview=nil;installing=nil;availableVersions=[];version="";reload()}
      .sheet(isPresented:$adding) {
        VStack(alignment:.leading,spacing:16) {
          Text("添加社区索引").font(.title2.bold())
          Text("使用 format: 1、plugins 列表的 HTTPS JSON 地址。只读取名称、说明与项目链接，不执行索引中的命令。").font(.callout).foregroundStyle(.secondary)
          TextField("来源名称",text:$title);TextField("HTTPS JSON 地址",text:$url)
          HStack {Spacer();Button("取消"){adding=false};Button("添加"){adding=false;request(["op":"catalog.community.add","id":UUID().uuidString,"title":title,"url":url]);title="";url=""}.disabled(title.isEmpty || !url.hasPrefix("https://"))}
        }.textFieldStyle(.roundedBorder).padding(24).frame(width:520)
      }
      .sheet(item:$preview) { entry in
        VStack(alignment:.leading,spacing:18) {
          Text(entry.title).font(.title2.weight(.semibold))
          if !entry.description.isEmpty {Text(entry.description)}
          LabeledContent("npm 包名",value:entry.name)
          if let link=URL(string:entry.url),link.scheme=="https" {LabeledContent("来源项目"){Link(link.absoluteString,destination:link)}}
          if let reason=entry.compatibilityReason,!reason.isEmpty {Text(reason).foregroundStyle(.secondary)}
          Label(canInstall ? "未验证的版本请谨慎安装，下载后默认停用。" : "当前兼容环境尚未就绪。",systemImage:"info.circle").font(.callout)
          HStack{Spacer();Button("完成"){preview=nil}.keyboardShortcut(.cancelAction)}
        }.padding(24).frame(width:520)
      }
      .sheet(item:$installing){entry in
        VStack(alignment:.leading,spacing:16){
          Text("下载 \(entry.title)").font(.title2.bold())
          Text("未验证的版本请谨慎安装。系统验证下载摘要并保存到本地，不执行安装脚本；插件兼容性需要启用后测试，可能因缺少服务而被停用。").font(.callout).foregroundStyle(.secondary)
          Picker("已发布版本",selection:$version){Text("选择版本").tag("");ForEach(availableVersions,id:\.self){Text($0).tag($0)}}
          if availableVersions.isEmpty {Text(versionError.isEmpty ? "正在读取 npm 版本…" : versionError).foregroundStyle(.secondary).font(.caption)}
          HStack{Spacer();Button("取消"){installing=nil};Button("下载安装"){install(entry);installing=nil}.disabled(version.isEmpty || !canInstall || busy)}
        }.padding(24).frame(width:520)
      }
  }
  private func chooseVersion(_ entry:CommunityEntry) {
    version="";versionError="";availableVersions=[];installing=entry
    store.catalogRequest(["op":"catalog.npm.versions","repository":entry.url,"package":entry.name],as:[String].self){reply in
      guard installing?.id==entry.id else{return}
      availableVersions=reply.data ?? [];version=availableVersions.first ?? ""
      if !reply.ok {versionError=packageError(reply.error)}
      else if availableVersions.isEmpty {versionError="没有可安装的发布版本。"}
    }
  }
  private func packageError(_ code:String?)->String {
    switch code {
    case "npm_repository_mismatch": return "npm 发布者登记的仓库与索引不同，无法确认包的身份。"
    case "npm_package_or_version_unsupported": return "此索引项没有有效的 npm 包名。"
    case "npm_version_not_found": return "npm 没有发布这个包或版本。"
    default: return "无法读取发布信息：\(code ?? "未知错误")"
    }
  }
  private func install(_ entry:CommunityEntry){
    busy=true;error="";let box=Callbacks();cancellation=box
    store.catalogRequest(["op":"catalog.npm.install","repository":entry.url,"package":entry.name,"version":version],as:[ExtensionRow].self,cancel:box){reply in
      busy=false;cancellation=nil
      error=reply.ok ? "已下载，默认停用。请到设置中的插件页面选择版本并启用。":"安装未完成：\(reply.error ?? "未知错误")"
    }
  }
  private func reload(){request(["op":"catalog.community.list"])}
  private func request(_ body:[String:Any]) {
    guard !busy else{return};busy=true;error="";let box=Callbacks();cancellation=box
    store.catalogRequest(body,as:[CommunitySource].self,cancel:box) {reply in
      busy=false;cancellation=nil
      if reply.ok {sources=reply.data ?? []}
      else {
        if body["op"] as? String == "catalog.community.list" {sources=[]}
        switch reply.error ?? "" {
        case "community_config_invalid":error="本地索引配置无效，未覆盖文件。请检查 plugin-index-sources.json。"
        case "community_source_invalid_or_duplicate":error="来源地址、名称无效或来源已存在。"
        case "community_download_or_format_failed":error="同步失败：网络或索引格式不符合要求，保留已有缓存。"
        case "cancelled":error="已取消同步，保留已有缓存。"
        default:error="索引操作未完成：\(reply.error ?? "未知错误")"
        }
      }
    }
  }
}
