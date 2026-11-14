import AppKit
import SwiftUI

struct ContextSourceSelection {
  let record: Int64
  let digests: [String]
}
private struct ContextSource: Decodable, Identifiable {
  let seq: Int, digest: String, text: String
  var id: Int { seq }
  var selectionKey: String { "\(seq):\(digest)" }
}

struct ContextSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var records: [CompactionRecord] = []
  @State private var error = ""
  @State private var reading: CompactionRecord?
  @State private var summary = ""
  @State private var sourceError = ""
  @State private var sources: [ContextSource] = []
  @State private var selectedSources: Set<String> = []
  @State private var choosingSources = false
  @State private var confirmOmitImages = false
  @State private var showBudgetDetails = false
  let openStrategies: () -> Void
  @Environment(\.dismiss) private var dismiss
  var body: some View {
    VStack(alignment:.leading,spacing:24) {
      VStack(alignment: .leading, spacing: 0) {
        option("自动压缩", "按当前模型的输入预算触发，原始聊天继续保留。") {
          Toggle("自动压缩", isOn: $store.contextPreferences.automatic).labelsHidden()
            .accessibilityIdentifier("context.automatic")
        }
        Divider()
        option("默认策略", "作为未单独配置整理策略的模型的默认选项。") {
          Picker("默认策略", selection: $store.contextPreferences.strategy) {
            if let ref = store.contextPreferences.external { Text(ref["package"] ?? "外部策略").tag("external") }
            Text("语义摘要 + 近期原文").tag("semantic")
            Text("完整轮次保留 · 本地 C").tag("recent")
          }.labelsHidden().accessibilityIdentifier("context.strategy")
        }
        Divider()
        if store.contextPreferences.strategy == "semantic" {
          option("摘要使用的模型", "需要一次额外模型请求，可能产生费用。") {
            Text("跟随当前模型").foregroundStyle(.secondary)
          }
        } else {
          option("整理方式", store.contextPreferences.strategy == "recent"
            ? "保留最近完整回合，较早原文仍可检索。" : "由已启用的策略插件处理选定的较早原文。") {
            Text(store.contextPreferences.strategy == "recent" ? "本地 C" : store.contextPreferences.external?["package"] ?? "外部策略")
              .foregroundStyle(.secondary)
          }
        }
        Divider()
        option("触发与保留", "保留近期完整轮次和工具配对，当前回合与系统指令始终保留。") {
          Button(showBudgetDetails ? "收起参数" : "预算 \(Int((store.contextPreferences.threshold * 100).rounded()))% · \(store.contextPreferences.retainTurns) 轮") {
            showBudgetDetails.toggle()
          }.accessibilityIdentifier("context.budget.details")
        }
        if showBudgetDetails {
          VStack(alignment: .leading, spacing: 12) {
            Slider(value: $store.contextPreferences.threshold, in: 0.25...0.95, step: 0.05) {
              Text("输入预算的 \(Int((store.contextPreferences.threshold * 100).rounded()))%")
            }
            Stepper("保留最近 \(store.contextPreferences.retainTurns) 个完整回合", value: $store.contextPreferences.retainTurns, in: 1...8)
            HStack {
              Text("摘要最大输出 tokens")
              TextField("tokens", value: $store.contextPreferences.summaryOutput, format: .number).frame(width: 100)
                .onChange(of: store.contextPreferences.summaryOutput) { _, value in
                  store.contextPreferences.summaryOutput = min(8192, max(1, value))
                }
            }
            Button("配置模型窗口与上下文预算…") { store.settingsSection = "模型" }
          }.padding([.horizontal,.bottom], 16)
        }
      }.background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      Text("辅助阶段").font(.headline)
      VStack(alignment: .leading, spacing: 0) {
        option("缩短过长工具输出", "保留头尾与来源，完整输出仍可在工具记录中查看。") {
          Toggle("缩短过长工具输出", isOn: Binding(get: { store.contextPreferences.shortenToolOutput == true }, set: { store.contextPreferences.shortenToolOutput = $0 }))
            .labelsHidden().accessibilityIdentifier("context.shorten.tools")
        }
        Divider()
        option("移除历史图片内容", "仅影响后续请求。当前回合的图片与已保存原图继续保留。") {
          Toggle("移除历史图片内容", isOn: Binding(get: { store.contextPreferences.omitHistoryImages == true }, set: { if $0 { confirmOmitImages = true } else { store.contextPreferences.omitHistoryImages = false } }))
            .labelsHidden().accessibilityIdentifier("context.omit.images")
        }
      }.background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      HStack { Spacer(); Button("管理压缩策略…", action: openStrategies)
        .accessibilityIdentifier("context.strategies.open") }
      HStack {Text("当前聊天的整理记录").font(.headline);Spacer();Button("刷新",action:reload)}
      if store.selected == nil {Text("选择一段聊天后查看带来源的整理记录。").foregroundStyle(.secondary)}
      else if records.isEmpty {Text("尚无整理记录。模型需配置窗口上限，达到阈值后才会触发。").foregroundStyle(.secondary)}
      ForEach(records) { row in
        GroupBox {
          VStack(alignment:.leading,spacing:8) {
            HStack {
              Text(row.strategy == "semantic" ? "语义摘要":row.strategy.hasPrefix("external:") ? "外部策略":"近期原文").fontWeight(.semibold)
              Spacer();Text(row.status == "committed" ? "已应用到请求视图":row.status == "cancelled" ? "已取消整理":"未应用").foregroundStyle(.secondary)
            }
            Text("\(row.beforeBytes) → \(row.afterBytes) 字节 · \(row.sourceCount) 条来源请求消息").font(.callout)
            Text("输入计量：UTF-8 字节估算；原始聊天未删减。").font(.caption).foregroundStyle(.secondary)
            Text("来源摘要：\(row.sourceHash)").font(.caption2.monospaced()).textSelection(.enabled)
            Button("查看记录与来源…") {
              guard let chat = store.selected else { return }
              reading = row; summary = ""; sourceError = ""; sources=[];choosingSources=false
              selectedSources = store.reincludeSelections[chat].flatMap { $0.record==row.id ? Set($0.digests):nil } ?? []
              store.workspaceRequest(["op":"context.sources","chat":chat,"id":row.id],as:[ContextSource].self){result in
                guard reading?.id==row.id,store.selected==chat else{return}
                sources=result.data ?? []; sourceError = result.ok ? "" : "无法读取这次整理的来源，请返回后重试。"
              }
              store.workspaceRequest(["op":"context.record","chat":chat,"id":row.id],as:String.self) { result in
                guard reading?.id == row.id, store.selected == chat else { return }
                summary = result.ok ? (result.data ?? "") : "无法读取这条记录。"
              }
            }
            if !row.error.isEmpty {Text(row.error).font(.caption).foregroundStyle(.orange)}
          }.padding(10)
        }
      }
      if !error.isEmpty {Text(error).foregroundStyle(.red)}
    }
    .alert("从后续请求中移除历史图片？",isPresented:$confirmOmitImages){
      Button("取消",role:.cancel){}
      Button("启用"){store.contextPreferences.omitHistoryImages=true}
    } message:{Text("之前回合仅发送文字和图片来源，当前回合的图片仍可发送。原图继续保存在聊天中。")}
    .sheet(item: $reading) { row in
      VStack(alignment: .leading, spacing: 18) {
        Text("上下文记录").font(.title2.bold())
        Text([row.strategy == "semantic" ? "语义摘要 + 近期原文" : row.strategy.hasPrefix("external:") ? "外部策略" : "近期原文",
              Date(timeIntervalSince1970: Double(row.created)).formatted(date: .abbreviated, time: .shortened)]
          .joined(separator: " · ")).foregroundStyle(.secondary)
        HStack(spacing: 12) {
          metric(String(row.beforeBytes), "整理前 · 字节")
          metric(String(row.afterBytes), "整理后 · 字节")
          metric(String(row.sourceCount), "来源消息")
        }
        if !sourceError.isEmpty { Text(sourceError).foregroundStyle(.orange) }
        if choosingSources {
          Text("选择下一次请求保留的原文").font(.headline)
          ScrollView { VStack(alignment:.leading,spacing:12){ForEach(sources){item in
            Toggle(isOn:Binding(get:{selectedSources.contains(item.selectionKey)},set:{if $0 {selectedSources.insert(item.selectionKey)}else{selectedSources.remove(item.selectionKey)}})) {
              Text(item.text).lineLimit(6).textSelection(.enabled)
            }.toggleStyle(.checkbox)
          }}}
          HStack {Button("返回摘要"){choosingSources=false};Spacer();Button("应用选择"){
            guard let chat=store.selected else{return}
            store.reincludeSelections[chat]=selectedSources.isEmpty ? nil : ContextSourceSelection(record:row.id,digests:selectedSources.sorted())
            reading=nil
          }.disabled(selectedSources.count>32)}
        } else {
        Text("摘要覆盖的内容").font(.headline)
        ScrollView { Text(summary.isEmpty ? "这条记录没有摘要正文。" : summary).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(14) }
          .frame(maxWidth: .infinity).background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
        Text("摘要可能遗漏细节。可选原文会在下一次模型请求前核对预算。").font(.callout).foregroundStyle(.secondary)
        Text("来源校验 · \(row.sourceCount) 条请求消息").font(.caption)
        Text(row.sourceHash).font(.caption.monospaced()).textSelection(.enabled)
        HStack {
          Button("查看源聊天…") {
            if let id = store.selected, store.openChat(id) { reading = nil; dismiss() }
          }
          Button("重新纳入所选原文…"){choosingSources=true}.disabled(sources.isEmpty)
          Button("更改默认压缩策略…") { reading = nil; openStrategies() }
          Spacer(); Button("完成") { reading = nil }
        }
        }
      }.padding(26).frame(width: 720, height: 520).background(store.themeBackground)
    }
    .onAppear(perform:reload).onChange(of:store.selected){_,_ in reload()}
  }
  private func option<Control: View>(_ title: String, _ subtitle: String, @ViewBuilder control: () -> Control) -> some View {
    HStack(spacing: 20) {
      VStack(alignment: .leading, spacing: 5) {
        Text(title).font(.body.weight(.semibold))
        Text(subtitle).font(.callout).foregroundStyle(.secondary)
      }
      Spacer()
      control()
    }.padding(16).toggleStyle(.switch)
  }
  private func metric(_ value: String, _ label: String) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(value).font(.title2.weight(.semibold)).monospacedDigit()
      Text(label).font(.caption).foregroundStyle(.secondary)
    }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
      .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
  }
  private func reload() {
    reading = nil
    summary = ""
    records = []
    error = ""
    let identity = store.identityEpoch
    guard let id=store.selected else {return}
    store.workspaceRequest(["op":"context.records","chat":id],as:[CompactionRecord].self) { result in
      guard store.identityEpoch == identity, store.selected == id else {return}
      if result.ok {records=result.data ?? [];error=""} else {error=store.workspaceError(result.error)}
    }
  }
}

