import AppKit
import CDesktop
import SwiftUI

private struct IdentityTransitionSheet: View {
  let source: String
  let target: DesktopIdentity
  let activity: String
  let finish: (Bool) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      Text("切换到" + target.name).font(.title2.weight(.semibold))
      Text(target.kind == "shared" ? "接下来使用家庭共享的聊天、资料与记忆。" : "接下来使用目标身份的聊天、资料与记忆。")
        .foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 10) {
        Label("当前空间还有活动", systemImage: "clock").font(.headline)
        Text(activity + "将请求停止并等待状态确认。")
        Text("草稿、历史与授权记录保留在" + source + "的空间。")
      }.font(.callout).padding(20).frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 10))
      Text(target.kind == "shared" ? "家庭成员只能看到你明确共享的内容。" : "不同身份的聊天和记忆分别保存。")
        .foregroundStyle(.secondary)
      HStack {
        Spacer()
        Button("留在当前空间") { finish(false) }.keyboardShortcut(.cancelAction)
        Button("停止活动并切换") { finish(true) }.buttonStyle(.borderedProminent)
      }.padding(.top, 12)
    }.padding(28).frame(width: 650)
  }
}

extension WorkspaceStore {
  var currentIdentity: DesktopIdentity? { identities.first(where: { $0.selected }) }
  func identityRequest(_ body: [String: Any], completion: @escaping (Bool) -> Void = { _ in }) {
    guard !busy, let catalog = identityCatalog,
      let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8)
    else { completion(false); return }
    let epoch = identityEpoch
    queue.async {
      let result = self.decode(
        dc_identity_request(catalog.raw, raw), as: WorkspaceReply<[DesktopIdentity]>.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { completion(false); return }
        if let result, result.ok {
          self.identities = result.data ?? []
          completion(true)
        } else {
          self.error = "身份操作未完成：" + (result?.error ?? "存储错误")
          completion(false)
        }
      }
    }
  }
  func createIdentity(name: String, shared: Bool, members: [String], completion: @escaping (Bool) -> Void = { _ in }) {
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.isEmpty else { completion(false); return }
    identityRequest([
      "op": "create", "id": UUID().uuidString, "name": trimmed,
      "kind": shared ? "shared" : "person", "members": shared ? members : [],
    ], completion: completion)
  }
  var canRequestIdentitySwitch: Bool {
    pendingIdentitySwitch == nil && !creatingChat && !switchingModel && !projectSaving
      && !configSaving && !checkingConnection && !workspaceTransitioning && !attachmentInteraction
      && (!busy || callbacks != nil || !agentWorkers.isEmpty || !helperWorkers.isEmpty || agentQueueRunning)
  }

  func switchIdentity(_ identity: DesktopIdentity) {
    guard canRequestIdentitySwitch, !identity.selected, !identity.hidden else { return }
    guard busy else { selectSettledIdentity(identity); return }
    guard let window = NSApp.keyWindow ?? NSApp.mainWindow
      ?? NSApp.orderedWindows.first(where: { $0.isVisible && $0.canBecomeKey && $0.sheetParent == nil })
    else { return }
    let epoch = identityEpoch
    let workers = agentWorkers.count + helperWorkers.count
    let activity: String
    if workers > 0 {
      activity = "\(workers) 个 Agent 任务仍在运行。"
    } else if compactingChat != nil {
      activity = "正在整理聊天上下文。"
    } else if agentQueueRunning {
      activity = "协作队列正在调度任务。"
    } else {
      activity = "当前聊天正在执行。"
    }
    let sheet = NSWindow(contentRect: .zero, styleMask: [.titled, .fullSizeContentView],
                         backing: .buffered, defer: false)
    sheet.title = "切换到" + identity.name
    sheet.titleVisibility = .hidden
    sheet.titlebarAppearsTransparent = true
    let content = NSHostingView(rootView: IdentityTransitionSheet(
      source: currentIdentity?.name ?? "原身份", target: identity, activity: activity
    ) { [weak window, weak sheet] proceed in
      guard let window, let sheet else { return }
      window.endSheet(sheet, returnCode: proceed ? .OK : .cancel)
    })
    sheet.contentView = content
    sheet.setContentSize(content.fittingSize)
    window.beginSheet(sheet) { reply in
      guard reply == .OK, self.identityEpoch == epoch,
        self.canRequestIdentitySwitch else { return }
      if self.busy {
        self.pendingIdentitySwitch = (identity, epoch)
        self.agentQueueRunning = false
        self.stop()
      } else {
        self.selectSettledIdentity(identity)
      }
    }
  }

  func finishPendingIdentitySwitch() {
    guard let pending = pendingIdentitySwitch, !busy, callbacks == nil,
      agentWorkers.isEmpty, helperWorkers.isEmpty else { return }
    pendingIdentitySwitch = nil
    if identityEpoch == pending.epoch { selectSettledIdentity(pending.target) }
  }

  func selectSettledIdentity(_ identity: DesktopIdentity, openMemory: Bool = false) {
    guard !busy, !creatingChat, !switchingModel, !projectSaving, !configSaving, !checkingConnection,
      !workspacePlanning, !workspaceSaving, workspaceReview == nil, !identity.selected,
      !attachmentInteraction, !identity.hidden, let catalog = identityCatalog
    else { return }
    if let selected { persistDraft(selected) }
    saveSettings()
    busy = true
    let epoch = identityEpoch
    let path = identity.directory
    queue.async {
      guard let rawStore = dc_open(path) else {
        DispatchQueue.main.async {
          self.busy = false
          self.error = "无法打开目标身份，已保留当前身份。"
        }
        return
      }
      guard dc_bind_identity(rawStore, identity.id) != 0 else {
        dc_close(rawStore)
        DispatchQueue.main.async {
          self.busy = false
          self.error = "目标数据与身份绑定不一致，已保留当前身份。"
        }
        return
      }
      _ = dc_configure_platform_grants(rawStore, Bundle.main.bundleIdentifier ?? "org.dunlin.copilot")
      let next = NativeStoreReference(raw: rawStore)
      let data = try? Data(
        contentsOf: URL(fileURLWithPath: path).appendingPathComponent("settings.json"))
      let saved = data.flatMap { try? JSONDecoder().decode(DesktopSettings.self, from: $0) }
      guard let registry = self.loadConfiguration(next) else {
        DispatchQueue.main.async {
          self.busy = false
          self.error = "目标身份的模型配置无法载入，当前身份保留。"
        }
        return
      }
      let request = "{\"op\":\"select\",\"id\":\"\(identity.id)\",\"epoch\":\(epoch)}"
      let result = self.decode(
        dc_identity_request(catalog.raw, request), as: WorkspaceReply<[DesktopIdentity]>.self)
      guard let result, result.ok, let rows = result.data else {
        DispatchQueue.main.async {
          self.busy = false
          self.error = "身份已变化，请重试。当前聊天保留。"
        }
        return
      }
      DispatchQueue.main.async {
        self.handle = next
        self.directory = URL(fileURLWithPath: path, isDirectory: true)
        self.identities = rows
        self.identityEpoch = rows.first(where: { $0.selected })?.epoch ?? epoch + 1
        self.resetAttachments()
        self.selected = nil
        self.selectedProject = nil
        self.editingProject = nil
        self.projects = []
        self.chats = []
        self.agentRequestID = UUID()
        self.agentQueueReadToken = UUID()
        self.agentSourceRevision = [:]
        self.helperReading = false
        self.agentLiveText = [:]
        self.agentLiveTruncated = []
        self.agentHasOlderJobs = false
        self.agentJobs = []
        self.agentQueueRunning = false
        self.agentQueueProject = ""
        self.agentLoading = false
        self.agentError = ""
        self.activeAgentID = nil
        self.messages = []
        self.messagesLoading = false
        self.messageReadError = nil
        self.messageRequestID = UUID()
        self.projectContentEpoch = UUID()
        self.workspaceReview = nil
        self.workspacePlanning = false
        self.workspaceSaving = false
        self.workspaceReviewError = ""
        self.chatCatalogLoaded = false
        self.stopping = false
        self.drafts = [:]
        self.projectReferences = []
        self.selectedProjectReferences = [:]
        self.referencePreviews = [:]
        self.projectReferenceRequestID = UUID()
        self.projectPage = "chats"
        self.draft = ""
        self.live = ""
        self.liveReasoning = ""
        self.liveModelName = nil
        self.chatStreamCursor.finish()
        self.activity = []
        self.compactionIssueChat = nil
        self.failedTurn = nil
        self.reincludeSelections = [:]
        self.projectEditorError = ""
        self.fileDirectory = ""
        self.pluginFaultNotice = ""
        self.settingsPluginLogs = false
        self.grants = []
        self.hits = []
        self.jumpToMessage = nil
        self.remembering = nil
        self.searchProject = ""; self.searchDays = 0; self.searchedQuery = nil; self.searchFailure = nil
        self.search = ""
        self.route = "chat"
        self.openedDocument = nil
        self.projectFiles = []
        self.error = ""
        self.skillScope = nil
        self.installedThemes = []
        self.pluginPresentations = []
        self.receiptRows = []
        self.receiptMore = false
        self.receiptError = ""
        self.cleanupChat = nil
        self.selectedReceipt = nil
        self.receiptRequestID = UUID()
        self.connectionStatus = ""
        self.checkingConnection = false
        self.indexStatus = nil
        self.indexWarning = ""
        self.resetImportReview()
        self.configSaving = false
        self.acceptConfiguration(registry)
        self.conversationPage = "chat"
        self.toolFocusSequence = nil
        self.trajectoryRunFocus = nil
        self.trajectoryCallFocus = nil
        let savedModel = saved?.profileID ?? ""
        self.profileID = savedModel.isEmpty || self.profiles.contains(where: { $0.id == savedModel })
          ? savedModel : (self.profiles.first?.id ?? "")
        self.modelSwitchPlan = nil
        self.modelSwitchError = ""
        self.modelSwitchRequestID = UUID()
        let savedDefault = saved?.defaultProfileID ?? self.profileID
        self.defaultProfileID = savedDefault.isEmpty || self.profiles.contains(where: { $0.id == savedDefault })
          ? savedDefault : self.profileID
        self.theme = saved?.theme ?? "system"
        self.settingsSection = openMemory ? "记忆与知识库" : (saved?.settingsSection ?? "通用")
        self.knowledgePage = openMemory ? .memory : .overview
        self.showReasoning = saved?.showReasoning ?? false
        self.generalPreferences = saved?.general ?? GeneralPreferences()
        self.mediaPreferences = saved?.media ?? MediaPreferences()
        self.pluginPages = []; self.pluginClients = []; self.pluginPageID = ""
        self.mediaCalls = []; self.mediaTranscripts = []; self.mediaError = ""; self.mediaStatus = "准备通话"
        self.notificationStatus = ""
        self.notificationPending = false
        self.notificationRequestID = UUID()
        self.contextPreferences = saved?.context ?? ContextPreferences()
        self.knowledgePreferences = saved?.knowledge ?? KnowledgePreferences()
        self.busy = false
        self.registerMemorySubjects()
        self.configureExtensionAssets()
        self.reloadProjects();self.reload()
        self.prepareIndex()
      }
    }
  }
}
struct IdentitiesSettings: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var adding = false

  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(store.identities.filter { !$0.hidden }.sorted { lhs, rhs in
          if lhs.selected != rhs.selected { return lhs.selected }
          if lhs.kind != rhs.kind { return lhs.kind == "person" }
          return lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending
        }) { identity in
          HStack(spacing: 14) {
            Text(String(identity.name.prefix(1)))
              .font(.headline).foregroundStyle(.white)
              .frame(width: 36, height: 36)
              .background(identity.kind == "shared" ? Color.green : identity.selected ? store.themeTint : Color.indigo, in: Circle())
              .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
              Text(identity.name).fontWeight(.semibold)
              Text((identity.kind == "shared" ? "家庭共享空间 · " + String(identity.members.count) + " 位成员" : "个人空间")
                   + (identity.selected ? " · 正在使用" : ""))
                .font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
            if identity.selected {
              Text("当前身份").font(.callout.weight(.medium)).foregroundStyle(store.themeTint)
            } else {
              Button("切换…") { store.switchIdentity(identity) }
                .accessibilityIdentifier("identity.switch.\(identity.id)").disabled(!store.canRequestIdentitySwitch)
            }
          }.frame(minHeight: 66)
            .contextMenu {
              if !identity.selected && identity.id != "local" {
                Button("移除身份") { store.identityRequest(["op": "remove", "id": identity.id]) }
                  .disabled(store.busy)
              }
            }
          Divider()
        }
        Button("添加身份…") { adding = true }.accessibilityIdentifier("identity.add")
          .disabled(store.busy).padding(.vertical, 14)
      }.padding(.horizontal, 20).background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      Text("家庭空间只使用明确共享的聊天和资料。个人记忆保留在各自的空间。")
        .font(.callout).foregroundStyle(.secondary)
      if store.identities.contains(where: { $0.hidden }) {
        DisclosureGroup("已移除的身份") {
          ForEach(store.identities.filter { $0.hidden }) { identity in
            HStack {
              Text(identity.name)
              Spacer()
              Button("恢复") { store.identityRequest(["op": "restore", "id": identity.id]) }.disabled(store.busy)
            }
          }
        }
      }
      if !store.error.isEmpty { Text(store.error).foregroundStyle(.red) }
    }.sheet(isPresented: $adding) { IdentityEditor() }
      .onChange(of: store.identityEpoch) { _, _ in adding = false }
  }
}

