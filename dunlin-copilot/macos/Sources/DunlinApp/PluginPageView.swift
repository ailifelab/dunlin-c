import SwiftUI
struct ContributionAction: Decodable,Identifiable {let id:String;let title:String;let command:String}
struct ContributionSection: Decodable,Identifiable {let id:String;let title:String;let body:String}
struct PluginPage: Identifiable {
  var id:String {contribution.id}
  let source:String,package:String,version:String
  let contribution:NativeContribution
}
extension WorkspaceStore {
  func refreshPluginPages() {
    extensionRequest(["op":"extensions.clients"],as:[PluginClient].self) {self.pluginClients=$0.ok ? ($0.data ?? []) : []}
    extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self) {inventory in
      guard inventory.ok,let installed=inventory.data else {self.pluginPages=[];return}
      self.extensionRequest(["op":"extensions.contributions"],as:[NativeContribution].self) {result in
        guard result.ok,let contributions=result.data else {self.pluginPages=[];return}
        self.pluginPages=contributions.compactMap {item in
          guard ["sidebar","settings"].contains(item.kind),let owner=installed.first(where:{$0.active && $0.runtimeID==item.plugin}) else {return nil}
          return PluginPage(source:owner.source,package:owner.package,version:owner.version,contribution:item)
        } + installed.filter{$0.active && ($0.selected?.manifest.client?.slots.contains("dunlin.workbench") ?? false)}.map {owner in
          PluginPage(source:owner.source,package:owner.package,version:owner.version,contribution:NativeContribution(id:"react:"+owner.runtimeID,plugin:owner.runtimeID,kind:"sidebar",title:pluginWorkbenchTitle(owner.package),command:nil,name:nil,scheme:nil,tokens:nil,body:nil,actions:nil,sections:nil))
        }
      }
    }
  }
}
struct PluginPageView: View {
  @EnvironmentObject var store: WorkspaceStore
  let pageID:String
  @State private var command:ExtensionCommandSelection?
  var body: some View {
    Group {
      if let client=store.pluginClients.first(where:{"react:"+$0.runtimeID==pageID}) {
        PluginWorkbenchView(client:client)
      } else if let page=store.pluginPages.first(where:{$0.id==pageID}) {
        ScrollView {VStack(alignment:.leading,spacing:24) {
          Text(page.contribution.title ?? page.package).font(.title2.bold())
          Text("\(page.package) · \(page.version)").font(.callout).foregroundStyle(.secondary)
          Divider()
          if let body=page.contribution.body {Text(body).textSelection(.enabled)}
          ForEach(page.contribution.sections ?? []) {section in
            GroupBox(section.title) {Text(section.body).frame(maxWidth:.infinity,alignment:.leading).textSelection(.enabled).padding(12)}
          }
          HStack {
            ForEach(page.contribution.actions ?? []) {action in
              Button(action.title) {command=ExtensionCommandSelection(source:page.source,package:page.package,command:action.command,title:action.title)}
            }
          }
        }.padding(30).frame(maxWidth:.infinity,alignment:.leading)}
      } else {ContentUnavailableView("插件页面不可用",systemImage:"puzzlepiece.extension",description:Text("插件可能已停用或发生错误。请在插件设置中查看状态。")).frame(maxWidth:.infinity,maxHeight:.infinity)}
    }.sheet(item:$command,onDismiss:store.refreshPluginPages) {selection in ExtensionCommandSheet(selection:selection)}
  }
}
