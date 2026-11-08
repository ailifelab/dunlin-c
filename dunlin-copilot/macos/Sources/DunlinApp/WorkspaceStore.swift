import AppKit
import CDesktop
import Security
import SwiftUI

// Opaque C references cross queues as values; all dereferences occur on queue.
final class NativeStoreReference: @unchecked Sendable {
  let raw: OpaquePointer
  init(raw: OpaquePointer) { self.raw = raw }
  deinit { dc_close(raw) }
}
final class NativeIdentityReference: @unchecked Sendable {
  let raw: OpaquePointer
  init(raw: OpaquePointer) { self.raw = raw }
  deinit { dc_identity_close(raw) }
}
// Callbacks remain alive until dc_run returns; cancellation has its own lock.
private struct NativeHooksReference: @unchecked Sendable { let value: dc_hooks }
@MainActor final class WorkspaceStore: ObservableObject {
  @Published var chats: [Chat] = []
  @Published var selected: String?
  private var preparedSelection: (epoch: Int64, chat: String?)?
  @Published var messages: [Message] = []
  @Published var messagesLoading = false
  @Published var messageReadError: String?
  @Published var chatCatalogLoaded = false
  @Published var creatingChat = false
  var messageRequestID = UUID()
  var projectContentEpoch = UUID()
  @Published var jumpToMessage: Int?
  @Published var remembering: MemorySelection?
  @Published var draft = ""
  @Published var pendingAttachment: ChatAttachment?
  @Published var attachmentsLoaded = false
  @Published var attachmentBusy = false
  @Published var attachmentError = ""
  @Published var attachmentSelection: AttachmentSelection?
  var attachmentRequestID = UUID()
  var screenshotCapture: ScreenshotCapture?
  @Published var live = ""
  @Published var liveReasoning = ""
  @Published var liveReasoningKind = "reasoning"
  @Published var liveModelName: String?
  var chatStreamCursor = ChatStreamCursor()
  @Published var agentJobs: [AgentJob] = []
  var agentRequestID = UUID()
  var agentProgressReadAt = Date.distantPast
  @Published var agentHasOlderJobs = false
  @Published var agentQueueRunning = false
  var agentQueueProject = ""
  @Published var agentLiveText:[String:String] = [:]
  @Published var agentLiveTruncated:Set<String> = []
  @Published var agentSourceRevision:[String:Int] = [:]
  @Published var agentRunningIDs:Set<String> = []
  var agentWorkers:[String:Callbacks] = [:]
  var agentQueueReadToken=UUID()
  var agentWorkerProjects:[String:String] = [:]
  var agentApprovalRequests:[AgentApprovalRequest] = []
  var agentApprovalPresenting = false
  var agentActiveApproval:(AgentApprovalRequest,NSAlert)?
  @Published var agentLoading = false
  @Published var agentError = ""
  @Published var activeAgentID: String?
  @Published var mediaPreferences = MediaPreferences()
  @Published var mediaVisible = false
  @Published var mediaVideo = false
  @Published var mediaPreview: Data?
  @Published var mediaStatus = "准备通话"
  @Published var mediaError = ""
  @Published var mediaCallID: String?
  @Published var mediaStarted: Date?
  @Published var mediaCalls: [MediaCallRecord] = []
  @Published var mediaTranscripts: [MediaTranscript] = []
  var mediaTranscriptRequestID = UUID()
  var mediaEngine: MediaCallEngine?
  var mediaLastFrame: TimeInterval = -10
  var mediaStartedUptime: TimeInterval = 0
  var mediaFramePending = false
  var mediaFrameFailed = false
  @Published var mediaRecordWarning = ""
  var mediaStartToken = UUID()
  var mediaStarting = false
  @Published var busy = false
  @Published var stopping = false
  @Published var error = ""
  @Published var activity: [String] = []
  @Published var projects: [ProjectRecord] = []
  @Published var route = "chat"
  @Published var selectedProject: String?
  @Published var editingProject: ProjectRecord?
  @Published var projectSaving = false
  @Published var projectPage = "chats"
  @Published var projectReferences: [ProjectReferenceState] = []
  @Published var selectedProjectReferences: [String: String] = [:]
  @Published var referencePreviews: [String: ProjectDocument] = [:]
  var projectReferenceRequestID = UUID()
  @Published var projectEditorError = ""
  @Published var projectFiles: [ProjectFile] = []
  @Published var fileDirectory = ""
  @Published var openedDocument: ProjectDocument?
  @Published var skillScope: SkillScope?
  @Published var search = ""
  @Published var hits: [SearchHit] = []
  @Published var profiles: [ModelProfile] = []
  @Published var providers: [ProviderProfile] = []
  @Published var configImportPlan: ConfigImportPlan?
  @Published var configSaving = false
  @Published var configurationReady = false
  var configRevision: Int64 = 0
  @Published var profileID = ""
  @Published var modelSwitchPlan: ModelSwitchReview?
  @Published var modelSwitchError = ""
  @Published var switchingModel = false
  var modelSwitchRequestID = UUID()
  @Published var workspaceReview: ChatWorkspaceReview?
  @Published var workspacePlanning = false
  @Published var workspaceSaving = false
  @Published var workspaceReviewError = ""
  @Published var defaultProfileID = ""
  @Published var theme = "system"
  @Published var pluginPages: [PluginPage] = []
  @Published var pluginClients: [PluginClient] = []
  @Published var pluginFaultNotice = ""
  @Published var settingsPluginLogs = false
  var extensionHealthTimer:Timer?
  var extensionHealthEpoch:Int64?
  var extensionHealthReading=false
  var extensionLastFault:Int64?