private struct IdentityEditor: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @State private var name = ""
  @State private var shared = false
  @State private var members: Set<String> = []
  @State private var saving = false
  @State private var error = ""
  private var trimmed: String { name.trimmingCharacters(in: .whitespacesAndNewlines) }

  var body: some View {
    VStack(alignment: .leading, spacing: 16) {
      Text("添加身份").font(.title2.weight(.semibold))
      TextField("身份名称", text: $name).textFieldStyle(.roundedBorder).accessibilityIdentifier("identity.name")
      Toggle("家庭或团队共享身份", isOn: $shared)
      if shared {
        Text("共享空间成员").font(.headline)
        ScrollView {
          VStack(alignment: .leading, spacing: 8) {
            ForEach(store.identities.filter { !$0.hidden && $0.kind == "person" }) { member in
              Toggle(member.name, isOn: Binding(get: { members.contains(member.id) },
                set: { if $0 { members.insert(member.id) } else { members.remove(member.id) } }))
            }
          }.frame(maxWidth: .infinity, alignment: .leading)
        }.frame(maxHeight: 220)
      }
      if !error.isEmpty { Text(error).foregroundStyle(.red) }
      HStack {
        Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
        Spacer()
        Button("添加身份") {
          guard !saving else { return }
          if store.identities.contains(where: { $0.name == trimmed }) {
            error = "这个名称已经存在，请选择其他名称。"; return
          }
          saving = true
          store.createIdentity(name: trimmed, shared: shared, members: Array(members).sorted()) { ok in
            saving = false
            if ok { dismiss() } else { error = store.error.isEmpty ? "无法保存身份，请稍后重试。" : store.error }
          }
        }.keyboardShortcut(.defaultAction)
          .disabled(store.busy || trimmed.isEmpty || trimmed.utf8.count > 128 || (shared && members.count > 32))
          .accessibilityIdentifier("identity.create")
      }
    }.disabled(saving).interactiveDismissDisabled(saving).padding(24).frame(width: 430)
  }
}
