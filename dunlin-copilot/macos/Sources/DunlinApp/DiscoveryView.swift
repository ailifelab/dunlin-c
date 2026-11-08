import SwiftUI

private struct CommunityEntry:Decodable,Identifiable {
  var id:String {name+url}
  let name:String;let title:String;let description:String;let url:String
}
private struct CommunitySource:Decodable,Identifiable {
  let id:String;let title:String;let url:String;let updatedAt:Int64;let entries:[CommunityEntry]
}
/// Only presents C-owned data-only indexes. Upstream commands never enter execution.
struct DiscoveryView:View {
  @EnvironmentObject var store:WorkspaceStore
  @State private var sources:[CommunitySource]=[]
  @State private var busy=false
  @State private var error=""
  @State private var adding=false
  @State private var title=""
  @State private var url=""
  @State private var query=""
  @State private var cancellation:Callbacks?
  var body:some View {
    GroupBox("社区插件索引") {
      VStack(alignment:.leading,spacing:14) {
        Text("同步 DSHSeek 第三方索引或 Dunlin 在 GitHub 维护的常用清单。仅更新本地列表，不自动安装；上游插件的 Dunlin 兼容性仍待逐项验证。").font(.callout).foregroundStyle(.secondary)
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
                  Label("兼容性待验证 · 仅供发现",systemImage:"exclamationmark.circle").font(.caption).foregroundStyle(.orange)
                }
                Spacer()
                if let link=URL(string:entry.url),link.scheme=="https" {Link("查看项目",destination:link)}
                Button("安装"){}.disabled(true).help("社区索引不是已验签的 Dunlin 安装包；完成适配与验证后才能安装。")
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
      .onChange(of:store.identityEpoch){_,_ in cancellation?.cancel();busy=false;sources=[];error="";reload()}
      .sheet(isPresented:$adding) {
        VStack(alignment:.leading,spacing:16) {
          Text("添加社区索引").font(.title2.bold())
          Text("使用 format: 1、plugins 列表的 HTTPS JSON 地址。只读取名称、说明与项目链接，不执行索引中的命令。").font(.callout).foregroundStyle(.secondary)
          TextField("来源名称",text:$title);TextField("HTTPS JSON 地址",text:$url)
          HStack {Spacer();Button("取消"){adding=false};Button("添加"){adding=false;request(["op":"catalog.community.add","id":UUID().uuidString,"title":title,"url":url]);title="";url=""}.disabled(title.isEmpty || !url.hasPrefix("https://"))}
        }.textFieldStyle(.roundedBorder).padding(24).frame(width:520)
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
