import AppKit
import SwiftUI

struct SkillsView: View {
  @EnvironmentObject var store: WorkspaceStore
  let scope: String
  let scopeID: String
  @State private var rows: [SkillRow] = []
  @State private var error = ""
  @State private var detail: SkillVersion?
  @State private var shop=false
  @State private var library=false
  @State private var libraryQuery=""
  @State private var versionReview: VersionReview?
  private struct VersionReview {
    let skill: SkillRow
    let version: String
    let epoch: Int64
  }
  private var configured: [SkillRow] {
    scope == "global" ? rows : rows.filter { $0.origin != "none" || $0.scopeMode != "inherit" }
  }
  var scopeLabel: String { scope == "global" ? "全局" : scope == "project" ? "本项目":"本对话" }
  var body: some View {
    VStack(alignment:.leading,spacing:20) {
      HStack {
        VStack(alignment:.leading,spacing:7) {
          Text("\(scopeLabel) Skills").font(.title3.weight(.semibold))
          Text(scope == "global" ? "当前身份已下载的本地 Skills，下载后需单独启用。":"继承上层配置，可为\(scopeLabel)单独关闭、启用或锁定版本。")
            .font(.callout).foregroundStyle(.secondary)
        }
        Spacer()
        if scope == "global" {
          Button("导入本地 Skills…",action:importLocal).accessibilityIdentifier("skills.import")
        } else {
          Button("从本地仓库选择") { library=true }.accessibilityIdentifier("skills.library")
        }
        Button("浏览商店") { shop=true }.accessibilityIdentifier("skills.shop")
      }
      if configured.isEmpty {
        ContentUnavailableView(scope == "global" ? "本地库还没有 Skills" : "尚未配置 Skills",systemImage:"square.stack.3d.up",description:Text(scope == "global" ? "导入本地目录或浏览商店，下载后可在右侧单独启用。" : "从本地仓库选择已下载的 Skills，或浏览商店。全局项会在这里继承。"))
      }
      ForEach(configured) { skill in
        GroupBox {
          VStack(alignment:.leading,spacing:12) {
            HStack(spacing:14) {
              Image(systemName:"square.stack.3d.up").font(.title2).foregroundStyle(.secondary)
              VStack(alignment:.leading,spacing:6) {
                Text(skill.selected?.name ?? skill.package).font(.headline)
                Text(skill.selected?.description ?? "").font(.caption).foregroundStyle(.secondary).lineLimit(2)
              }
              Spacer()
              Toggle("启用",isOn:Binding(get:{skill.enabled},set:{bind(skill,mode:$0 ? "enable":"disable",version:skill.version)}))
                .labelsHidden().toggleStyle(.switch).disabled(skill.blocked).accessibilityIdentifier("skill.toggle.\(skill.package)")
            }
            HStack {
              if scope != "global" {
                Text(skill.scopeMode == "inherit" ? "继承\(origin(skill.origin))" : (skill.enabled ? "\(scopeLabel)专用" : "\(scopeLabel)关闭"))
                  .foregroundStyle(.secondary)
                Button("恢复继承") { bind(skill,mode:"inherit",version:skill.version) }
                  .disabled(skill.scopeMode == "inherit").accessibilityIdentifier("skill.inherit.\(skill.package)")
              }
              Picker("版本",selection:Binding(get:{skill.version},set:{version in confirmVersion(skill,version:version)})) {
                ForEach(skill.versions,id:\.version) { version in
                  Text(version.version + (version.quarantined ? " · 已隔离":"")).tag(version.version)
                }
              }.fixedSize().accessibilityIdentifier("skill.version.\(skill.package)")
              Spacer()
              Button("更新") { update(skill) }.accessibilityIdentifier("skill.update.\(skill.package)")
              Button("详情") { detail=skill.selected }
            }.font(.caption)
            HStack {
              Text(skill.blocked ? "此版本已隔离，不能启用":"生效来源：\(origin(skill.origin))")
                .foregroundStyle(skill.blocked ? Color.orange:.secondary)
              Spacer()
              Text(skill.source.hasPrefix("catalog:") ? "商店下载" : "本地导入").foregroundStyle(.secondary)
            }.font(.caption)
          }.padding(12)
        }
      }
      if !error.isEmpty {Text(error).foregroundStyle(.red).textSelection(.enabled)}
      Text("Skills 提供操作说明，不增加文件、命令或网络权限。运行中的回合使用启动时冻结的版本。").font(.caption).foregroundStyle(.secondary)
    }.disabled(store.busy).onAppear(perform:reload).onChange(of:store.identityEpoch) {_,_ in
      rows=[];error="";detail=nil;versionReview=nil;library=false;shop=false;reload()
    }
      .alert("切换 Skills 版本？",isPresented:Binding(get:{versionReview != nil},set:{if !$0 {versionReview=nil}}),presenting:versionReview) { review in
        Button("应用版本") {
          guard review.epoch == store.identityEpoch else { return }
          bind(review.skill,mode:review.skill.enabled ? "enable":"disable",version:review.version)
          versionReview=nil
        }
        Button("取消",role:.cancel) { versionReview=nil }
      } message: { review in
        Text("将 \(review.skill.package) 切换到 \(review.version)。" + (scope == "global" ? "仍继承此全局配置的项目和聊天将在下回合使用；显式锁定的范围保留原版本。" : "只更改\(scopeLabel)的绑定，下回合生效，其他范围保留原版本。"))
      }
      .sheet(isPresented:Binding(get:{detail != nil},set:{if !$0 {detail=nil}})) {
        if let detail {
          VStack(alignment:.leading,spacing:18) {
            HStack {Text(detail.name + " · " + detail.version).font(.headline);Spacer();Button("完成"){self.detail=nil}}
            ScrollView {Text(detail.body).font(.system(size:13,design:.monospaced)).textSelection(.enabled).frame(maxWidth:.infinity,alignment:.leading)}
            Text("内容摘要 · \(detail.digest)").font(.caption2.monospaced()).textSelection(.enabled)
          }.padding(24).frame(width:780,height:600)
        }
      }
      .sheet(isPresented:$shop,onDismiss:reload){
        VStack {HStack{Text("商店与来源").font(.title2.weight(.semibold));Spacer();Button("完成"){shop=false}};ScrollView{CatalogView(scope:scope,scopeID:scopeID,closeMarketplace:{shop=false})}}.padding(26).frame(width:950,height:680)
      }
      .sheet(isPresented:$library,onDismiss:reload) {
        VStack(alignment:.leading,spacing:20) {
          HStack { Text("从本地仓库选择").font(.title2.weight(.semibold));Spacer();Button("完成"){library=false} }
          Text("只为\(scopeLabel)应用所选版本，其他范围保持原配置。").foregroundStyle(.secondary)
          HStack {
            TextField("查找已下载的 Skills",text:$libraryQuery).textFieldStyle(.roundedBorder)
            Button("导入本地目录…",action:importLocal)
          }
          ScrollView {
            LazyVStack(alignment:.leading,spacing:14) {
              ForEach(rows.filter { libraryQuery.isEmpty || ($0.package + ($0.selected?.description ?? "")).localizedCaseInsensitiveContains(libraryQuery) }) { skill in
                HStack(spacing:16) {
                  VStack(alignment:.leading,spacing:6) {
                    Text(skill.selected?.name ?? skill.package).font(.headline)
                    Text(skill.selected?.description ?? "").font(.caption).foregroundStyle(.secondary)
                    Text(skill.version + " · " + (skill.source.hasPrefix("catalog:") ? "商店下载" : "本地导入"))
                      .font(.caption).foregroundStyle(.secondary)
                  }.frame(maxWidth:.infinity,alignment:.leading)
                  Button("在\(scopeLabel)启用") {
                    bind(skill,mode:"enable",version:skill.version) { library=false }
                  }.disabled(skill.blocked).accessibilityIdentifier("skill.library.add.\(skill.package)")
                }.padding(16).background(Color.primary.opacity(0.035),in:RoundedRectangle(cornerRadius:10))
              }
              if rows.isEmpty {ContentUnavailableView("本地库还没有 Skills",systemImage:"square.stack.3d.up",description:Text("先导入本地目录或从商店下载。"))}
            }
          }
          if !error.isEmpty {Text(error).foregroundStyle(.red)}
        }.padding(28).frame(width:760,height:560)
      }
  }
  private func origin(_ source:String)->String {
    switch source {case "global":return "全局";case "project":return "项目";case "chat":return "对话";default:return "未启用"}
  }
  private func reload() {
    store.workspaceRequest(["op":"skills.list","scope":scope,"scopeID":scopeID],as:[SkillRow].self) { result in
      if result.ok {rows=result.data ?? [];error=""} else {error=message(result.error)}
    }
  }
  private func message(_ code:String?)->String {
    switch code {
    case "immutable_version_conflict": return "来源的相同版本内容已改变。请在 SKILL.md 中升级版本号，再重新导入；已有版本保留。"
    case "unsafe_or_oversized_skill_snapshot":return "无法导入：请检查 SKILL.md、链接、资源文件大小或目录层级。单文件最多 1 MiB，总量最多 8 MiB。"
    case "version_missing_or_quarantined":return "目标版本不存在或已隔离，原配置保留。"
    case "source_unavailable":return "原始本地目录不可用，已安装版本仍可使用。"
    default:return code ?? "Skills 操作失败"
    }
  }
  private func importLocal() {
    let panel=NSOpenPanel();panel.canChooseDirectories=true;panel.canChooseFiles=false;panel.allowsMultipleSelection=false;panel.prompt="导入 Skills"
    guard panel.runModal() == .OK, let url=panel.url else{return}
    let path=url.hasDirectoryPath ? url.path:url.deletingLastPathComponent().path
    store.workspaceRequest(["op":"skills.import","path":path],as:SkillInstall.self) { result in
      if result.ok {reload()} else {error=message(result.error)}
    }
  }
  private func bind(_ skill:SkillRow,mode:String,version:String,completion:(()->Void)?=nil) {
    store.workspaceRequest(["op":"skills.bind","scope":scope,"scopeID":scopeID,"source":skill.source,"package":skill.package,"mode":mode,"version":version],as:NoData.self) { result in
      if result.ok {reload();completion?()} else {error=message(result.error)}
    }
  }
  private func confirmVersion(_ skill:SkillRow,version:String) {
    guard version != skill.version else{return}
    versionReview=VersionReview(skill:skill,version:version,epoch:store.identityEpoch)
  }
  private func update(_ skill:SkillRow) {
    store.workspaceRequest(["op":"skills.update","scope":scope,"scopeID":scopeID,"source":skill.source,"package":skill.package],as:SkillInstall.self) { result in
      guard result.ok,let installed=result.data else{error=message(result.error);return}
      if installed.version != skill.version {confirmVersion(skill,version:installed.version)}
      reload()
    }
  }
}
struct SkillsSheet: View {
  @Environment(\.dismiss) private var dismiss
  let target: SkillScope
  var body: some View {
    VStack {
      HStack {Spacer();Button("完成"){dismiss()}}
      ScrollView {SkillsView(scope:target.scope,scopeID:target.scopeID)}
    }.padding(24).frame(width:850,height:620)
  }
}
