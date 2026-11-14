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
  @Published var deletingChats = false
  @Published var cleanupChat: Chat?
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
  var helperWorkers:[String:Callbacks] = [:]
  var helperReading=false
  var agentQueueReadToken=UUID()
  var agentWorkerProjects:[String:String] = [:]
  @Published var terminating = false
  var nativeAgentReading = false
  var nativeAgentFailures:Set<String> = []
  var nativeAgentEpoch:Int64 = -1
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
  @Published var knowledgePage: KnowledgePageRoute = .overview
  var pendingIdentitySwitch: (target: DesktopIdentity, epoch: Int64)?
  @Published var busy = false {
    didSet {
      if !busy, pendingIdentitySwitch != nil {
        DispatchQueue.main.async { self.finishPendingIdentitySwitch() }
      }
    }
  }
  @Published var stopping = false
  @Published var compactingChat: String?
  @Published var reincludeSelections: [String:ContextSourceSelection] = [:]
  @Published var compactionIssueChat: String?
  @Published var compactionCancelled = false
  @Published var failedTurn: (chat:String, run:String)?
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
  @Published var search = "" { didSet { if search != oldValue { hits = []; searchedQuery = nil; searchFailure = nil } } }
  @Published var searchProject = ""
  @Published var searchDays = 0
  @Published var searchedQuery: String?
  @Published var searchFailure: String?
  @Published var hits: [SearchHit] = []
  @Published var profiles: [ModelProfile] = []
  @Published var providers: [ProviderProfile] = []
  @Published var configImportPlan: ConfigImportPlan?
  @Published var configImportReview: ConfigImportPlan?
  @Published var configImportFilename = ""
  @Published var configImportIssue = ""
  var configImportRequest = UUID()
  func resetImportReview() {
    configImportRequest = UUID()
    configImportFilename = ""
    configImportIssue = ""
    configImportReview = nil
    configImportPlan = nil
  }
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
  let companionPanel = CompanionPanel()
  @Published var pluginClients: [PluginClient] = []
  @Published var pluginFaultNotice = ""
  @Published var settingsPluginLogs = false
  var extensionHealthTimer:Timer?
  var extensionHealthEpoch:Int64?
  var extensionHealthReading=false
  var extensionLastFault:Int64?

  @Published var pluginPageID = ""
  @Published var installedThemes: [InstalledTheme] = []
  @Published var pluginPresentations: [PluginPresentation] = []
  @Published var showReasoning = false
  @Published var generalPreferences = GeneralPreferences()
  @Published var notificationStatus = ""
  @Published var notificationPending = false
  var notificationRequestID = UUID()
  @Published var contextPreferences = ContextPreferences()
  @Published var knowledgePreferences = KnowledgePreferences()
  @Published var inspectorVisible = true
  @Published var grants: [GrantRecord] = []
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
  var ordinaryRunningChat:String? {busy && callbacks != nil && !runChat.isEmpty && agentWorkers.isEmpty && helperWorkers.isEmpty && mediaCallID == nil ? runChat:nil}
  var drafts: [String: String] = [:]
  var draftSave: DispatchWorkItem?
  var currentGrants: [GrantRecord] { grants.filter { $0.chat == selected } }
  var workspaceTransitioning: Bool { deletingChats || workspacePlanning || workspaceSaving || workspaceReview != nil }
  var selectedChat: Chat? { chats.first { $0.id == selected } }
  var requiresModelSelection: Bool {
    guard let chat = selectedChat else { return false }
    return !profiles.contains(where: { $0.id == chat.model })
  }
  var canSendMessage: Bool {
    !busy && !creatingChat && !switchingModel && !checkingConnection && !configSaving
      && !workspaceTransitioning
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
  private(set) var callbacks: Callbacks?
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
    queue.async {
      let rows = self.decode(dc_messages(h.raw, id), as: [Message].self)
      DispatchQueue.main.async {
        if self.selected == id && self.identityEpoch == epoch
          && self.chatStreamCursor.stamp == stamp && !self.busy && self.messageRequestID == request
        {
          self.messagesLoading = false
          if let rows {
            self.messages = rows
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
    guard !busy, !workspaceTransitioning, handle != nil else { return }
    cleanupChat = chat
  }

  func deleteReviewedChats(_ reviewed: [Chat], preserveMemories: Bool = false, completion: @escaping (Int) -> Void) {
    guard !busy, !workspaceTransitioning, !creatingChat, !attachmentInteraction, let h = handle
    else { completion(0); return }
    let epoch = identityEpoch
    let dataDirectory = directory
    let ids = Set(reviewed.map(\.id)).intersection(chats.map(\.id))
    deletingChats = true
    queue.async {
      let removed = Set(ids.filter { dc_delete_chat_preserving_memories(h.raw, $0, preserveMemories ? 1 : 0) != 0 })
      for id in removed {
        try? FileManager.default.removeItem(at: dataDirectory.appendingPathComponent("changes").appendingPathComponent(id + ".json"))
      }
      DispatchQueue.main.async {
        self.deletingChats = false
        guard self.identityEpoch == epoch else { return }
        self.chats.removeAll { removed.contains($0.id) }
        self.hits.removeAll { removed.contains($0.chat) }
        for id in removed {
          self.drafts.removeValue(forKey: id)
          self.reincludeSelections.removeValue(forKey: id)
        }
        if let selected = self.selected, removed.contains(selected) {
          self.draftSave?.cancel()
          self.draft = ""
          self.selected = nil
          self.messages = []
          if self.chats.isEmpty { self.route = "archive" }
        }
        self.reload()
        self.reloadProjects()
        self.reloadGrants()
        completion(removed.count)
      }
    }
  }

  func prepareIndex() {
    guard let h = handle else { return }
    let epoch = identityEpoch
    queue.async(qos:.utility, flags:.enforceQoS) {
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
    let project = searchProject, days = searchDays
    let since = days == 0 ? 0 : Int64(Date().addingTimeInterval(-Double(days) * 86400).timeIntervalSince1970 * 1000)
    let epoch = identityEpoch
    queue.async {
      let rows = self.decode(dc_search_filtered(h.raw, query, project.isEmpty ? nil : project, since), as: [SearchHit].self)
      DispatchQueue.main.async {
        if self.identityEpoch == epoch && self.search == query && self.searchProject == project && self.searchDays == days {
          self.hits = rows ?? []
          self.searchedQuery = query
          self.searchFailure = rows == nil ? "本地索引暂不可用，请到检索设置检查整理状态。" : nil
        }
      }
    }
  }
  func send() {
    guard canSendMessage else { return }
    run(input: draft)
  }
  func resume() { run(input: nil) }
  func retryFailedTurn() {
    guard let failedTurn, failedTurn.chat == selected else{return}
    run(input:nil,retry:failedTurn.run)
  }
  func run(input: String?, agent: AgentJob? = nil, retry:String? = nil) {
    guard !busy, !switchingModel, !checkingConnection, !configSaving, configurationReady,
      !workspaceTransitioning,
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
    if reincludeSelections[id] != nil && p.contextRange == nil {
      error = "请先配置当前模型的上下文预算，以便核对所选原文能否放入请求。"
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
    compactingChat = nil
    compactionIssueChat = nil
    compactionCancelled = false
    failedTurn = nil
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
          if name == "context.compacting" { self.compactingChat = id }
          if name == "context.compacted" { self.compactingChat = nil }
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
    let sourceSelection = reincludeSelections[id]
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
        var policy = contextPolicy.request(for:p, budget:budget)
        if let sourceSelection { policy["reinclude"] = ["record":sourceSelection.record,"digests":sourceSelection.digests] }
        settings["desktop_context"] = policy
      }
      let json =
        String(
          data: (try? JSONSerialization.data(withJSONObject: settings, options: .sortedKeys))
            ?? Data(), encoding: .utf8) ?? "{}"
      var observer = nativeHooks.value
      var inputCommitted: Int32 = 0
      let raw: UnsafeMutablePointer<CChar>? = {
        if let task = agent { return dc_agent_execute(h.raw, task.id, json, secret, &observer) }
        if let retry {return dc_retry_chat_turn(h.raw,id,json,retry,&observer,secret)}
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
        if result["status"] == "unknown" || result["status"] == nil {
          self.pendingIdentitySwitch = nil
        }
        self.busy = false
        self.stopping = false
        self.live = ""
        self.liveReasoning = ""
        self.liveModelName = nil
        self.chatStreamCursor.finish()
        self.callbacks = nil
        if let token=result["retryRun"], agent == nil {self.failedTurn=(id,token)}
        if self.compactingChat == id && result["status"] != "completed" {
          self.compactionIssueChat = id
          self.compactionCancelled = result["status"] == "cancelled"
        }
        self.compactingChat = nil
        if result["status"] != "completed" {
          switch result["status"] {
          case "denied": self.error = "本次工具请求未获授权。已完成的操作保留，可在工具记录中查看。"
          case "busy": self.error = "未完成的回合绑定原模型，请切回原配置后继续。"
          case "unknown": self.error = "执行结果尚未确认，请先核对工具记录，避免重复执行。"
          case "timeout": self.error = "请求超时。已完成的操作保留，请查看工具记录。"
          case "cancelled": self.error = self.compactionIssueChat == id ? "已取消整理，原始聊天保留。" : "本次请求已停止，聊天与已完成的操作保留。"
          default: self.error = "请求未完成：" + (result["text"] ?? "请检查连接后重试")
          }
          if agent == nil && !acceptedInput && self.draft.isEmpty { self.draft = input ?? "" }
        } else {
          self.reincludeSelections.removeValue(forKey:id)
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
  func cancelCompaction() {
    guard let selected, compactingChat == selected, busy, let callbacks else { return }
    stopping = true
    callbacks.cancel()
  }

  func stop() {
    if !helperWorkers.isEmpty {for worker in helperWorkers.values {worker.cancel()};stopping=true;if agentWorkers.isEmpty {return}}
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
    let alert = toolReviewAlert(tool, arguments)
    let allowed = alert.runModal() == .alertFirstButtonReturn
    grants.append(GrantRecord(chat: runChat, tool: tool, arguments: arguments, allowed: allowed))
    return allowed ? 1 : 0
  }
  func toolReviewAlert(_ tool: String, _ arguments: String) -> NSAlert {
    let alert = NSAlert()
    let fileChange = ["write_file", "patch_file"].contains(tool)
    let values = try? JSONSerialization.jsonObject(with: Data(arguments.utf8)) as? [String: Any]
    let command = values?.count == 1 ? values?["command"] as? String : nil
    alert.messageText = fileChange ? "允许修改这个文件？" : "允许执行这条命令？"
    alert.informativeText = fileChange ? "本次调用可能修改项目文件。" : "本次调用可能读取或写入项目文件，并启动本地进程。"
    let stack = NSStackView()
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 10
    stack.frame = NSRect(x: 0, y: 0, width: 540, height: fileChange ? 295 : 350)
    func label(_ value: String, heading: Bool = false) {
      let field = NSTextField(wrappingLabelWithString: value)
      field.font = heading ? .boldSystemFont(ofSize: 13) : .systemFont(ofSize: 13)
      field.isSelectable = true
      field.preferredMaxLayoutWidth = 535
      stack.addArrangedSubview(field)
    }
    label("工作目录", heading: true)
    label(runWorkspace)
    label(fileChange ? "文件操作参数" : "命令", heading: true)
    let scroll = NSScrollView()
    let text = NSTextView()
    text.isEditable = false
    text.isSelectable = true
    text.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
    text.string = command ?? arguments
    text.textContainerInset = NSSize(width: 6, height: 7)
    text.isVerticallyResizable = true
    text.textContainer?.widthTracksTextView = true
    scroll.documentView = text
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    stack.addArrangedSubview(scroll)
    scroll.widthAnchor.constraint(equalToConstant: 535).isActive = true
    scroll.heightAnchor.constraint(equalToConstant: 130).isActive = true
    label("授权仅用于本次调用及当前参数。")
    if !fileChange { label("停止进程后，已写入的文件会保留。") }
    label("等待审批 · 未开始执行", heading: true)
    alert.accessoryView = stack
    alert.addButton(withTitle: "仅允许这一次")
    alert.addButton(withTitle: "拒绝")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    alert.buttons[0].setAccessibilityIdentifier("approval.allowOnce")
    alert.buttons[1].setAccessibilityIdentifier("approval.deny")
    return alert
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
    previewConfigurationFile(url)
  }

  func previewConfigurationFile(_ url: URL) {
    resetImportReview()
    configImportFilename = url.lastPathComponent
    settingsSection = "导入配置"
    do {
      let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
      guard let bytes = attributes[.size] as? NSNumber, bytes.intValue <= 1_048_576 else {
        configImportIssue = "配置文件最多 1 MiB。"
        return
      }
      let data = try Data(contentsOf: url)
      guard data.count <= 1_048_576, let source = String(data: data, encoding: .utf8) else {
        configImportIssue = "请选择有效 UTF-8 静态 JSON／YAML 配置。"
        return
      }
      previewConfiguration(source, reviewFirst: true)
    } catch { configImportIssue = error.localizedDescription }
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
    guard !busy, !creatingChat, !switchingModel, !workspaceTransitioning,
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