struct ContextStrategiesView: View {
  @EnvironmentObject var store: WorkspaceStore
  let goBack: () -> Void
  private struct Candidate: Identifiable {
    let url: URL
    let manifest: ExtensionManifest
    var id: URL { url }
  }
  @State private var candidate: Candidate?
  @State private var compatibility: CompatibilityStatus?
  @State private var showModelRequirements = false
  @State private var extensions:[ExtensionRow]=[]
  @State private var issue=""
  private func reloadExtensions(){
    store.extensionRequest(["op":"compat.status"], as: CompatibilityStatus.self) { compatibility = $0.data }
    store.extensionRequest(["op":"extensions.list"],as:[ExtensionRow].self) { result in
      guard result.ok, let rows = result.data else {
        issue = extensionReason(result.error ?? "无法读取策略列表")
        return
      }
      extensions = rows
    }
  }
  private func install(host: String? = nil){
    let panel=NSOpenPanel();panel.canChooseDirectories=true;panel.canChooseFiles=false;panel.allowsMultipleSelection=false
    guard panel.runModal() == .OK,let url=panel.url else{return}
    installDirectory(url, host: host)
  }
  private func installDirectory(_ url: URL, host: String? = nil){
    let file=url.appendingPathComponent("dunlin.json")
    guard let size=try? file.resourceValues(forKeys:[.fileSizeKey]).fileSize, size<=65536,
          let data=try? Data(contentsOf:file),
          let manifest=try? JSONDecoder().decode(ExtensionManifest.self,from:data),
          manifest.contextStrategy == "extractive-v1" else {
      issue="此目录没有受支持的压缩策略声明。"
      return
    }
    if let host, manifest.engines.host != host {
      issue = "请选择声明使用此运行环境的压缩策略。"
      return
    }
    if manifest.engines.host == "node-dsh", compatibility?.healthy != true {
      issue = "需先开启并验证 DSH 兼容环境。"
      return
    }
    candidate=Candidate(url:url,manifest:manifest)
  }
  private func confirmInstall(_ url:URL){
    store.extensionRequest(["op":"extensions.import","path":url.path],as:[ExtensionRow].self){r in
      if r.ok {extensions=r.data ?? [];issue="已安装。请在插件设置中确认权限并启用。"}else{issue=extensionReason(r.error ?? "安装失败")}
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      ForEach(["semantic", "recent"], id: \.self) { id in
        HStack(spacing: 18) {
          VStack(alignment: .leading, spacing: 6) {
            Text(id == "semantic" ? "语义摘要 + 近期原文" : "完整轮次保留").font(.headline)
            Text(id == "semantic" ? "内置 · 保留近期细节，旧历史生成摘要" : "本地 C · 较早轮次不再送入请求")
              .foregroundStyle(.secondary)
          }
          Spacer()
          Button(store.contextPreferences.strategy == id ? "已选用" : "选用") {
            store.contextPreferences.strategy = id
          }.disabled(store.contextPreferences.strategy == id).accessibilityIdentifier("strategy.select." + id)
        }.padding(20).background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      }
      HStack { Text("外部压缩策略").font(.headline);Spacer();Button("从目录安装…"){install()} }
      VStack(alignment: .leading, spacing: 0) {
        HStack(spacing: 18) {
          VStack(alignment: .leading, spacing: 5) {
            Text("DSH 压缩适配").font(.headline)
            Text(compatibility?.healthy == true
              ? "固定版本策略桥 · 选择声明 compress_context 的适配包。"
              : "需先开启并验证 DSH 兼容环境。").foregroundStyle(.secondary)
          }
          Spacer()
          Button("从目录安装…") { install(host: "node-dsh") }
            .disabled(compatibility?.healthy != true).accessibilityIdentifier("strategy.install.dsh")
        }.padding(16)
        Divider()
        HStack(spacing: 18) {
          VStack(alignment: .leading, spacing: 5) {
            Text("LLMLingua-2").font(.headline)
            Text("候选外部策略 · 独立 Python 环境与模型权重").foregroundStyle(.secondary)
          }
          Spacer()
          Button(showModelRequirements ? "收起详情" : "查看详情") { showModelRequirements.toggle() }
            .accessibilityIdentifier("strategy.details.llmlingua")
        }.padding(16)
        if showModelRequirements {
          Text("当前安装包未附带此适配器或权重。部署前需核对代码与权重许可，并验证目标模型对中文、代码和工具记录的保真度。")
            .font(.callout).foregroundStyle(.secondary).padding([.horizontal,.bottom], 16)
        }
      }.background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      if !extensions.contains(where:{$0.package=="dunlin-extractive"}) {
        HStack {
          VStack(alignment:.leading){Text("本地摘录").font(.headline);Text("独立 Python 宿主 · 保留较早消息中的原文片段").foregroundStyle(.secondary)}
          Spacer()
          Button("安装本地摘录"){
            if let url=Bundle.main.resourceURL?.appendingPathComponent("ContextStrategies/extractive"){installDirectory(url)}
          }.accessibilityIdentifier("strategy.install.extractive")
        }
      }
      ForEach(extensions.filter{$0.selected?.manifest.contextStrategy == "extractive-v1"}) { row in
        HStack {
          Text(row.package + " · " + row.version);Spacer()
          if row.active {
            let selected = store.contextPreferences.strategy == "external"
              && store.contextPreferences.external == ["source":row.source,"package":row.package,"version":row.version]
            Button(selected ? "已选用" : "选用外部策略"){
              store.contextPreferences.external=["source":row.source,"package":row.package,"version":row.version]
              store.contextPreferences.strategy="external"
            }.disabled(selected).accessibilityIdentifier("strategy.select." + row.package)
          } else {Button("前往启用"){store.settingsSection="插件"}}
        }
      }
      if !issue.isEmpty {Text(issue).foregroundStyle(.secondary)}
      Text("策略变更在后续请求中生效。已保存的摘要保留来源。").font(.callout).foregroundStyle(.secondary)
      HStack { Spacer(); Button("返回上下文压缩", action: goBack) }
    }.onAppear(perform:reloadExtensions)
      .sheet(item:$candidate){ item in
        VStack(alignment:.leading,spacing:18) {
          Text(item.manifest.displayName ?? item.manifest.packageId).font(.title2.bold())
          Text("外部压缩策略 · "+item.manifest.version).foregroundStyle(.secondary)
          Label("安装后保持停用", systemImage: "info.circle").padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
          Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 16) {
            GridRow { Text("执行环境"); Text(item.manifest.engines.host ?? "未声明") }
            GridRow { Text("包版本"); Text(item.manifest.version) }
            GridRow { Text("内容访问"); Text("当前聊天中选定的较早原文") }
            if !item.manifest.capabilities.isEmpty {
              GridRow { Text("申请能力"); Text(item.manifest.capabilities.joined(separator: "、")) }
            }
            GridRow { Text("安装来源"); Text(item.url.path).lineLimit(3).textSelection(.enabled) }
          }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
            .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
          Text("启用前将单独确认宿主权限。可以在策略列表中切回内置策略。").font(.callout).foregroundStyle(.secondary)
          HStack {Button("取消"){candidate=nil};Spacer();Button("确认安装"){
            candidate=nil;confirmInstall(item.url)
          }.buttonStyle(.borderedProminent)}
        }.padding(24).frame(width:640,height:480).background(store.themeBackground)
      }
  }
}
