import AppKit
import SwiftUI

struct SettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var editing: ModelProfile?
  @State private var selectedModel: String?
  @State private var modelPage = "budget"
  @State private var editingProvider: ProviderProfile?
  private let sections: [(String, String)] = [
    ("通用", "gearshape"), ("身份与家庭", "person.2"), ("服务商", "network"), ("模型", "cpu"),
    ("上下文压缩", "text.alignleft"), ("导入配置", "square.and.arrow.down"),
    ("Skills", "square.stack.3d.up"), ("插件", "puzzlepiece.extension"),
    ("DSH 兼容环境", "puzzlepiece.extension"), ("商店与来源", "storefront"),
    ("外观", "paintpalette"), ("授权记录", "lock.shield"), ("权限与数据", "externaldrive"),
    ("语音与视频", "phone"), ("记忆与知识库", "brain.head.profile"), ("聊天检索", "magnifyingglass"),
    ("运行轨迹", "point.3.connected.trianglepath.dotted"),
  ]
  var body: some View {
    HStack(spacing: 0) {
      VStack(alignment: .leading, spacing: 20) {
        Text("设置").font(.system(size: 22, weight: .semibold)).padding(.horizontal, 30).padding(
          .top, 24)
        ScrollViewReader { proxy in
          ScrollView {
            VStack(spacing:7) {
              ForEach(sections,id:\.0) {title,_ in settingsSidebarButton(title,id:title)}
              ForEach(store.pluginPages.filter {$0.contribution.kind == "settings"}) {page in
                settingsSidebarButton(page.contribution.title ?? page.package,id:"plugin:"+page.id)
              }
            }.padding(.horizontal,15)
          }.scrollIndicators(.hidden)
            .onChange(of:store.settingsSection) {_,value in proxy.scrollTo(value)}
            .onMoveCommand {direction in
              guard let index=sections.firstIndex(where:{$0.0==store.settingsSection}) else{return}
              if direction == .down,index+1<sections.count {store.settingsSection=sections[index+1].0}
              if direction == .up,index>0 {store.settingsSection=sections[index-1].0}
            }
        }
        Text("Dunlin Copilot").font(.caption).foregroundStyle(.secondary).padding(24)
      }.frame(width: 260).background {SidebarBackdrop().ignoresSafeArea()}
      ScrollView {
        VStack(alignment: .leading, spacing: 28) {
          VStack(alignment:.leading,spacing:14) {
            Text(contentTitle).font(.system(size:24,weight:.semibold))
            Text(subtitle).foregroundStyle(.secondary).font(.system(size:13))
          }
          Divider()
          content.id(store.identityEpoch).padding(.top,4).disabled(
            (store.busy && store.settingsSection != "授权记录") || store.creatingChat || store.switchingModel || store.workspaceTransitioning
          )
          Spacer(minLength: 20)
        }.padding(.leading,55).padding(.trailing,77).padding(.top,16).padding(.bottom,36).frame(maxWidth: .infinity, alignment: .topLeading)
      }.frame(maxWidth: .infinity, maxHeight: .infinity).background(
        store.themeBackground).foregroundStyle(store.themeText)
    }.frame(minWidth: 1040, idealWidth: 1360, minHeight: 720, idealHeight: 830)
    .preferredColorScheme(
      store.themeScheme
    )
    .tint(store.themeTint)
    .environment(\.chatHTMLPalette,store.themeHTMLPalette)
    .sheet(item: $store.configImportPlan) { plan in
      ConfigImportView(plan: plan).environmentObject(store)
    }
    .onChange(of: store.settingsSection) { _,value in if value != "插件" {store.settingsPluginLogs=false};store.saveSettings() }
    .onChange(of: store.theme) { _, _ in store.saveSettings() }
    .onChange(of: store.showReasoning) { _, _ in store.saveSettings() }
    .onChange(of: store.generalPreferences) { _, _ in store.saveSettings() }
    .onChange(of: store.identityEpoch) { _, _ in
      editing = nil
      selectedModel = nil
      editingProvider = nil
    }
    .onChange(of: store.profiles) { _, models in
      if let editing, let saved = models.first(where: { $0.id == editing.id }) {
        self.editing = saved
      }
    }
    .onChange(of: store.knowledgePreferences) { _, _ in store.saveSettings() }
    .onChange(of: store.contextPreferences) { _, _ in store.saveSettings() }
  }
  private func settingsSidebarButton(_ title:String,id:String)->some View {
    Button {store.settingsSection=id} label: {
      Text(title).font(.system(size:13,weight:store.settingsSection==id ? .semibold:.regular))
        .foregroundStyle(store.themeText).frame(maxWidth:.infinity,alignment:.leading)
        .padding(.horizontal,23).frame(height:33)
        .background(store.settingsSection==id ? store.themeTint.opacity(0.16):Color.clear,in:RoundedRectangle(cornerRadius:8))
        .contentShape(Rectangle())
    }.buttonStyle(.plain).id(id).accessibilityAddTraits(store.settingsSection==id ? .isSelected:[])
  }
  private var contentTitle: String {
    if store.settingsSection == "插件",store.settingsPluginLogs {return "错误日志"}
    if store.settingsSection == "模型", let editing {
      return modelPage == "advanced" ? "模型高级设置" : editing.name
    }
    if store.settingsSection == "服务商", let editingProvider {
      return store.providers.contains(where: { $0.id == editingProvider.id }) ? "编辑服务商" : "添加服务商"
    }
    if store.settingsSection.hasPrefix("plugin:"),let page=store.pluginPages.first(where:{"plugin:"+$0.id == store.settingsSection}) {return page.contribution.title ?? page.package}
    return store.settingsSection
  }
  private var subtitle: String {
    switch store.settingsSection {
    case "插件": return store.settingsPluginLogs ? "插件故障 · 来源与处理结果":"管理已下载的插件与独立扩展环境。"
    case "模型":
      if let editing {
        return modelPage == "budget"
          ? "模型配置 · 上下文与输出"
          : modelPage == "advanced" ? "\(editing.name) · 修改默认请求参数" : "连接配置 · 保存后从下一回合生效"
      }
      return "配置用于聊天的模型，切换时保留当前聊天。"
    case "DSH 兼容环境": return "按需开启，检测并准备独立的插件运行环境。"
    case "语音与视频": return "选择设备、AI 服务与通话记录方式。"
    case "授权记录": return "查看授权范围、持续时间和撤销结果。"
    case "通用": return "为日常聊天设置默认行为。"
    case "服务商": return editingProvider == nil ? "管理模型连接，密钥保存在系统钥匙串。" : "连接信息与模型参数分开管理。"
    default: return "管理当前客户端的设置。"
    }
  }
  @ViewBuilder private var content: some View {
    if store.settingsSection.hasPrefix("plugin:") {
      PluginPageView(pageID:String(store.settingsSection.dropFirst(7)))
    } else { switch store.settingsSection {
    case "语音与视频": MediaSettingsView()
    case "身份与家庭": IdentitiesSettings()
    case "插件":
      if store.settingsPluginLogs {PluginFaultLogView(onBack:{store.settingsPluginLogs=false})}
      else {ExtensionsView(onLogs:{store.settingsPluginLogs=true})}
    case "DSH 兼容环境": CompatibilityView()
    case "Skills": SkillsView(scope: "global", scopeID: "")
    case "上下文压缩": ContextSettingsView()
    case "运行轨迹": TrajectorySettingsView()
    case "商店与来源":
      CatalogView()
    case "模型":
      if let editing {
        ModelEditor(profile: editing, page: $modelPage) { self.editing = nil }.id(editing.id)
      } else {
        models.disabled(!store.configurationReady || store.configSaving)
      }
    case "服务商": ProviderSettingsView(editing: $editingProvider)
    case "通用":
      GeneralSettingsView()
    case "外观":
      Form {
        Section("主题") {
          appearanceRow
          Text("系统材质与正文保持清楚的层级。").foregroundStyle(.secondary)
        }
      }.formStyle(.grouped)
    case "导入配置":
      VStack(alignment: .leading, spacing: 18) {
        Text("从静态 JSON／YAML 预览服务商、模型与字段映射。密钥值不会从文件复制，也不会执行脚本或动态 include。").foregroundStyle(
          .secondary)
        Button("选择配置文件…", action: store.importModel).buttonStyle(.borderedProminent)
          .accessibilityIdentifier("config.import")
        Text(store.connectionStatus).font(.callout)
      }
    case "授权记录": GrantSettings()
    case "权限与数据": DataSettingsView()
    case "聊天检索": IndexSettingsView()
    case "记忆与知识库": KnowledgeView()
    default:
      Form {
        Section("DSH 兼容环境") {
          LabeledContent("当前状态", value: "未开启")
          Text("请从左侧选择一个设置项目。").foregroundStyle(.secondary)
        }
      }.formStyle(.grouped)
    }
  }
  }
  private var appearanceRow: some View {
    Picker(
      "外观",
      selection: Binding(
        get: {
          ["system", "light", "dark"].contains(store.theme)
            || store.installedThemes.contains(where: { $0.id == store.theme })
            ? store.theme : "system"
        }, set: { store.theme = $0 })
    ) {
      Text("跟随系统").tag("system")
      Text("科技白").tag("light")
      Text("科技黑").tag("dark")
      ForEach(store.installedThemes) { Text($0.name).tag($0.id) }
    }.accessibilityIdentifier("settings.theme").onAppear { store.refreshThemes() }
  }
  private var models: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack {
        Text("已配置模型").font(.headline)
        Spacer()
        Button {
          modelPage = "connection"
          editing = ModelProfile()
        } label: {
          Label("添加模型…", systemImage: "plus")
        }.buttonStyle(.borderedProminent).accessibilityIdentifier("model.add")
      }
      VStack(spacing: 0) {
        HStack {
          Text("模型").frame(maxWidth: .infinity, alignment: .leading)
          Text("服务商").frame(width: 110)
          Text("上下文预算").frame(width: 110)
          Text("能力状态").frame(width: 110)
        }.font(.caption).foregroundStyle(.secondary).padding(14).background(
          Color.primary.opacity(0.04))
        ForEach(store.profiles) { p in
          Button {
            selectedModel = p.id
            modelPage = "budget"
            editing = p
          } label: {
            HStack(alignment: .center) {
              VStack(alignment: .leading, spacing: 5) {
                Text(p.name).fontWeight(.medium)
                Text(p.model).font(.caption).foregroundStyle(.secondary)
              }.frame(maxWidth: .infinity, alignment: .leading)
              Text(store.providers.first(where: { $0.id == p.providerID })?.name ?? "独立连接").font(
                .callout
              ).lineLimit(1).frame(width: 110)
              Text(p.contextRange.map { "\($0.selected) / \($0.maximum)" } ?? "待配置").font(
                .caption.monospacedDigit()
              ).frame(width: 110)
              Text(
                store.modelIssues.contains(where: { $0.id == p.id })
                  ? "参数待修复"
                  : p.contextSource?.hasPrefix("导入") == true
                    ? "配置声明"
                    : p.contextLimit != nil ? "目录已读取" : p.deploymentLimit != nil ? "手动设置" : "能力待确认"
              ).font(.caption).foregroundStyle(
                .secondary
              ).frame(width: 110)
              Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
            }.padding(14).contentShape(Rectangle())
          }.buttonStyle(.plain).accessibilityIdentifier("model.edit.\(p.id)")
          Divider()
        }
      }.background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 10))
      HStack {
        Text("默认模型")
        Spacer()
        Picker("默认模型", selection: $store.defaultProfileID) {
          ForEach(store.profiles) { Text($0.name).tag($0.id) }
        }.labelsHidden().fixedSize()
      }.padding(.vertical, 10)
      HStack {
        Button {
          store.testConnection(profile: selectedModel)
        } label: {
          Text(store.checkingConnection ? "正在检查…" : "测试连接")
        }.disabled(store.checkingConnection).accessibilityIdentifier("model.test")
        Button("导入配置…", action: store.importModel)
        Spacer()
      }
      if !store.connectionStatus.isEmpty {
        Text(store.connectionStatus).font(.callout).foregroundStyle(.secondary)
          .accessibilityIdentifier("model.connectionStatus")
      }
      Text("模型变更在下一回合生效；运行中的任务保持原配置。").font(.caption).foregroundStyle(.secondary)
    }.onChange(of: store.defaultProfileID) { _, _ in store.saveSettings() }
  }
  private func protocolLabel(_ id: String) -> String {
    switch id {
    case "chat": return "Chat"
    case "responses": return "Responses"
    case "anthropic": return "Messages"
    case "ollama": return "Ollama"
    default: return id
    }
  }
}
struct ModelEditor: View {
  @EnvironmentObject var store: WorkspaceStore
  @Binding var page: String
  let onClose: () -> Void
  @State private var draft: ModelProfile
  @State private var secret = ""
  @State private var originalSecret: String?
  @State private var credentialLoaded = false
  private let original: ModelProfile
  @State private var error = ""
  @State private var ownerEpoch: Int64?
  @State private var warning = ""
  @State private var deploymentVisible = false
  @State private var budgetNotice = ""
  init(profile: ModelProfile, page: Binding<String>, onClose: @escaping () -> Void) {
    original = profile
    _page = page
    self.onClose = onClose
    _draft = State(initialValue: profile)
  }
  private var hasActiveChat: Bool { store.chats.contains { $0.active && $0.model == draft.id } }
  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack(spacing: 18) {
        Button("‹ 模型列表", action: onClose).buttonStyle(.plain)
        Spacer()
        Button("连接配置") { page = "connection" }.disabled(page == "connection")
        Button("高级设置") { page = "advanced" }.disabled(page == "advanced")
        Button("上下文预算") { page = "budget" }.disabled(page == "budget")
      }.font(.callout)
      Form {
        if page == "connection" {
          Section("连接") {
            TextField("名称", text: $draft.name).accessibilityIdentifier("model.name")
            Picker(
              "服务商",
              selection: Binding(
                get: { draft.providerID ?? "" },
                set: { id in
                  if let provider = store.providers.first(where: { $0.id == id }) {
                    draft = provider.applying(to: draft)
                  } else {
                    draft.providerID = nil
                    draft.credentialID = nil
                    draft.credentialRevision = nil
                  }
                  credentialLoaded = false
                  originalSecret = nil
                  secret = ""
                  loadSecret()
                })
            ) {
              Text("独立连接").tag("")
              ForEach(store.providers) { Text($0.name).tag($0.id) }
            }.accessibilityIdentifier("model.provider")
            TextField("服务地址", text: $draft.endpoint).accessibilityIdentifier("model.endpoint")
              .disabled(draft.providerID != nil)
            TextField("模型 ID", text: $draft.model).accessibilityIdentifier("model.id")
            Picker("协议", selection: $draft.protocolName) {
              Text("Chat Completions").tag("chat")
              Text("Responses").tag("responses")
              Text("Anthropic Messages").tag("anthropic")
              Text("Ollama").tag("ollama")
              Text("Gemini").tag("gemini")
            }.accessibilityIdentifier("model.protocol").disabled(draft.providerID != nil)
            if draft.providerID == nil {
              SecureField("API 密钥", text: $secret).accessibilityIdentifier("model.key").disabled(
                !credentialLoaded || draft.authentication == "none")
              if !credentialLoaded { Button("重新读取钥匙串", action: loadSecret) }
            } else {
              Text("连接和凭据由所选服务商管理。").font(.caption).foregroundStyle(.secondary)
            }
            if let reference = draft.keyEnvironment, !reference.isEmpty {
              Text("旧配置密钥引用：\(reference)。本应用不会读取这个环境变量，请单独填写密钥。").font(.caption).foregroundStyle(
                .secondary)
            }
            Picker(
              "认证方式",
              selection: Binding(
                get: { draft.authentication ?? "auto" }, set: { draft.authentication = $0 })
            ) {
              Text("按协议选择").tag("auto")
              Text("Bearer Token").tag("bearer")
              Text("无需认证").tag("none")
            }.disabled(draft.providerID != nil)
          }
        }
        if page == "advanced" {
          Section("输入能力声明") {
            Picker(
              "支持的输入",
              selection: Binding(
                get: {
                  draft.inputModalities == nil
                    ? "unknown"
                    : draft.inputModalities?.contains("image") == true ? "image" : "text"
                },
                set: {
                  draft.inputModalities =
                    $0 == "unknown" ? nil : $0 == "image" ? ["text", "image"] : ["text"]
                }
              )
            ) {
              Text("尚未确认").tag("unknown")
              Text("仅文本").tag("text")
              if ["chat", "responses", "anthropic", "gemini"].contains(draft.protocolName) {
                Text("文本与图片").tag("image")
              } else if draft.inputModalities?.contains("image") == true {
                Text("文本与图片（当前协议不支持）").tag("image")
              }
            }.accessibilityIdentifier("model.input-modalities")
            Text("按服务实际支持的输入填写。含图片的聊天不会静默丢弃图片后切换到文本模型。")
              .font(.caption).foregroundStyle(.secondary)
          }
          Section("请求选项") {
            Button {
              page = "budget"
            } label: {
              LabeledContent(
                "上下文预算",
                value: draft.contextRange.map { "\($0.selected) / 最大 \($0.maximum)  ›" }
                  ?? "能力待确认  ›")
            }.buttonStyle(.plain)
            TextField("最大输出 tokens", value: $draft.maxOutput, format: .number)
              .accessibilityIdentifier("model.output")
            Text("请按服务部署能力设置输出限制；当前不将未知窗口显示为已确认能力。").font(.caption).foregroundStyle(.secondary)
            Toggle(
              "自定义温度",
              isOn: Binding(
                get: { draft.temperature != nil }, set: { draft.temperature = $0 ? 1 : nil })
            ).disabled(draft.protocolName == "anthropic")
            if draft.temperature != nil {
              TextField(
                "温度（0–2）",
                value: Binding(get: { draft.temperature ?? 1 }, set: { draft.temperature = $0 }),
                format: .number
              )
              .accessibilityIdentifier("model.temperature")
            }
            Picker(
              "推理强度",
              selection: Binding(
                get: { draft.reasoningEffort ?? "default" },
                set: { draft.reasoningEffort = $0 == "default" ? nil : $0 })
            ) {
              Text("服务商默认").tag("default")
              if draft.reasoningCapabilities?.contains("none") == true { Text("关闭").tag("none") }
              if draft.reasoningCapabilities?.contains("levels") == true {
                Text("低").tag("low")
                Text("中").tag("medium")
                Text("高").tag("high")
              }
              if draft.reasoningCapabilities?.contains("xhigh") == true
                && ["chat", "responses"].contains(draft.protocolName)
              {
                Text("xhigh").tag("xhigh")
              }
            }.accessibilityIdentifier("model.reasoningEffort")
            DisclosureGroup("推理能力声明") {
              capability("允许关闭推理", id: "none")
              capability("支持低／中／高强度", id: "levels")
              capability("支持 xhigh（Chat／Responses）", id: "xhigh").disabled(
                !["chat", "responses"].contains(draft.protocolName))
              Text("按实际服务能力填写；能力声明不代表已完成真实模型验证。").font(.caption).foregroundStyle(.secondary)
            }
            Toggle(
              "关闭模型思考（兼容服务选项）",
              isOn: Binding(
                get: { draft.enableThinking == false },
                set: { draft.enableThinking = $0 ? false : nil })
            ).disabled(draft.protocolName != "chat")
            LabeledContent("上下文压缩", value: draft.compactionStrategy ?? "跟随全局策略")
            Text("不支持的参数不会发送。保存后从下一回合生效。").font(.caption).foregroundStyle(.secondary)
          }
        }
        if page == "budget" {
          Section("上下文能力与预算") {
            if let capability = draft.contextLimit {
              LabeledContent("模型声明窗口", value: "\(capability) tokens")
              Text(draft.contextSource ?? "服务能力信息").font(.caption).foregroundStyle(.secondary)
            }
            DisclosureGroup("部署能力与协议余量", isExpanded: $deploymentVisible) {
              TextField(
                "部署窗口上限（0 表示未知）",
                value: Binding(
                  get: { draft.deploymentLimit ?? 0 },
                  set: {
                    draft.deploymentLimit = $0 > 0 ? $0 : nil
                    draft.contextBudget = nil
                  }), format: .number
              )
              .accessibilityIdentifier("model.contextLimit")
              TextField(
                "部署最大输出（0 表示未知）",
                value: Binding(
                  get: { draft.outputLimit ?? 0 }, set: { draft.outputLimit = $0 > 0 ? $0 : nil }),
                format: .number)
              TextField(
                "协议与工具余量",
                value: Binding(
                  get: { draft.contextReserve ?? 512 },
                  set: {
                    draft.contextReserve = max(0, $0)
                    draft.contextBudget = nil
                  }), format: .number)
              Text("手动填写的是部署声明，不会标记为已实测的服务能力。").font(.caption).foregroundStyle(.secondary)
            }
            if let range = draft.contextEditorRange {
              LabeledContent("上下文预算", value: "\(range.selected) tokens")
              if range.maximum > range.minimum {
                Slider(
                  value: Binding(
                    get: { Double(draft.contextEditorRange?.selected ?? range.recommended) },
                    set: { setBudget(Int($0.rounded())) }),
                  in: Double(range.minimum)...Double(range.maximum), step: 1
                )
                .accessibilityIdentifier("model.contextBudget")
              }
              HStack {
                Text("\(range.minimum)")
                Spacer()
                Button("使用建议值 \(range.recommended)") {
                  setBudget(Int(range.recommended))
                }
                Spacer()
                Text("\(range.maximum)")
              }.font(.caption)
              TextField(
                "精确预算值",
                value: Binding(
                  get: { Int(draft.contextEditorRange?.selected ?? range.selected) },
                  set: { setBudget($0) }), format: .number)
                .accessibilityIdentifier("model.contextBudget.precise")
              if !budgetNotice.isEmpty { Text(budgetNotice).font(.caption).foregroundStyle(.secondary) }
              Text(
                "已预留 \(range.output) 输出与 \(draft.contextReserve ?? 512) 协议余量。窗口上限取模型与部署限制的较小值。当前输入计量采用保守 UTF-8 字节估算，并非厂商精确 tokenizer。"
              ).font(.caption).foregroundStyle(.secondary)
              LabeledContent(
                "预算分配",
                value:
                  "输入 \(range.input_budget) + 输出 \(range.output) + 余量 \(draft.contextReserve ?? 512) = \(range.selected)"
              )
            } else {
              Text(
                draft.maxOutput <= 0 ? "最大输出必须大于 0，请先填写有效的输出限制。" :
                (draft.deploymentLimit ?? draft.contextLimit ?? 0) > 0
                  ? "输出与预留已超过可用窗口，请先降低输出限制。" : "窗口能力未知。填写部署上限或测试服务返回的能力后，才能使用有上限的预算滑条。"
              )
              .font(.caption).foregroundStyle(.secondary)
              HStack {
                Button("填写部署能力…") { deploymentVisible = true }
                if store.profiles.contains(where: { $0.id == draft.id }) {
                  Button("重新检查服务能力") { store.testConnection(profile: draft.id) }
                    .disabled(
                      draft.endpoint != original.endpoint || draft.model != original.model
                        || draft.protocolName != original.protocolName || store.checkingConnection)
                }
              }
            }
            TextField("最大输出 tokens", value: $draft.maxOutput, format: .number)
              .accessibilityIdentifier("model.outputBudget")
            Picker(
              "压缩策略",
              selection: Binding(
                get: { draft.compactionStrategy ?? "global" },
                set: { draft.compactionStrategy = $0 == "global" ? nil : $0 })
            ) {
              Text("跟随统一配置").tag("global")
              Text("语义摘要 + 近期原文").tag("semantic")
              Text("仅保留近期原文").tag("recent")
            }
          }
        }
      }.formStyle(.grouped)
      if !warning.isEmpty { Text(warning).font(.callout).foregroundStyle(.orange) }
      if !error.isEmpty { Text(error).foregroundStyle(.red) }
      HStack {
        if store.profiles.contains(where: { $0.id == draft.id }) {
          Button("删除模型…", role: .destructive) { remove() }.disabled(hasActiveChat)
        }
        Spacer()
        Button("取消", action: onClose)
        Button("保存") { save() }.buttonStyle(.borderedProminent).accessibilityIdentifier(
          "model.save"
        ).disabled(store.checkingConnection || draft.maxOutput <= 0
          || (((draft.contextLimit ?? 0) > 0 || (draft.deploymentLimit ?? 0) > 0) && draft.contextRange == nil))
      }
    }.frame(maxWidth: .infinity, minHeight: 530).disabled(store.configSaving).onAppear(
      perform: {
        ownerEpoch = store.identityEpoch
        normalizeBudget()
        if draft.protocolName != "chat", draft.enableThinking != nil || draft.chatTemplate != nil {
          draft.enableThinking = nil
          draft.chatTemplate = nil
          warning = "旧配置包含当前协议不支持的兼容选项。保存后将移除这些选项；尚未发送测试或聊天请求。"
        }
        if draft.chatTemplate?.isEmpty == true { draft.chatTemplate = nil }
        if page == "connection" { loadSecret() }
      }
    )
    .onChange(of: draft.endpoint) { _, _ in clearCapability() }
    .onChange(of: draft.model) { _, _ in clearCapability() }
    .onChange(of: draft.protocolName) { _, _ in clearCapability() }
    .onChange(of: store.identityEpoch) { _, _ in onClose() }
    .onChange(of: draft.authentication) { _, _ in loadSecret() }
    .onChange(of: page) { _, next in if next == "connection" && !credentialLoaded { loadSecret() } }
    .onChange(of: [draft.contextLimit ?? 0, draft.deploymentLimit ?? 0, draft.outputLimit ?? 0,
      draft.advertisedOutputLimit ?? 0, draft.maxOutput, draft.contextReserve ?? 512]) { _, _ in normalizeBudget() }
    .onChange(of: store.profiles) { old, models in
      guard let before = old.first(where: { $0.id == draft.id }),
        let after = models.first(where: { $0.id == draft.id }),
        draft.endpoint == before.endpoint, draft.model == before.model,
        draft.protocolName == before.protocolName
      else { return }
      if draft.contextLimit == before.contextLimit { draft.contextLimit = after.contextLimit }
      if draft.contextSource == before.contextSource { draft.contextSource = after.contextSource }
      if draft.advertisedOutputLimit == before.advertisedOutputLimit {
        draft.advertisedOutputLimit = after.advertisedOutputLimit
      }
      if draft.maxOutput == before.maxOutput { draft.maxOutput = after.maxOutput }
      if draft.contextBudget == before.contextBudget { draft.contextBudget = after.contextBudget }
    }
  }
  private func setBudget(_ value: Int) {
    guard let range = draft.contextEditorRange, draft.setEditorContextBudget(value) else { return }
    budgetNotice = draft.contextBudget == value ? "" : "预算已调整到有效范围 \(range.minimum)…\(range.maximum) 内。"
  }
  private func normalizeBudget() {
    guard let choice = draft.contextBudget, choice != 0,
      let range = draft.contextEditorRange, Int(range.selected) != choice else { return }
    setBudget(choice)
  }
  private func capability(_ title: String, id: String) -> some View {
    Toggle(
      title,
      isOn: Binding(
        get: { draft.reasoningCapabilities?.contains(id) == true },
        set: { enabled in
          var values = Set(draft.reasoningCapabilities ?? [])
          if enabled {
            values.insert(id)
          } else {
            values.remove(id)
            draft.reasoningEffort = nil
          }
          draft.reasoningCapabilities =
            values.isEmpty ? nil : ["none", "levels", "xhigh"].filter { values.contains($0) }
        }))
  }
  private func clearCapability() {
    draft.contextLimit = nil
    draft.advertisedOutputLimit = nil
    draft.contextSource = nil
    draft.contextBudget = nil
    draft.deploymentLimit = nil
    draft.outputLimit = nil
    draft.inputModalities = nil
    draft.reasoningSupported = nil
    draft.reasoningCapabilities = nil
    draft.reasoningEffort = nil
    draft.temperature = nil
    if draft.protocolName != "chat" {
      draft.enableThinking = nil
      draft.chatTemplate = nil
    }
  }
  private func loadSecret() {
    if draft.authentication == "none" {
      credentialLoaded = true
      secret = ""
      originalSecret = nil
      return
    }
    if draft.providerID != nil {
      credentialLoaded = true
      secret = ""
      originalSecret = nil
      return
    }
    switch readCredential(draft.keychainID) {
    case .success(let saved):
      originalSecret = saved
      secret = saved ?? ""
      credentialLoaded = true
      error = ""
    case .failure(let failure):
      credentialLoaded = false
      error = failure.localizedDescription
    }
  }
  private func remove() {
    guard ownerEpoch == store.identityEpoch, !store.busy, !store.configSaving else {
      error = "身份已变化或配置正在使用，请重新打开后操作。"
      return
    }
    guard !store.checkingConnection else {
      error = "正在检查模型连接，请等待完成后删除。"
      return
    }
    guard !hasActiveChat else {
      error = "这个模型仍有未完成的回合，请先恢复并结算。"
      return
    }
    let alert = NSAlert()
    alert.messageText = "删除这个模型配置？"
    alert.informativeText = "聊天记录保留，模型密钥会从钥匙串移除。"
    alert.addButton(withTitle: "删除")
    alert.addButton(withTitle: "取消")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    guard alert.runModal() == .alertFirstButtonReturn else { return }
    let remaining = store.profiles.filter { $0.id != draft.id }
    store.saveConfiguration(models: remaining, providers: store.providers) { ok, message in
      guard ok else {
        error = message
        return
      }
      if draft.providerID == nil
        && !remaining.contains(where: { $0.keychainID == draft.keychainID })
        && !store.providers.contains(where: { $0.keychainID == draft.keychainID }),
        !saveCredential(draft.keychainID, "")
      {
        store.error = "模型已删除，钥匙串凭据未能移除，请手动核对。"
      }
      store.connectionStatus = ""
      onClose()
    }
  }
  private func save() {
    guard ownerEpoch == store.identityEpoch, !store.busy, !store.configSaving else {
      error = "身份已变化或配置正在使用，请重新打开后操作。"
      return
    }
    guard !store.checkingConnection else {
      error = "正在检查模型连接，请等待完成后保存。"
      return
    }
    guard ["chat", "responses", "anthropic", "ollama", "gemini"].contains(draft.protocolName),
      !draft.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
      !draft.model.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
      let u = URL(string: draft.endpoint), ["http", "https"].contains(u.scheme ?? ""),
      u.host != nil, u.user == nil, u.password == nil, u.query == nil, draft.maxOutput > 0,
      draft.maxOutput <= min(draft.effectiveOutputLimit, 65536)
    else {
      error = "请检查模型名称、服务地址、模型 ID 与输出限制。"
      return
    }
    if ((draft.contextLimit ?? 0) > 0 || (draft.deploymentLimit ?? 0) > 0)
      && draft.contextRange == nil
    {
      error = "上下文预算无效：必须容纳输出预留与协议余量，且不能超过模型或部署上限。"
      return
    }
    let keyChanged = draft.providerID == nil && credentialLoaded && secret != (originalSecret ?? "")
    var comparable = draft
    comparable.name = original.name
    if hasActiveChat && (comparable != original || keyChanged) {
      error = "这个模型仍有未完成的回合。可更改显示名称；连接、预算与凭据需要在回合结束后修改。"
      return
    }
    var staged: String?
    if keyChanged {
      let keyID = UUID().uuidString
      guard (draft.credentialRevision ?? 0) < Int.max, saveCredential(keyID, secret) else {
        error = "无法保存密钥到钥匙串"
        return
      }
      staged = keyID
      draft.credentialID = keyID
      draft.credentialRevision = (draft.credentialRevision ?? 0) + 1
      if secret.isEmpty { draft.keyEnvironment = nil }
    }
    var models = store.profiles
    if let i = models.firstIndex(where: { $0.id == draft.id }) {
      models[i] = draft
    } else {
      models.append(draft)
    }
    store.saveConfiguration(models: models, providers: store.providers) { ok, message in
      if ok {
        store.connectionStatus = ""
        onClose()
      } else {
        error = message
        if let staged {
          let removed = saveCredential(staged, "")
          if !removed { error += " 暂存凭据未能清理，请在钥匙串中核对。" }
        }
      }
    }
  }
}
