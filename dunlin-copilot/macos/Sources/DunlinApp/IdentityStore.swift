import AppKit
import CDesktop
import SwiftUI

extension WorkspaceStore {
  var currentIdentity: DesktopIdentity? { identities.first(where: { $0.selected }) }
  func identityRequest(_ body: [String: Any]) {
    guard !busy, let catalog = identityCatalog,
      let data = try? JSONSerialization.data(withJSONObject: body),
      let raw = String(data: data, encoding: .utf8)
    else { return }
    queue.async {
      let result = self.decode(
        dc_identity_request(catalog.raw, raw), as: WorkspaceReply<[DesktopIdentity]>.self)
      DispatchQueue.main.async {
        if let result, result.ok {
          self.identities = result.data ?? []
        } else {
          self.error = "身份操作未完成：" + (result?.error ?? "存储错误")
        }
      }
    }
  }
  func createIdentity(name: String, shared: Bool, members: [String]) {
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !trimmed.isEmpty else { return }
    identityRequest([
      "op": "create", "id": UUID().uuidString, "name": trimmed,
      "kind": shared ? "shared" : "person", "members": shared ? members : [],
    ])
  }
  func switchIdentity(_ identity: DesktopIdentity) {
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
        self.changes = []
        self.grants = []
        self.hits = []
        self.jumpToMessage = nil
        self.remembering = nil
        self.search = ""
        self.searchVisible = false
        self.route = "chat"
        self.openedDocument = nil
        self.projectFiles = []
        self.error = ""
        self.skillScope = nil
        self.installedThemes = []
        self.receiptRows = []
        self.receiptMore = false
        self.receiptError = ""
        self.selectedReceipt = nil
        self.receiptRequestID = UUID()
        self.connectionStatus = ""
        self.checkingConnection = false
        self.indexStatus = nil
        self.indexWarning = ""
        self.configImportPlan = nil
        self.configSaving = false
        self.acceptConfiguration(registry)
        self.conversationPage = "chat"
        self.toolFocusSequence = nil
        self.trajectoryRunFocus = nil
        self.trajectoryCallFocus = nil
        self.profileID =
          self.profiles.contains(where: { $0.id == saved?.profileID })
          ? saved!.profileID : (self.profiles.first?.id ?? "")
        self.modelSwitchPlan = nil
        self.modelSwitchError = ""
        self.modelSwitchRequestID = UUID()
        self.defaultProfileID =
          self.profiles.contains(where: { $0.id == saved?.defaultProfileID })
          ? saved!.defaultProfileID! : self.profileID
        self.theme = saved?.theme ?? "system"
        self.settingsSection = saved?.settingsSection ?? "通用"
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
  @State private var name = ""
  @State private var shared = false
  @State private var members: Set<String> = []
  var body: some View {
    VStack(alignment: .leading, spacing: 22) {
      Text("不同身份使用独立的聊天、项目、设置和本地记忆。共享身份只使用明确存入它的内容。").foregroundStyle(.secondary)
      ForEach(store.identities.filter { !$0.hidden }) { identity in
        GroupBox {
          HStack(spacing: 14) {
            Image(systemName: identity.kind == "shared" ? "person.2.circle" : "person.crop.circle")
              .font(.title2)
            VStack(alignment: .leading, spacing: 5) {
              Text(identity.name).fontWeight(.semibold)
              Text(identity.kind == "shared" ? "共享身份 · \(identity.members.count) 位成员" : "个人身份")
                .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if identity.selected {
              Text("当前身份").foregroundStyle(.secondary)
            } else {
              Button("切换") { store.switchIdentity(identity) }.accessibilityIdentifier(
                "identity.switch.\(identity.id)")
              if identity.id != "local" {
                Button("移除") { store.identityRequest(["op": "remove", "id": identity.id]) }
              }
            }
          }.padding(10)
        }
      }
      GroupBox("添加身份") {
        VStack(alignment: .leading, spacing: 14) {
          TextField("身份名称", text: $name).textFieldStyle(.roundedBorder).accessibilityIdentifier(
            "identity.name")
          Toggle("家庭或团队共享身份", isOn: $shared)
          if shared {
            ForEach(store.identities.filter { !$0.hidden && $0.kind == "person" }) { member in
              Toggle(
                member.name,
                isOn: Binding(
                  get: { members.contains(member.id) },
                  set: { if $0 { members.insert(member.id) } else { members.remove(member.id) } }))
            }
          }
          Button("添加身份") {
            store.createIdentity(name: name, shared: shared, members: Array(members).sorted())
            name = ""
          }.disabled(name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            .accessibilityIdentifier("identity.create")
        }.padding(12)
      }
      if store.identities.contains(where: { $0.hidden }) {
        DisclosureGroup("已移除的身份") {
          ForEach(store.identities.filter { $0.hidden }) { identity in
            HStack {
              Text(identity.name)
              Spacer()
              Button("恢复") { store.identityRequest(["op": "restore", "id": identity.id]) }
            }
          }
        }
      }
      Text("身份切换由本机用户操作，不是模型推断出的身份，也不替代系统账户的登录保护。移除后可恢复。").font(.caption).foregroundStyle(
        .secondary)
      if !store.error.isEmpty { Text(store.error).foregroundStyle(.red) }
    }
  }
}