  @Published var pluginPageID = ""
  @Published var installedThemes: [InstalledTheme] = []
  @Published var showReasoning = false
  @Published var generalPreferences = GeneralPreferences()
  @Published var notificationStatus = ""
  @Published var notificationPending = false
  var notificationRequestID = UUID()
  @Published var contextPreferences = ContextPreferences()
  @Published var knowledgePreferences = KnowledgePreferences()
  @Published var searchVisible = false
  @Published var inspectorVisible = true
  @Published var changes: [FileChange] = []
  @Published var grants: [GrantRecord] = []
  @Published var selectedChange: FileChange?
  @Published var receiptRows: [FileReceipt] = []
  @Published var receiptMore = false
  @Published var receiptError = ""
  @Published var selectedReceipt: FileReceiptSelection?
  @Published var conversationPage = "chat"
  @Published var toolFocusSequence: Int?
  @Published var trajectoryRunFocus: String?
  @Published var trajectoryCallFocus: String?
  var receiptCursor: Int64 = 0
  var receiptRequestID = UUID()
  var grantRequestID = UUID()
  @Published var settingsSection = "通用"
  @Published var connectionStatus = ""
  @Published var checkingConnection = false
  @Published var connectionTests: [ProviderConnectionTest] = []
  @Published var modelIssues: [ModelConfigIssue] = []
  @Published var indexWarning = ""
  @Published var indexStatus: HistoryIndexStatus?
  @Published var identities: [DesktopIdentity] = []
  var identityCatalog: NativeIdentityReference?
  var identityEpoch: Int64 = 0
  private var runWorkspace = ""
  private var runChat = ""
  var ordinaryRunningChat:String? {busy && agentWorkers.isEmpty && mediaCallID == nil ? runChat:nil}
  var drafts: [String: String] = [:]
  var draftSave: DispatchWorkItem?
  var currentGrants: [GrantRecord] { grants.filter { $0.chat == selected } }
  var workspaceTransitioning: Bool { workspacePlanning || workspaceSaving || workspaceReview != nil }
  var selectedChat: Chat? { chats.first { $0.id == selected } }
  var requiresModelSelection: Bool {
    guard let chat = selectedChat else { return false }
    return !profiles.contains(where: { $0.id == chat.model })
  }
  var canSendMessage: Bool {
    !busy && !creatingChat && !switchingModel && !checkingConnection && !configSaving
      && !workspacePlanning && !workspaceSaving && workspaceReview == nil
      && configurationReady
      && handle != nil
      && selected != nil && selectedChat?.active != true
      && profiles.contains(where: { $0.id == profileID })
      && !requiresModelSelection && !messagesLoading && messageReadError == nil
      && attachmentsLoaded && !attachmentInteraction
      && (!draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || pendingAttachment != nil)
  }
  var isProject: Bool { selectedChat.map { !$0.project.isEmpty } ?? false }
  var currentProject: ProjectRecord? {
    guard var project = projects.first(where: {
      $0.id
        == (["project", "projectSettings"].contains(route)
          ? selectedProject : selectedChat?.project)
    }) else { return nil }
    if route == "chat", let chat = selectedChat, let available = chat.workspaceAvailable {
      project.path = chat.workspace
      project.device = chat.workspaceDevice ?? ""
      project.inode = chat.workspaceInode ?? ""
      project.available = available
      project.availability = chat.workspaceAvailability ?? "工作区状态尚未确认"
    }
    return project
  }
  let queue = DispatchQueue(label: "org.dunlin.desktop.runtime", qos: .utility)
  var handle: NativeStoreReference?
  private var callbacks: Callbacks?
  var directory: URL
  let rootDirectory: URL
  init() {
    let args = ProcessInfo.processInfo.arguments
    if let i = args.firstIndex(of: "--data-dir"), i + 1 < args.count {
      directory = URL(fileURLWithPath: args[i + 1], isDirectory: true)
    } else if let acceptance = Bundle.main.object(
      forInfoDictionaryKey: "DunlinAcceptanceDataDirectory") as? String
    {
      directory = URL(fileURLWithPath: acceptance, isDirectory: true)
    } else {
      directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[
        0
      ].appendingPathComponent("Dunlin C", isDirectory: true)
    }
    rootDirectory = directory
    do {
      try FileManager.default.createDirectory(
        at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
      try FileManager.default.setAttributes(
        [.posixPermissions: 0o700], ofItemAtPath: directory.path)
    } catch { self.error = error.localizedDescription }
    if let pointer = dc_identity_open(rootDirectory.path) {
      identityCatalog = NativeIdentityReference(raw: pointer)
      if let raw = dc_identity_request(pointer, "{\"op\":\"list\"}") {
        let result = try? JSONDecoder().decode(
          WorkspaceReply<[DesktopIdentity]>.self, from: Data(String(cString: raw).utf8))
        dc_free(raw)
        identities = result?.data ?? []
        if let current = identities.first(where: { $0.selected }) {
          directory = URL(fileURLWithPath: current.directory, isDirectory: true)
          identityEpoch = current.epoch
        }
      }
    } else {
      self.error = "身份目录无法打开，请确认没有另一份客户端正在使用它。"
    }
    profiles = []
    profileID = ""
    theme = "system"
    if let data = try? Data(contentsOf: directory.appendingPathComponent("settings.json")),
      let saved = try? JSONDecoder().decode(DesktopSettings.self, from: data)
    {
      profileID = saved.profileID
      defaultProfileID = saved.defaultProfileID ?? saved.profileID
      theme = saved.theme
      settingsSection = saved.settingsSection ?? "通用"
      showReasoning = saved.showReasoning
      generalPreferences = saved.general ?? GeneralPreferences()
      mediaPreferences = saved.media ?? MediaPreferences()
      contextPreferences = saved.context ?? ContextPreferences()
      knowledgePreferences = saved.knowledge ?? KnowledgePreferences()
    }
    let path = directory.path
    let principal = currentIdentity?.id ?? "local"
    queue.async {
      let h = dc_open(path).flatMap { raw -> NativeStoreReference? in
        guard dc_bind_identity(raw, principal) != 0 else {
          dc_close(raw)
          return nil
        }
        _ = dc_configure_platform_grants(raw, Bundle.main.bundleIdentifier ?? "org.dunlin.copilot")
        return NativeStoreReference(raw: raw)
      }
      let registry = h.flatMap { self.loadConfiguration($0) }
      DispatchQueue.main.async {
        self.handle = h
        if h == nil {
          self.error = "无法打开本地数据。请确认没有另一份 Dunlin C 正在运行。"
        } else {
          if let registry {
            self.acceptConfiguration(registry)
          } else {
            self.error = "模型配置无法载入，原始设置保留。请先检查本地配置。"
          }
          self.registerMemorySubjects()
          self.configureExtensionAssets()
          self.reloadProjects();self.reload()
          self.prepareIndex()
        }
      }
    }
  }
  func saveSettings() {
    do {
      let settings = DesktopSettings(
        profileID: profileID, theme: theme,
        showReasoning: showReasoning, context: contextPreferences,
        defaultProfileID: defaultProfileID, knowledge: knowledgePreferences,
        general: generalPreferences, media: mediaPreferences,settingsSection:settingsSection)
      let file = directory.appendingPathComponent("settings.json")
      try JSONEncoder().encode(settings).write(to: file, options: .atomic)
      try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
    } catch { self.error = error.localizedDescription }
  }

  nonisolated func decode<T: Decodable>(_ pointer: UnsafeMutablePointer<CChar>?, as: T.Type)
    -> T?
  {
    guard let pointer else { return nil }
    defer { dc_free(pointer) }
    return try? JSONDecoder().decode(T.self, from: Data(String(cString: pointer).utf8))
  }
  func reload() {
    guard let h = handle else { return }
    let epoch = identityEpoch
    queue.async {
      let rows = self.decode(dc_chats(h.raw), as: [Chat].self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        guard let rows else {
          self.error = "聊天目录暂时不可读，原数据保留。"
          return
        }
        let priorChat = self.selectedChat
        self.chats = rows
        if let priorChat, let current = self.selectedChat,
          priorChat.project != current.project || priorChat.workspace != current.workspace
            || priorChat.workspaceRevision != current.workspaceRevision
            || priorChat.workspaceAvailable != current.workspaceAvailable
        {
          self.invalidateProjectContent()
          if let project = self.currentProject { self.reloadProjectReferences(project) }
        }
        self.chatCatalogLoaded = true
        if self.selected == nil && self.route == "chat" {
          self.selected = rows.first(where: { !$0.archived })?.id
          if self.selected == nil && !self.profiles.isEmpty && !self.creatingChat && !self.busy {
            self.newChat()
          }
        }
        self.loadMessages()
      }
    }
  }
  func loadMessages() {
    guard !busy, let h = handle, let id = selected else { return }
    let epoch = identityEpoch
    let stamp = chatStreamCursor.stamp
    let request = UUID()
    messageRequestID = request
    messagesLoading = true
    messageReadError = nil
    let dataDirectory = directory
    queue.async {
      let rows = self.decode(dc_messages(h.raw, id), as: [Message].self)
      let file = dataDirectory.appendingPathComponent("changes").appendingPathComponent(
        id + ".json")
      let changes =
        (try? Data(contentsOf: file)).flatMap {
          try? JSONDecoder().decode([FileChange].self, from: $0)
        } ?? []
      DispatchQueue.main.async {
        if self.selected == id && self.identityEpoch == epoch
          && self.chatStreamCursor.stamp == stamp && !self.busy && self.messageRequestID == request
        {
          self.messagesLoading = false
          if let rows {
            self.messages = rows
            self.changes = changes
          } else {
            self.error = "聊天记录暂时不可读，原始数据保留。请先核对恢复状态。"
            self.messageReadError = self.error
          }
        }
      }
    }
  }
  func newChat(workspace: String = "") {
    guard !busy, !creatingChat, !switchingModel, !workspaceTransitioning, !attachmentInteraction, let h = handle else { return }
    let epoch = identityEpoch
    if let selected { persistDraft(selected) }
    let model =
      projects.first(where: { $0.path == workspace }).flatMap { p in
        profiles.contains(where: { $0.id == p.model }) ? p.model : nil
      } ?? defaultProfileID
    let id = UUID().uuidString
    let root =
      workspace.isEmpty ? directory.appendingPathComponent("Chat Workspace").path : workspace
    if !workspace.isEmpty, let p = projects.first(where: { $0.path == workspace }), !p.available {
      error = "项目目录不可用，请在项目属性中重新关联。"
      return
    }
    do {
      if workspace.isEmpty {
        try FileManager.default.createDirectory(
          atPath: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
      }
    } catch {
      self.error = error.localizedDescription
      return
    }
    creatingChat = true
    queue.async {
      let ok = dc_add_chat(h.raw, id, workspace.isEmpty ? "新聊天" : "项目聊天", root)
      var modelSaved = false
      if ok != 0,
        let data = try? JSONSerialization.data(withJSONObject: [
          "op": "chats.update", "id": id, "field": "model", "value": model,
        ]), let request = String(data: data, encoding: .utf8)
      {
        modelSaved =
          self.decode(dc_workspace_request(h.raw, request), as: WorkspaceReply<NoData>.self)?.ok
          == true
      }
      let saved = modelSaved
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        self.creatingChat = false
        if ok != 0 {
          self.route = "chat"
          self.selected = id
          self.profileID = model
          self.messages = []
          self.reload()
          self.reloadProjects()
          if !saved { self.error = "聊天已创建，但默认模型未保存，请重新选择模型。" }
        } else {
          self.error = "创建聊天失败"
        }
      }
    }
  }
  func addProject() {
    guard !busy else { return }
    projectEditorError = ""
    editingProject = ProjectRecord()
  }
  func removeChat(_ chat: Chat) {
    guard !busy, let h = handle else { return }
    let alert = NSAlert()
    alert.messageText = "删除这段聊天？"
    alert.informativeText = "聊天与检索缓存会删除。已修改的项目文件和已执行命令的效果会保留。"
    alert.addButton(withTitle: "删除")
    alert.addButton(withTitle: "取消")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    guard alert.runModal() == .alertFirstButtonReturn else { return }
    let epoch = identityEpoch
    let dataDirectory = directory
    queue.async {
      let ok = dc_delete_chat(h.raw, chat.id)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        if ok != 0 {
          self.chats.removeAll { $0.id == chat.id }
          self.hits.removeAll { $0.chat == chat.id }
          self.drafts.removeValue(forKey: chat.id)
          try? FileManager.default.removeItem(
            at: dataDirectory.appendingPathComponent("changes").appendingPathComponent(
              chat.id + ".json"))
          if self.selected == chat.id {
            self.selected = nil
            self.messages = []
          }
          self.reload()
        } else {
          self.error = "删除失败"
        }
      }
    }
  }
  func prepareIndex() {
    guard let h = handle else { return }
    let epoch = identityEpoch
    queue.async {
      let more = dc_index_pending(h.raw, 16)
      if more > 0 {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
          if self.identityEpoch == epoch { self.prepareIndex() }
        }
      } else if more < 0 {
        DispatchQueue.main.async {
          guard self.identityEpoch == epoch else { return }
          self.indexWarning = "本地索引整理未完成；原始聊天已保留，可在聊天检索设置中查看和重试。"
          self.reloadIndexStatus()
        }
      } else {
        DispatchQueue.main.async {
          guard self.identityEpoch == epoch else { return }
          self.indexWarning = ""
          self.reloadIndexStatus()
        }
      }
    }
  }
  func find() {
    guard !busy, let h = handle else { return }
    let query = search
    let epoch = identityEpoch
    queue.async {
      let rows = self.decode(dc_search(h.raw, query), as: [SearchHit].self)
      DispatchQueue.main.async {
        if self.identityEpoch == epoch && self.search == query {
          self.hits = rows ?? []
          if rows == nil { self.error = "搜索失败，请稍后重试。" }
        }
      }
    }
  }
  func send() {
    guard canSendMessage else { return }
    run(input: draft)
  }
  func resume() { run(input: nil) }
  func run(input: String?, agent: AgentJob? = nil) {
    guard !busy, !switchingModel, !checkingConnection, !configSaving, configurationReady,
      !workspacePlanning, !workspaceSaving, workspaceReview == nil,
      let h = handle,
      let id = selected,
      let p = profiles.first(where: { $0.id == profileID })
    else { return }
    if selectedChat?.unsupportedCheckpoint == true {
      error = "此回合使用不再支持的存档格式，请新建聊天；原记录可查看或删除。"
      return
    }
    if ((p.contextLimit ?? 0) > 0 || (p.deploymentLimit ?? 0) > 0) && p.contextRange == nil {
      error = "模型上下文预算与输出预留不匹配，请在模型设置中调整。"
      return
    }
    jumpToMessage = nil
    draftSave?.cancel()
    if let input, agent == nil {
      persistDraft(id)
      drafts[id] = ""
      draft = ""
      _ = input
    }
    activeAgentID = agent?.id
    busy = true
    stopping = false
    messagesLoading = false
    live = ""
    liveReasoning = ""
    liveReasoningKind = "reasoning"
    liveModelName = nil
    let uiRun = chatStreamCursor.start(after: messages.compactMap(\.sequence).max() ?? -1)
    let runEpoch = identityEpoch
    activity = []
    runWorkspace = selectedChat?.workspace ?? ""
    runChat = id
    error = ""
    saveSettings()
    let box = Callbacks()
    callbacks = box
    box.event = { name, text in
      DispatchQueue.main.async {
        guard self.chatStreamCursor.run == uiRun, self.identityEpoch == runEpoch else { return }
        if name == "model.request" {
          _ = self.chatStreamCursor.requested(uiRun)
          self.live = ""
          self.liveReasoning = ""
          self.liveModelName = nil
          self.liveReasoningKind = "reasoning"
          self.liveModelName =
            (text.data(using: .utf8).flatMap {
              (try? JSONSerialization.jsonObject(with: $0)) as? [String: Any]
            })?["model"] as? String
        } else if name == "agent.child.progress" {
          if Date().timeIntervalSince(self.agentProgressReadAt)>0.5 {
            self.agentProgressReadAt = Date();self.reloadAgentJobs()
          }
        } else if name == "delta" {
          self.live += text
        } else if name == "reasoning.delta" || name == "reasoning.summary" {
          self.liveReasoning += text
          self.liveReasoningKind = name == "reasoning.summary" ? "summary" : "reasoning"
        } else if name == "chat.commit",
          let data = text.data(using: .utf8),
          let commit = try? JSONDecoder().decode(ChatCommit.self, from: data), commit.chat == id,
          let ticket = self.chatStreamCursor.ticket(
            uiRun, through: commit.through, settlesModel: commit.settlesModel)
        {
          self.readLocalView(
            ["op": "chat.messages", "chat": id, "through": commit.through], as: [Message].self
          ) { reply in
            guard self.busy, self.selected == id, reply.ok, let rows = reply.data,
              let settle = self.chatStreamCursor.apply(ticket)
            else { return }
            self.messages = rows
            if settle {
              self.live = ""
              self.liveReasoning = ""
              self.liveModelName = nil
            }
          }
        } else if [
          "tool.start", "tool.result", "exec.result", "context.trimmed", "context.compacting",
          "context.compacted", "knowledge.context",
        ].contains(name) {
          self.activity.append(name + ": " + text)
        }
      }
    }
    box.approve = { tool, args in DispatchQueue.main.sync { self.reviewTool(tool, args) } }
    let context = Unmanaged.passUnretained(box).toOpaque()
    let hooks = dc_hooks(
      user: context,
      event: { ctx, name, text in
        guard let ctx, let name, let text else { return }
        Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().event(
          String(cString: name), String(cString: text))
      },
      approve: { ctx, name, args in
        guard let ctx, let name, let args else { return 0 }
        if Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().isCancelled() { return 0 }
        let tool = String(cString: name)
        let body = String(cString: args)
        return Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().approve(tool, body)
      },
      cancelled: { ctx in
        guard let ctx else { return 1 }
        return Unmanaged<Callbacks>.fromOpaque(ctx).takeUnretainedValue().isCancelled() ? 1 : 0
      })
    let nativeHooks = NativeHooksReference(value: hooks)
    let selfRunTools = isProject ? (currentProject?.policy ?? "ask") : "noTools"
    let explicitReferences =
      isProject
      ? selectedProjectReferences.keys.sorted().map {
        ["path": $0, "sha256": selectedProjectReferences[$0]!]
      } : []
    let projectTurn = isProject
    let contextPolicy = contextPreferences
    let knowledgePolicy = knowledgePreferences
    let configurationRevision = configRevision
    let attachmentID = input == nil ? nil : pendingAttachment?.id
    queue.async {
      let mapped = self.coreModelSettings(h, id: p.id, revision: configurationRevision)
      guard var settings = mapped.data else {
        DispatchQueue.main.async {
          self.busy = false
          self.stopping = false
          self.callbacks = nil
          self.live = ""
          self.liveReasoning = ""
          self.liveModelName = nil
          self.chatStreamCursor.finish()
          self.activeAgentID = nil
          self.agentQueueRunning = false
          self.error = self.configurationError(mapped.error)
          if agent == nil && self.draft.isEmpty { self.draft = input ?? "" }
        }
        return
      }
      let credential = modelCredential(p)
      guard case .success(let secret) = credential else {
        DispatchQueue.main.async {
          self.busy = false
          self.stopping = false
          self.callbacks = nil
          self.live = ""
          self.liveReasoning = ""
          self.liveModelName = nil
          self.chatStreamCursor.finish()
          self.activeAgentID = nil
          self.agentQueueRunning = false
          if case .failure(let error) = credential { self.error = error.localizedDescription }
          if agent == nil && self.draft.isEmpty { self.draft = input ?? "" }
        }
        return
      }
      settings["stream"] = true
      // Public-text delivery stays stable across local display toggles, so a
      // visibility preference never changes the frozen continuation identity.
      settings["show_reasoning"] = true
      settings["desktop_tools"] = selfRunTools
      if projectTurn { settings["desktop_project_context"] = explicitReferences }
      if knowledgePolicy.memory || knowledgePolicy.rag {
        settings["desktop_knowledge"] = [
          "memory": knowledgePolicy.memory, "rag": knowledgePolicy.rag,
        ]
      }
      if let budget = p.contextRange {
        settings["max_tokens"] = Int(budget.output)
        settings["desktop_context"] = [
          "modelLimit": p.contextLimit ?? 0, "deploymentLimit": p.deploymentLimit ?? 0,
          "selectedLimit": Int(budget.selected), "outputLimit": p.effectiveOutputLimit,
          "requestedOutput": p.maxOutput, "reserve": p.contextReserve ?? 512,
          "strategy": p.compactionStrategy ?? contextPolicy.strategy,
          "automatic": contextPolicy.automatic,
          "threshold": contextPolicy.threshold, "retainTurns": contextPolicy.retainTurns,
          "summaryOutput": min(contextPolicy.summaryOutput, Int(budget.output)),
        ]
      }
      let json =
        String(
          data: (try? JSONSerialization.data(withJSONObject: settings, options: .sortedKeys))
            ?? Data(), encoding: .utf8) ?? "{}"
      var observer = nativeHooks.value
      var inputCommitted: Int32 = 0
      let raw: UnsafeMutablePointer<CChar>? = {
        if let task = agent { return dc_agent_execute(h.raw, task.id, json, secret, &observer) }
        return input.map { value in
          value.withCString { text in
            if let attachmentID {
              return attachmentID.withCString {
                dc_run_message_with_secret(h.raw, id, json, text, &observer, secret, $0, &inputCommitted)
              }
            }
            return dc_run_message_with_secret(h.raw, id, json, text, &observer, secret, nil, &inputCommitted)
          }
        }
        ?? dc_run_message_with_secret(h.raw, id, json, nil, &observer, secret, nil, &inputCommitted)
      }()
      let acceptedInput = inputCommitted != 0
      var result: [String: String] = [:]
      if let raw {
        result =
          (try? JSONDecoder().decode([String: String].self, from: Data(String(cString: raw).utf8)))
          ?? [:]
        dc_free(raw)
      }
      DispatchQueue.main.async {
        self.busy = false
        self.stopping = false
        self.live = ""
        self.liveReasoning = ""
        self.liveModelName = nil
        self.chatStreamCursor.finish()
        self.callbacks = nil
        if result["status"] != "completed" {
          switch result["status"] {
          case "denied": self.error = "本次工具请求未获授权。已完成的操作保留，可在工具记录中查看。"
          case "busy": self.error = "未完成的回合绑定原模型，请切回原配置后继续。"
          case "unknown": self.error = "执行结果尚未确认，请先核对工具记录，避免重复执行。"
          case "timeout": self.error = "请求超时。已完成的操作保留，请查看工具记录。"
          case "cancelled": self.error = "本次请求已停止，聊天与已完成的操作保留。"
          default: self.error = "请求未完成：" + (result["text"] ?? "请检查连接后重试")
          }
          if agent == nil && !acceptedInput && self.draft.isEmpty { self.draft = input ?? "" }
        } else {
          self.selectedProjectReferences = [:]
        }
        self.loadMessages()
        self.reloadPendingAttachment()
        self.persistDraft(id)
        self.reload()
        self.reloadProjects()
        self.prepareIndex()
        self.reloadGrants()
        self.reloadReceipts()
        self.notifyCompletion(run: uiRun, succeeded: result["status"] == "completed")
        self.activeAgentID = nil
        if agent != nil {
          if result["status"] != "completed" { self.agentQueueRunning = false }
          self.reloadAgentJobs(startNext: self.agentQueueRunning)
        }
      }
    }
  }
  func stop() {
    if !agentWorkers.isEmpty {
      agentQueueRunning=false;stopping=true
      for worker in agentWorkers.values {worker.cancel()}
      return
    }
    if mediaCallID != nil {endMediaCall();return}
    agentQueueRunning = false
    guard busy, let callbacks else { return }
    stopping = true
    callbacks.cancel()
  }
  func reviewTool(_ tool: String, _ arguments: String) -> Int32 {
    let alert = NSAlert()
    alert.messageText = ["write_file", "patch_file"].contains(tool) ? "允许修改这个文件？" : "允许执行这条命令？"
    alert.informativeText = "项目：\(runWorkspace)\n授权仅适用于本次调用。"
    let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 620, height: 220))
    let text = NSTextView(frame: scroll.bounds)
    text.isEditable = false
    text.isSelectable = true
    text.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    text.string = arguments
    scroll.documentView = text
    scroll.hasVerticalScroller = true
    alert.accessoryView = scroll
    alert.addButton(withTitle: "仅允许这一次")
    alert.addButton(withTitle: "拒绝")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    alert.buttons[0].setAccessibilityIdentifier("approval.allowOnce")
    alert.buttons[1].setAccessibilityIdentifier("approval.deny")
    let allowed = alert.runModal() == .alertFirstButtonReturn
    grants.append(GrantRecord(chat: runChat, tool: tool, arguments: arguments, allowed: allowed))
    return allowed ? 1 : 0
  }
  func renameChat(_ chat: Chat) {
    guard !busy, let h = handle else { return }
    let a = NSAlert()
    a.messageText = "重命名聊天"
    let field = NSTextField(string: chat.title)
    field.setAccessibilityIdentifier("chat.rename.input")
    field.frame = NSRect(x: 0, y: 0, width: 320, height: 26)
    a.accessoryView = field
    a.addButton(withTitle: "保存")
    a.addButton(withTitle: "取消")
    guard a.runModal() == .alertFirstButtonReturn else { return }
    let title = field.stringValue
    queue.async {
      let ok = dc_rename_chat(h.raw, chat.id, title)
      DispatchQueue.main.async { if ok != 0 { self.reload() } else { self.error = "重命名失败" } }
    }
  }
  func importModel() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = false
    panel.allowsMultipleSelection = false
    guard panel.runModal() == .OK, let url = panel.url else { return }
    do {
      let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
      guard let bytes = attributes[.size] as? NSNumber, bytes.intValue <= 1_048_576 else {
        error = "配置文件最多 1 MiB。"
        return
      }
      let data = try Data(contentsOf: url)
      guard data.count <= 1_048_576, let source = String(data: data, encoding: .utf8) else {
        error = "请选择有效 UTF-8 静态 JSON／YAML 配置。"
        return
      }
      previewConfiguration(source)
    } catch { self.error = error.localizedDescription }
  }

  func testConnection(profile: String? = nil) {
    guard !busy, !checkingConnection, !configSaving, configurationReady,
      let p = profiles.first(where: { $0.id == (profile ?? defaultProfileID) })
    else { return }
    let config: [String: Any] = [
      "base_url": p.endpoint, "model": p.model, "protocol": p.protocolName,
      "desktop_auth": p.authentication ?? "auto",
    ]
    guard let data = try? JSONSerialization.data(withJSONObject: config),
      let raw = String(data: data, encoding: .utf8)
    else { return }
    checkingConnection = true
    connectionStatus = "正在检查…"
    let epoch = identityEpoch
    queue.async {
      let credential = modelCredential(p)
      guard case .success(let secret) = credential else {
        DispatchQueue.main.async {
          if self.identityEpoch == epoch {
            self.checkingConnection = false
            self.connectionStatus = "未能读取模型凭据，未发送检查请求。"
            if case .failure(let error) = credential {
              self.connectionStatus = error.localizedDescription
            }
          }
        }
        return
      }
      let result = self.decode(dc_probe_model(raw, secret, nil), as: ModelProbe.self)
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        self.checkingConnection = false
        guard let result else {
          self.connectionStatus = "连接检查未返回有效结果"
          return
        }
        guard self.profiles.contains(p) else {
          self.connectionStatus = "配置已变化，请重新测试"
          return
        }
        if !result.ok {
          switch result.httpStatus {
          case 401, 403: self.connectionStatus = "认证未通过（HTTP \(result.httpStatus)），请检查模型密钥。"
          case 429: self.connectionStatus = "服务正在限流，请稍后重试。"
          case 300...399: self.connectionStatus = "服务返回重定向，未转发密钥；请填写最终 API 地址。"
          default:
            self.connectionStatus =
              result.httpStatus > 0 ? "服务返回 HTTP \(result.httpStatus)" : "无法连接服务，请检查地址和服务状态。"
          }
          return
        }
        self.connectionStatus =
          result.listKnown
          ? (result.modelFound ? "连接正常 · 已找到模型" : "连接正常 · 列表中未找到此模型 ID")
          : "HTTP 可达 · 模型目录格式未知，可保存后通过聊天验证。"
        if result.modelFound {
          if self.chats.contains(where: { $0.active && $0.model == p.id }) {
            self.connectionStatus += "；能力数据将在未完成回合结算后重新检查并应用。"
            return
          }
          self.applyModelCapabilities(
            p.id, contextLimit: result.contextLimit, outputLimit: result.outputLimit,
            source: result.source
          ) { ok, message in
            if !ok { self.connectionStatus += "；能力信息未保存：\(message)" }
          }
        }
      }
    }
  }

  @discardableResult
  func openChat(_ id: String, sequence: Int? = nil) -> Bool {
    guard !busy, !creatingChat, !switchingModel, !workspacePlanning, !workspaceSaving,
      !attachmentInteraction else { return false }
    guard chats.contains(where: { $0.id == id }) else {
      error = "找不到这段聊天，请刷新列表后重试。原有来源记录仍保留。"
      return false
    }
    // Selection alone has no change event when returning to the same chat
    // from a project page, or jumping from that chat's tool/trajectory page.
    let sameChat = selected == id
    conversationPage = "chat"
    jumpToMessage = sequence
    searchVisible = false
    error = ""
    route = "chat"
    selected = id
    if sameChat { loadMessages() }
    return true
  }

  func selectChat(from _: String?, to new: String?) {
    guard selected == new else { return }
    if let context = preparedSelection, context.epoch == identityEpoch, context.chat == new {
      return
    }
    let outgoing = preparedSelection?.epoch == identityEpoch ? preparedSelection?.chat : nil
    preparedSelection = (identityEpoch, new)
    resetAttachments()
    messageRequestID = UUID()
    messagesLoading = false
    messageReadError = nil
    conversationPage = "chat"
    toolFocusSequence = nil
    trajectoryRunFocus = nil
    trajectoryCallFocus = nil
    invalidateProjectContent()
    if let outgoing, chats.contains(where: { $0.id == outgoing }) { persistDraft(outgoing) }
    if new != nil { route = "chat" }
    draft = new.flatMap { drafts[$0] } ?? ""
    error = ""
    changes = []
    messages = []
    receiptRows = []
    receiptMore = false
    receiptError = ""
    selectedReceipt = nil
    receiptRequestID = UUID()
    loadMessages()
    reloadPendingAttachment()
    reloadReceipts()
    if let new { reloadGrants(chat: new) }
    if let new { restoreChatMetadata(new) }
    if let project = currentProject { reloadProjectReferences(project) }
  }

}
